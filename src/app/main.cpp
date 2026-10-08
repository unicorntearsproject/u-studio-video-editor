#include <adwaita.h>
#include <glib-unix.h>
#include <gtk/gtk.h>

#include <csignal>

#include <cstdlib>
#include <string>

#include "app_window.h"
#include "drop_ins.h"
#include "dropins/registry.h"
#include "settings.h"
#include "snap_env.h"
#include "stall_monitor.h"
#include "core/log.h"
#include "core/trace.h"
#include "engine/factory_policy.h"

namespace ustudio::app {
namespace {

void applyStyle(GtkApplication *)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/com/ustudio/VideoEditor/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// `userData`: the drop-ins' host (main()), whose shell extensions the window
// runs (doc 15 IP5).
void onActivate(GtkApplication *app, gpointer userData)
{
    // "activate" fires more than once per process: G_APPLICATION_DEFAULT_
    // FLAGS makes this app single-instance, so GApplication re-delivers
    // "activate" to THIS already-running primary instance every time
    // something else tries to launch it again (a second double-click, a
    // second terminal invocation, a launcher re-click) instead of starting
    // a new process -- confirmed from a real session's log, two
    // back-to-back "Application activated" lines with no process restart
    // in between. The previous code unconditionally built a brand new
    // AppWindow (a whole new Model/EngineSync/PlaybackController) on every
    // one of those, leaving the earlier window's PlaybackController alive
    // and orphaned (its pointer overwritten in the "ustudio-window" slot,
    // never stopped) -- a second live sdl2_audio consumer fighting the
    // first one over the same PipeWire/audio-device state in the same
    // process. That's the leading suspect for both a real SIGSEGV inside
    // MLT's SDL2 audio callback and "play doesn't work any more" after
    // re-launching once already running: present the existing window
    // instead, the standard GtkApplication pattern for a single-window
    // app.
    if (auto *existing = static_cast<AppWindow *>(g_object_get_data(G_OBJECT(app), "ustudio-window"))) {
        core::Log::info(
            "[app] Application re-activated with a window already open -- presenting it, not creating another");
        gtk_window_present(GTK_WINDOW(existing->widget()));
        return;
    }

    core::Log::info("[app] Application activated");
    stall::install(); // doc 19 MT0: logs main-loop iterations over 16 ms (debug)

    // The Unicorn Tears palette (style.css) is dark-only: under a light
    // system theme libadwaita's own light cards and dialogs showed through
    // it (white Help/Settings lists, 2026-09-24).
    // Marked so the stall monitor names the first iteration's work, not
    // whatever scope ran last (it said "refreshTimeline 0.0 ms").
    {
        {
            core::trace::Scope trace("startup: dark scheme");
            adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
        }
        core::trace::Scope trace("startup: style");
        applyStyle(app);
    }

    // Leaked intentionally: the app has exactly one window for its whole
    // lifetime, and GTK owns/destroys the underlying widget tree on quit.
    // Stashed on `app` (not just leaked) so onShutdown below can reach its
    // PlaybackController and stop the Mlt::Consumer before Factory::close()
    // runs, and so the re-activation check above can find it.
    const auto *dropInHost = static_cast<const dropins::BasicDropInHost *>(userData);
    AppWindow *window = nullptr;
    {
        core::trace::Scope trace("startup: build window");
        window = new AppWindow(app, dropInHost->shellExtensions());
    }
    g_object_set_data(G_OBJECT(app), "ustudio-window", window);
    core::trace::Scope trace("startup: present window");
    gtk_window_present(GTK_WINDOW(window->widget()));
}

// Fires once, synchronously inside g_application_run(), after the main
// loop stops but before it returns -- i.e. still before main()'s
// FactoryPolicy local goes out of scope and calls Mlt::Factory::close().
// This is the hook point that stops the Mlt::Consumer first (its own MLT-
// owned thread(s)), so Factory::close() never runs concurrently with it
// (see PlaybackController::shutdown).
void onShutdown(GtkApplication *app, gpointer /*userData*/)
{
    if (auto *window = static_cast<AppWindow *>(g_object_get_data(G_OBJECT(app), "ustudio-window")))
        window->prepareForShutdown();
}

// SIGTERM (logout, shutdown, `kill`) and SIGINT (Ctrl+C in a terminal):
// quit through GApplication so the normal "shutdown" path runs --
// onShutdown() -> AppWindow::prepareForShutdown(), which writes a final
// autosave if the project is dirty and stops the consumer before
// Factory::close(). Before this, SDL's own handlers swallowed both
// signals (sanitizer report S2, 2026-09-23): see main()'s
// SDL_NO_SIGNAL_HANDLERS comment.
gboolean onQuitSignal(gpointer userData)
{
    core::Log::info("[app] Received SIGTERM/SIGINT -- quitting");
    g_application_quit(G_APPLICATION(userData));
    return G_SOURCE_CONTINUE;
}

} // namespace
} // namespace ustudio::app

int main(int argc, char **argv)
{
    // First: GLib caches the XDG directories on first use (Log::init asks
    // for XDG_STATE_HOME), and GIO loads its modules at first use.
    const std::vector<std::string> scrubbed = ustudio::app::scrubSnapEnvironment();
    ustudio::core::Log::init("u-studio-video-editor");
    if (!scrubbed.empty()) {
        std::string names;
        for (const std::string &name : scrubbed)
            names += (names.empty() ? "" : ", ") + name;
        ustudio::core::Log::info("[app] Launched from a snap's environment; undid " + names);
    }
    const char *envLevel = std::getenv("USTUDIO_LOG_LEVEL");
    ustudio::core::Log::info("[app] Starting U-Stu Video Editor (log level=" +
                             std::string(envLevel ? envLevel
                                         : ustudio::core::Log::defaultLevel() == ustudio::core::LogLevel::Debug
                                             ? "debug (default)"
                                             : "info (default)") +
                             ")");

    // Constructed before any window (and before the first
    // PlaybackController, which never calls Mlt::Factory::init() itself —
    // see factory_policy.h), destroyed after g_application_run() returns:
    // RAII brackets the required init()/close() lifetime automatically.
    // MLT's sdl2_audio consumer initialises SDL, and SDL by default installs
    // SIGINT/SIGTERM handlers that turn both into an SDL_QUIT event -- which
    // nothing in a GTK app ever reads, so the process ignored `kill`,
    // Ctrl+C and session logout until SIGKILLed (sanitizer report S2,
    // 2026-09-23: still running 15 s after SIGTERM by default, exited in
    // 0.1 s with this set). SDL_HINT_NO_SIGNAL_HANDLERS's env name per
    // /usr/include/SDL2/SDL_hints.h. Set before any consumer starts; FALSE
    // so an explicit value in the environment still wins. GLib handles
    // both signals instead (onQuitSignal, registered below).
    g_setenv("SDL_NO_SIGNAL_HANDLERS", "1", FALSE);

    // Drop-ins (ADR-013/014): the built-in ones, then modules from trusted
    // locations; their factory paths before MLT starts (IP4), their
    // registration once it has. Kept for the whole run: modules stay loaded.
    static ustudio::dropins::DropInRegistry dropIns;
    // Settings > Drop-ins: the ones switched off aren't loaded (read once;
    // a change applies from the next start).
    dropIns.setDisabled(ustudio::app::Settings().disabledDropIns());
    dropIns.setKnown(knownDropIns());
    for (const UStudioDropInDescription *builtin : builtinDropIns())
        dropIns.addBuiltin(builtin);
    dropIns.loadModules();
    ustudio::dropins::DropInRegistry::setCurrent(&dropIns);
    ustudio::dropins::FactoryPaths factoryPaths;
    dropIns.contributeFactoryPaths(factoryPaths);

    ustudio::engine::FactoryPolicy factoryPolicy(factoryPaths);
    static ustudio::dropins::BasicDropInHost dropInHost("editor");
    dropIns.registerAll(dropInHost);

    AdwApplication *app = adw_application_new("com.ustudio.VideoEditor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(ustudio::app::onActivate), &dropInHost);
    g_signal_connect(app, "shutdown", G_CALLBACK(ustudio::app::onShutdown), nullptr);
    g_unix_signal_add(SIGTERM, ustudio::app::onQuitSignal, app);
    g_unix_signal_add(SIGINT, ustudio::app::onQuitSignal, app);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    ustudio::core::Log::info("[app] Exiting with status " + std::to_string(status));
    return status;
}
