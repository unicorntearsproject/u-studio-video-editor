#include "app_window.h"
#include "number_row.h"

#include "action_registry.h"
#include "autosave.h"
#include "portal_path.h"
#include "timeline/us_timeline_view.h"
#include "ui_hints.h"
#include "media_badges.h"
#include "core/commands/composite_command.h"
#include "platform/process.h"
#include "core/media/fingerprint.h"
#include "core/media/image_sequence.h"
#include "core/media/missing_media.h"
#include "core/media/utf8_path.h"
#include "core/commands/primitives.h"
#include "core/commands/timeline_edits.h"
#include "core/commands/transaction.h"
#include "core/log.h"
#include "core/trace.h"
#include "engine/audio_sync.h"
#include "core/model/profile_match.h"
#include "core/model/retime.h"
#include "core/xml/backup.h"
#include "dropins/registry.h"
#include "pending_renders.h"
#include "dropins_page.h"
#include "render_profiles_page.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {
// The timeline's geometry. Its colours come from style.css through the
// generated tokens.h (timeline/timeline_renderer.cpp).
constexpr double kTrackRowHeight = 60.0;
constexpr double kHandleWidth = 22.0;
constexpr double kEdgeGrabWidth = 8.0;
constexpr double kDragClickThreshold = 3.0; // below this, a "drag" is really just a click
constexpr int kHoverPreviewWidth = 240;     // the hover preview's width in pixels
constexpr guint kHoverPreviewDelayMs = 300; // how long the pointer rests before it shows
// A slim strip at the top of each track row for the track's name; clips
// draw below it.
constexpr double kTrackLabelHeight = 14.0;
constexpr double kRulerHeight = 20.0;

// GTK's modifier state as the timeline controller's own flags.
timeline::Modifiers timelineModifiers(GtkEventController *controller)
{
    GdkModifierType state = gtk_event_controller_get_current_event_state(controller);
    timeline::Modifiers mods = timeline::Modifiers::None;
    if (state & GDK_SHIFT_MASK)
        mods = mods | timeline::Modifiers::Shift;
    if (state & GDK_CONTROL_MASK)
        mods = mods | timeline::Modifiers::Ctrl;
    if (state & GDK_ALT_MASK)
        mods = mods | timeline::Modifiers::Alt;
    return mods;
}

// Media-browser "length" column: an asset's own native duration, not
// tied to any sequence's fps -- "H:MM:SS", or "M:SS" under an hour.
std::string formatMediaLength(double seconds)
{
    if (seconds <= 0.0)
        return "—";
    int total = static_cast<int>(seconds + 0.5);
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int secs = total % 60;
    char buf[32];
    if (hours > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, secs);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", minutes, secs);
    return buf;
}

// Media-browser "fps" column: 30, 25, 23.976, 29.97 (core::formatFps).
std::string formatMediaFps(core::Rational fps)
{
    return core::formatFps(fps);
}

// `length` is the caller's ADJUSTED length (still images get resized to
// cover the current timeline, or a 10s default -- see the call site),
// deliberately separate from `probed.length` (the raw probe result,
// meaningless for a still image beyond "MLT's pixbuf default").
core::Asset makeImportedAsset(const std::string &path, core::FrameIndex length,
                              const engine::EngineSync::ProbedMedia &probed, const core::Rational &sequenceFps)
{
    core::Asset asset;
    asset.path = path;
    auto slash = path.find_last_of('/');
    asset.displayName = (slash == std::string::npos) ? path : path.substr(slash + 1);
    asset.info.hasVideo = true;
    asset.info.hasAudio = probed.hasAudio;
    asset.info.isStillImage = probed.isStillImage;
    asset.info.hasAlpha = probed.hasAlpha;
    asset.info.lengthInSequenceFrames = length;
    asset.info.fps = probed.fps;
    asset.info.width = probed.width;
    asset.info.height = probed.height;
    asset.fileFingerprint = probed.fingerprint;
    asset.status = core::Asset::Status::Ready;
    // `length` is already measured in the sequence's own frames
    // (probeMedia opens its throwaway producer at that fps), so duration
    // follows directly from it -- no dependency on probed.fps (0 for a
    // still image/generator) being set at all.
    if (sequenceFps.num > 0 && sequenceFps.den > 0)
        asset.info.nativeDurationSeconds = static_cast<double>(length) * sequenceFps.den / sequenceFps.num;
    // "Format" (container) is a filename-extension read, not an MLT
    // property -- meta.media.* has no reliable container/format string
    // (verified empirically alongside probeMedia's fps/width/height
    // work), and the extension is exactly what a "format" column means
    // to a user browsing their imports anyway.
    auto dot = asset.displayName.find_last_of('.');
    asset.info.container = (dot == std::string::npos) ? std::string{} : asset.displayName.substr(dot + 1);
    asset.status = core::Asset::Status::Ready;
    return asset;
}

// A GFile from a file dialog or a drop, as a local path -- see
// portal_path.h for why it isn't just g_file_get_path().
std::string localPathFor(GFile *file, bool createIfMissing = false)
{
    char *raw = g_file_get_path(file);
    if (!raw)
        return {};
    std::string path = raw;
    g_free(raw);
    return portal::resolveHostPath(path, createIfMissing);
}
} // namespace

AppWindow::AppWindow(GtkApplication *app, const std::vector<dropins::ShellExtension> &shellExtensions)
{
    // Before buildUi(): its transport-bar preview-scale dropdown reads
    // defaultPreviewScale() for its initial selection.
    m_settings = std::make_unique<Settings>();
    m_renderProfiles = std::make_unique<RenderProfileStore>(RenderProfileStore::defaultPath());
    m_snapWhileDragging = m_settings->snapWhileDragging();
    m_followPlayhead = m_settings->followPlayhead();
    m_showTimelineThumbnails = m_settings->showTimelineThumbnails();
    m_showWaveforms = m_settings->showWaveforms();
    m_showHoverPreview = m_settings->showHoverPreview();

    // One starting track, matching v1's "track 0 always exists" default --
    // not through the UndoStack, since this is the pristine starting
    // state, not a user edit to undo back out of.
    m_model.addTrack(core::Track::Kind::Video, 0, "V1");

    m_engine = std::make_unique<engine::Engine>(m_model.snapshot(), engine::PreviewScale::Full);
    // Fires on the main thread after the engine thread has swapped in a
    // rebuilt graph, whether from a published edit (m_undoStack.changed
    // below) or a reset() (Open Project, recovery load).
    m_engine->rebuilt.connect([this] {
        m_lastEditMonotonicUsec = g_get_monotonic_time();
        if (m_unsavedSinceMonotonicUsec == 0)
            m_unsavedSinceMonotonicUsec = m_lastEditMonotonicUsec;
        updatePreviewScaleLabel(); // Auto may have changed with the edit
    });
    m_engine->mediaUnavailable.connect([this](const std::string &path) { onMediaUnavailable(path); });
    m_engine->setFrameCallback([this](std::vector<uint8_t> rgba, int width, int height, int frameNumber) {
        onFrameReady(std::move(rgba), width, height, frameNumber);
    });

    // Sized once, here: the worker-threads setting takes effect at the next
    // launch (0 = automatic).
    const int workerThreads = m_settings->workerThreads();
    const size_t poolSize =
        workerThreads > 0 ? static_cast<size_t>(workerThreads) : core::concurrency::ThreadPool::defaultThreadCount();
    m_pool = std::make_unique<core::concurrency::ThreadPool>(poolSize);
    Log::debug("[app] worker pool: " + std::to_string(poolSize) + " threads" +
               (workerThreads > 0 ? " (worker-threads setting)" : " (automatic)"));
    m_importQueue = std::make_unique<ImportQueue>(
        *m_pool,
        [token = std::weak_ptr<void>(m_lifetime)](std::function<void()> fn) {
            engine::MainThreadDispatcher::post(token, std::move(fn));
        },
        // A probe's timeout (doc 07): dropped with the window like a post.
        [token = std::weak_ptr<void>(m_lifetime)](unsigned ms, std::function<void()> fn) {
            using Timed = std::pair<std::weak_ptr<void>, std::function<void()>>;
            g_timeout_add_once(
                ms,
                [](gpointer data) {
                    std::unique_ptr<Timed> timed(static_cast<Timed *>(data));
                    if (!timed->first.expired())
                        timed->second();
                },
                new Timed(token, std::move(fn)));
        });
    // Which H.264 encoder there is (Settings > Render asks): MLT prints a
    // list to find out, so it's asked once, off the main thread, and nothing
    // waits for it. A render asks too, if it gets there first.
    m_pool->submit([](std::stop_token) { engine::h264Encoder(); });
    m_renderThreadsPercent = m_settings->renderThreadsPercent();
    m_renderQueue = std::make_unique<RenderQueue>(
        [this](const RenderJob &job, std::string &error, std::function<void(int, int)> onProgress,
               const std::atomic<bool> &cancel) {
            core::Model model(*job.snapshot);
            const int budget = core::renderThreadBudget(
                m_renderThreadsPercent.load(), static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
            return engine::renderProject(model, job.outputPath, error, std::move(onProgress), &cancel, job.profile,
                                         budget);
        },
        [token = std::weak_ptr<void>(m_lifetime)](std::function<void()> fn) {
            engine::MainThreadDispatcher::post(token, std::move(fn));
        },
        RenderQueue::Callbacks{
            .started = [this](const RenderJob &job) { onRenderStarted(job); },
            .progress = [this](const RenderJob &, double fraction) { onRenderProgress(fraction); },
            .finished = [this](const RenderJob &job, bool ok, bool cancelled,
                               const std::string &error) { onRenderDone(job, ok, cancelled, error); },
        });
    m_saveQueue =
        std::make_unique<SaveQueue>(*m_pool, [token = std::weak_ptr<void>(m_lifetime)](std::function<void()> fn) {
            engine::MainThreadDispatcher::post(token, std::move(fn));
        });
    m_projectLoader = std::make_unique<ProjectLoader>(
        *m_pool,
        [token = std::weak_ptr<void>(m_lifetime)](std::function<void()> fn) {
            engine::MainThreadDispatcher::post(token, std::move(fn));
        },
        // Pool: the parse, then which media files are missing (a stat each;
        // doc 07), so a slow mount never stalls the window.
        [](const std::string &path) {
            ProjectLoader::Result loaded = core::loadProjectFile(path);
            if (loaded)
                core::markMissingMedia(*loaded);
            return loaded;
        });
    // doc 19 MT3: the caches run on the pool too, capped so imports, probes
    // and saves still get threads: about half the pool between the timeline's
    // strips and its waveforms, plus one job for the media browser's row
    // thumbnails (Background: its list asks for every row, shown or not).
    // Settings > Performance > Cache jobs overrides the half, up to the pool.
    const int cacheJobsSetting = m_settings->cacheJobs();
    const size_t cacheJobs = cacheJobsSetting > 0 ? std::min(static_cast<size_t>(cacheJobsSetting), poolSize)
                                                  : std::max<size_t>(1, poolSize / 2);
    const size_t stripJobs = std::max<size_t>(1, cacheJobs / 2);
    const size_t waveformJobs = std::max<size_t>(1, cacheJobs - stripJobs);
    m_waveforms = std::make_unique<engine::WaveformCache>(*m_pool, [this] { onWaveformReady(); }, waveformJobs);
    m_thumbnails = std::make_unique<engine::ThumbnailCache>(
        *m_pool, [this] { onThumbnailReady(); }, 1, core::concurrency::Priority::Background);
    // The timeline's strips are a separate cache: dozens of frames arrive
    // while scrolling, and each only needs the timeline redrawn, not the
    // media browser rebuilt.
    m_timelineThumbnails = std::make_unique<engine::ThumbnailCache>(
        *m_pool,
        [this] {
            if (m_timeline)
                gtk_widget_queue_draw(m_timeline);
        },
        stripJobs, core::concurrency::Priority::Interactive);
    m_hoverThumbnails = std::make_unique<engine::ThumbnailCache>(
        *m_pool, [this] { showHoverPreviewIfReady(); }, 1, core::concurrency::Priority::Interactive, kHoverPreviewWidth,
        64);

    // Single source of truth for the undo/redo buttons and the title's
    // dirty mark (audit A1): every place that used to call
    // updateWindowTitle() by hand after touching the undo stack (import,
    // split, move, trim, Save, Open, Undo, Redo, Recover, ...) now just
    // goes through UndoStack::execute()/undo()/redo()/setCleanPoint()/
    // clear(), all of which emit `changed`, so nothing can forget to
    // refresh these after an ordinary edit the way manual call sites did.
    m_undoStack.changed.connect([this] {
        // doc 19 MT2: the engine builds from snapshots, published here --
        // once per command, undo or redo (a batch is one command), not per
        // model event. A save marking the stack clean publishes the same
        // snapshot again, which EngineSync ignores.
        m_engine->publish(m_model.snapshot());
        updateWindowTitle();
        // Any edit, undo or redo can remove clips (RemoveAsset takes every
        // clip cut from the asset): drop them from the selection here, in
        // one place (post-M3 audit P1: a stale id aborted Shift+Delete).
        // And a drag in progress was measured against the old model, so it
        // ends (P8).
        m_timelineController.selection().prune(m_model);
        if (m_timelineController.mode() != timeline::TimelineController::Mode::None) {
            m_timelineController.cancel();
            if (m_timeline)
                gtk_widget_queue_draw(m_timeline);
        }
        // A drop-in's drag too, unless the change is its own: an edit it
        // makes while dragging comes back through here, so it's cancelled
        // only when it didn't cause the change (m_overlayDragEditing).
        if (!m_overlayDragEditing)
            cancelOverlayDrag();
        // IP5: drop-ins' inspector pages refresh, the selection already
        // pruned.
        m_shellProjectChanged.emit();
        refreshMissingBanner(); // a relink, or its undo
        endTransformDrag();     // unless the change is the drag's own
        refreshTransformDialog();
        if (m_transformOverlay)
            gtk_widget_queue_draw(m_transformOverlay);
    });

    gchar *sessionUuid = g_uuid_string_random();
    m_autosaveSessionId = sessionUuid;
    m_ownAutosaves = std::make_unique<autosave::OwnAutosaves>(m_autosaveSessionId);
    g_free(sessionUuid);

    buildUi(app);
    installActions(app);
    // Doc 15 IP5: the drop-ins' pages, actions and overlays, into the
    // finished shell (shell_hosts.cpp).
    setUpProxies();
    setUpGpu();
    setUpTransformOverlay();
    m_hasShellExtensions = !shellExtensions.empty();
    for (const dropins::ShellExtension &extension : shellExtensions)
        extension(*this);
    // Every extension has added its widgets: the docking inspector's
    // minimum covers them all (shell_hosts.cpp).
    if (m_inspectorSplit)
        pinInspectorMinimum();
    g_signal_connect(m_window, "notify::is-active", G_CALLBACK(&AppWindow::windowActiveChangedTrampoline), this);
    g_signal_connect(m_window, "notify::focus-widget", G_CALLBACK(&AppWindow::focusWidgetChangedTrampoline), this);
    g_signal_connect(m_window, "close-request", G_CALLBACK(&AppWindow::closeRequestTrampoline), this);
    // Heartbeat, not a one-shot timer reset on every edit: simpler to
    // reason about than adding/removing a GSource on every
    // keystroke-equivalent. autosave::autosaveDue() accounts for its
    // interval so a kill -9 still loses at most the autosave delay.
    m_autosaveHeartbeatId =
        g_timeout_add_seconds(kAutosaveHeartbeatSeconds, &AppWindow::autosaveHeartbeatTrampoline, this);

    // Read before updateWindowTitle() records the (still untitled) current
    // project over it.
    const std::string lastProject = m_settings->lastProjectPath();
    refreshTimeline();
    updateWindowTitle();
    refreshRecentProjectsMenu();
    showStatus("Import a media file to begin.");
    // Recovering unsaved work takes precedence over reopening.
    if (!offerRecoveryIfAny())
        reopenLastProjectIfWanted(lastProject);
    offerPendingRenders();
    updateRenderButton();
}

void AppWindow::reopenLastProjectIfWanted(const std::string &path)
{
    if (!m_settings->reopenLastProject() || path.empty())
        return;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        Log::warn("[app] the last project isn't there any more: " + path);
        showStatus("The last project isn't there any more: " + path);
        return;
    }
    loadProjectFromPath(path);
}

void AppWindow::prepareForShutdown()
{
    Log::info("[app] Preparing for shutdown");
    // Audit A2: a last-resort safety net. onCloseRequest() prompts before
    // a normal window close, but this fires unconditionally whenever the
    // GApplication actually quits -- including after that prompt's own
    // "Discard" (the autosave doesn't touch m_currentProjectPath, so
    // discarding still leaves a recovery point behind) and any path that
    // reaches shutdown without going through onCloseRequest at all.
    // Saves and autosaves write on the pool: let the running one land and
    // run the waiting ones here, then take a last autosave the same way.
    if (m_saveQueue) {
        m_saveQueue->finish();
        if (!m_undoStack.isClean()) {
            performAutosave();
            m_saveQueue->finish();
        } else if (m_ownAutosaves) {
            // Nothing unsaved: this session's autosaves are all stale, and
            // left behind they'd be offered for recovery at the next launch
            // (and stop reopen-last-project).
            removeStaleAutosaves(m_ownAutosaves->quitClean());
        }
    }
    // A render still running would be inside MLT when main() closes the
    // factory (post-M3 audit P2: 3/3 crashes). Stop it and wait for it;
    // renderProject() removes its .part file when cancelled. What didn't
    // finish is kept for the next launch to offer. (Nothing unfinished
    // leaves the folder alone, so an offer left unanswered stays.)
    if (m_renderQueue) {
        std::vector<RenderJob> unfinished = m_renderQueue->shutdown();
        if (!unfinished.empty()) {
            std::string error = pending_renders::save(unfinished, pending_renders::directory());
            if (error.empty())
                Log::info("[render] Kept " + std::to_string(unfinished.size()) + " unfinished render(s) for next time");
            else
                Log::warn("[render] " + error);
        }
    }
    // Pool jobs open MLT producers too: cancel them and join the workers
    // before main() closes the factory (doc 19 MT1).
    if (m_importQueue)
        m_importQueue->cancelAll();
    m_importQueue.reset();
    if (m_proxyQueue)
        m_proxyQueue->cancelAll(); // the children stop and remove their .part files
    if (m_projectLoader)
        m_projectLoader->cancel();
    m_projectLoader.reset();
    m_saveQueue.reset();
    // The caches' jobs run on the pool: stop them before it goes. The
    // caches stay alive (a late draw may still ask them) but take no work.
    hideHoverPreview();
    for (engine::ThumbnailCache *cache : {m_thumbnails.get(), m_timelineThumbnails.get(), m_hoverThumbnails.get()})
        if (cache)
            cache->shutdown();
    if (m_waveforms)
        m_waveforms->shutdown();
    m_pool.reset();
    if (m_engine)
        m_engine->shutdown();
    if (m_gpu)
        m_gpu->shutdown(); // a clean exit: no crash sentinel
    if (m_refreshSourceId != 0) {
        g_source_remove(m_refreshSourceId);
        m_refreshSourceId = 0;
    }
}

void AppWindow::buildUi(GtkApplication *app)
{
    m_window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1100, 700);
    // Maximised by default (owner, 2026-09-28): the editor wants the room.
    // 1100x700 is the size it unmaximises to. No window state is saved
    // yet; when it is, a saved state wins over this.
    gtk_window_maximize(GTK_WINDOW(m_window));

    GtkWidget *toolbarView = adw_toolbar_view_new();
    m_toolbarView = ADW_TOOLBAR_VIEW(toolbarView);

    GtkWidget *headerBar = adw_header_bar_new();
    m_windowTitle = ADW_WINDOW_TITLE(adw_window_title_new("U-Stu", nullptr));
    // The title is a button: its subtitle shows the project's size and rate,
    // and clicking it changes the rate (onProjectFrameRateClicked()).
    GtkWidget *titleButton = gtk_button_new();
    gtk_widget_add_css_class(titleButton, "flat");
    gtk_button_set_child(GTK_BUTTON(titleButton), GTK_WIDGET(m_windowTitle));
    setTooltip(titleButton, "header.project-format");
    g_signal_connect(titleButton, "clicked", G_CALLBACK(&AppWindow::projectFrameRateClickedTrampoline), this);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(headerBar), titleButton);

    // Header buttons come in groups, spaced apart by the .header-group CSS
    // class: left is Import, Add track, media browser | Undo, Redo.
    auto headerGroup = [] {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(box, "header-group");
        return box;
    };

    GtkWidget *mediaGroup = headerGroup();
    GtkWidget *importButton = gtk_button_new_with_label("Import…");
    gtk_widget_add_css_class(importButton, "suggested-action");
    setTooltip(importButton, "header.import");
    g_signal_connect(importButton, "clicked", G_CALLBACK(&AppWindow::importClickedTrampoline), this);
    gtk_box_append(GTK_BOX(mediaGroup), importButton);

    GtkWidget *addTrackButton = gtk_button_new_from_icon_name("list-add-symbolic");
    setTooltip(addTrackButton, "header.add-track");
    g_signal_connect(addTrackButton, "clicked", G_CALLBACK(&AppWindow::addTrackClickedTrampoline), this);
    gtk_box_append(GTK_BOX(mediaGroup), addTrackButton);

    // Verified against the installed Adwaita symbolic icon set
    // (/usr/share/icons/Adwaita/symbolic/actions/sidebar-show-symbolic.svg)
    // rather than guessed -- CLAUDE.md's icon rule.
    GtkWidget *toggleMediaBrowserButton = gtk_button_new_from_icon_name("sidebar-show-symbolic");
    setTooltip(toggleMediaBrowserButton, "header.media-browser");
    g_signal_connect(toggleMediaBrowserButton, "clicked", G_CALLBACK(&AppWindow::toggleMediaBrowserClickedTrampoline),
                     this);
    gtk_box_append(GTK_BOX(mediaGroup), toggleMediaBrowserButton);

    GtkWidget *historyGroup = headerGroup();
    m_undoButton = GTK_BUTTON(gtk_button_new_from_icon_name("edit-undo-symbolic"));
    setTooltip(GTK_WIDGET(m_undoButton), "header.undo");
    g_signal_connect(m_undoButton, "clicked", G_CALLBACK(&AppWindow::undoClickedTrampoline), this);
    gtk_box_append(GTK_BOX(historyGroup), GTK_WIDGET(m_undoButton));

    m_redoButton = GTK_BUTTON(gtk_button_new_from_icon_name("edit-redo-symbolic"));
    setTooltip(GTK_WIDGET(m_redoButton), "header.redo");
    g_signal_connect(m_redoButton, "clicked", G_CALLBACK(&AppWindow::redoClickedTrampoline), this);
    gtk_box_append(GTK_BOX(historyGroup), GTK_WIDGET(m_redoButton));
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), mediaGroup);
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), historyGroup);

    // The right-hand side reads, left to right: zoom | project (New, Open,
    // Recent, Reload, Save) | Render | Settings, Help. Zoom comes first, the
    // quickest to reach (owner, 2026-09-25). pack_end places right to left,
    // so the groups are packed in reverse at the end.

    GtkWidget *zoomGroup = headerGroup();
    auto addZoomButton = [&](const char *iconName, const char *actionName, const char *hintKey) {
        GtkWidget *button = gtk_button_new_from_icon_name(iconName);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(button), actionName);
        setTooltip(button, hintKey);
        gtk_box_append(GTK_BOX(zoomGroup), button);
    };
    addZoomButton("zoom-out-symbolic", "win.zoom-out", "header.zoom-out");
    addZoomButton("zoom-in-symbolic", "win.zoom-in", "header.zoom-in");
    addZoomButton("zoom-fit-best-symbolic", "win.zoom-fit", "header.zoom-fit");

    GtkWidget *projectGroup = headerGroup();
    GtkWidget *newProjectButton = gtk_button_new_from_icon_name("document-new-symbolic");
    setTooltip(newProjectButton, "header.new-project");
    g_signal_connect(newProjectButton, "clicked", G_CALLBACK(&AppWindow::newProjectClickedTrampoline), this);
    gtk_box_append(GTK_BOX(projectGroup), newProjectButton);

    GtkWidget *openButton = gtk_button_new_from_icon_name("document-open-symbolic");
    setTooltip(openButton, "header.open");
    g_signal_connect(openButton, "clicked", G_CALLBACK(&AppWindow::openProjectClickedTrampoline), this);
    gtk_box_append(GTK_BOX(projectGroup), openButton);

    // Enhancement #15: recent projects, next to Open. refreshRecentProjectsMenu()
    // (called once below, and again after every successful save/open)
    // rebuilds m_recentProjectsPopover's contents from GtkRecentManager.
    GtkWidget *recentProjectsButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(recentProjectsButton), "document-open-recent-symbolic");
    setTooltip(recentProjectsButton, "header.recent");
    m_recentProjectsPopover = GTK_POPOVER(gtk_popover_new());
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(recentProjectsButton), GTK_WIDGET(m_recentProjectsPopover));
    gtk_box_append(GTK_BOX(projectGroup), recentProjectsButton);
    g_signal_connect(gtk_recent_manager_get_default(), "changed",
                     G_CALLBACK(&AppWindow::recentManagerChangedTrampoline), this);

    GtkWidget *reloadButton = gtk_button_new_from_icon_name("view-refresh-symbolic");
    setTooltip(reloadButton, "header.reload");
    g_signal_connect(reloadButton, "clicked", G_CALLBACK(&AppWindow::reloadProjectClickedTrampoline), this);
    gtk_box_append(GTK_BOX(projectGroup), reloadButton);

    GtkWidget *saveButton = gtk_button_new_from_icon_name("document-save-symbolic");
    setTooltip(saveButton, "header.save");
    g_signal_connect(saveButton, "clicked", G_CALLBACK(&AppWindow::saveClickedTrampoline), this);
    GtkGesture *saveRightClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(saveRightClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(saveRightClick, "pressed", G_CALLBACK(&AppWindow::saveButtonRightClickTrampoline), this);
    gtk_widget_add_controller(saveButton, GTK_EVENT_CONTROLLER(saveRightClick));
    gtk_box_append(GTK_BOX(projectGroup), saveButton);

    // Render: a label over a progress bar that fills magenta while a render
    // runs, and a badge counting the queue (updateRenderButton()).
    GtkWidget *renderButton = gtk_button_new();
    m_renderButton = renderButton;
    gtk_widget_add_css_class(renderButton, "render-button");
    GtkWidget *renderOverlay = gtk_overlay_new();
    m_renderProgress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(m_renderProgress), "render-progress");
    gtk_widget_set_valign(GTK_WIDGET(m_renderProgress), GTK_ALIGN_FILL);
    gtk_widget_set_visible(GTK_WIDGET(m_renderProgress), FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(renderOverlay), GTK_WIDGET(m_renderProgress));
    m_renderLabel = GTK_LABEL(gtk_label_new("Render…"));
    gtk_widget_add_css_class(GTK_WIDGET(m_renderLabel), "render-label");
    // "Open Render" and "Render 100%" fit, so the header doesn't reflow as
    // it counts; kept narrow for the header's room (owner, 2026-09-28).
    gtk_label_set_width_chars(m_renderLabel, 11);
    gtk_overlay_add_overlay(GTK_OVERLAY(renderOverlay), GTK_WIDGET(m_renderLabel));
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(renderOverlay), GTK_WIDGET(m_renderLabel), TRUE);
    m_renderBadge = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(m_renderBadge), "render-badge");
    gtk_widget_set_halign(GTK_WIDGET(m_renderBadge), GTK_ALIGN_END);
    gtk_widget_set_valign(GTK_WIDGET(m_renderBadge), GTK_ALIGN_START);
    gtk_widget_set_visible(GTK_WIDGET(m_renderBadge), FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(renderOverlay), GTK_WIDGET(m_renderBadge));
    gtk_button_set_child(GTK_BUTTON(renderButton), renderOverlay);
    setTooltip(renderButton, "header.render");
    g_signal_connect(renderButton, "clicked", G_CALLBACK(&AppWindow::renderClickedTrampoline), this);
    GtkGesture *renderRightClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(renderRightClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(renderRightClick, "pressed", G_CALLBACK(&AppWindow::renderButtonRightClickTrampoline), this);
    gtk_widget_add_controller(renderButton, GTK_EVENT_CONTROLLER(renderRightClick));
    m_renderMenu = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_renderMenu), renderButton);
    g_signal_connect(renderButton, "destroy", G_CALLBACK(&AppWindow::unparentPopoverTrampoline), m_renderMenu);

    GtkWidget *appGroup = headerGroup();
    m_appHeaderGroup = appGroup;
    GtkWidget *settingsButton = gtk_button_new_from_icon_name("preferences-system-symbolic");
    m_settingsButton = settingsButton;
    setTooltip(settingsButton, "header.settings");
    g_signal_connect(settingsButton, "clicked", G_CALLBACK(&AppWindow::settingsClickedTrampoline), this);
    gtk_box_append(GTK_BOX(appGroup), settingsButton);

    // Our own plain "?" (data/icons/symbolic): Adwaita's help icons are a
    // lifebuoy or a "?" in a speech bubble, unlike the header's other
    // single-glyph icons (owner, 2026-09-28).
    GtkWidget *helpButton = gtk_button_new_from_icon_name("ustudio-help-symbolic");
    setTooltip(helpButton, "header.help");
    g_signal_connect(helpButton, "clicked", G_CALLBACK(&AppWindow::helpClickedTrampoline), this);
    gtk_box_append(GTK_BOX(appGroup), helpButton);

    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), appGroup);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), renderButton);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), projectGroup);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), zoomGroup);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), headerBar);
    // Without a schema nothing in Settings survives a restart (reopen-last-
    // project included); say so where it can't be missed, not only in the
    // Settings dialog.
    if (!m_settings->isPersistent()) {
        AdwBanner *banner = ADW_BANNER(adw_banner_new("Settings can't be saved: schema not found"));
        adw_banner_set_revealed(banner, TRUE);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), GTK_WIDGET(banner));
    }

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 420);

    // Preview row: [media browser panel | preview], side by side.
    GtkWidget *previewRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    // Media browser: a plain toggleable GtkBox, not GtkRevealer (unused
    // elsewhere in this codebase, and an animated slide would be
    // "decorative" per the design system's own glow/animation rule) --
    // gtk_widget_set_visible(FALSE) on a box child reclaims its layout
    // space immediately, which is all "collapsible" needs here.
    GtkWidget *mediaBrowserScroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(mediaBrowserScroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(mediaBrowserScroller, 320, -1);
    gtk_widget_add_css_class(mediaBrowserScroller, "media-browser-panel");
    setUpMediaList(mediaBrowserScroller);
    m_mediaBrowserPanel = mediaBrowserScroller;
    gtk_widget_set_visible(m_mediaBrowserPanel, FALSE); // starts collapsed

    // Enhancement #7 (media-browser half): files dragged in from outside
    // the app land in the bin only (startImport() with no track) -- no track/
    // position to insert a clip at here, unlike the timeline's own file
    // drop target above.
    GtkDropTarget *mediaBrowserFileDropTarget = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(mediaBrowserFileDropTarget, "drop", G_CALLBACK(&AppWindow::mediaBrowserFileDropTrampoline), this);
    gtk_widget_add_controller(m_mediaBrowserPanel, GTK_EVENT_CONTROLLER(mediaBrowserFileDropTarget));
    gtk_box_append(GTK_BOX(previewRow), m_mediaBrowserPanel);

    // Right-click menu for a media browser row. Parented once to
    // m_mediaBrowserPanel (the outer GtkScrolledWindow, like
    // m_trackContextMenu -> m_timeline below), deliberately NOT to
    // m_mediaBrowserList or to the row that was clicked: rows are
    // destroyed and rebuilt wholesale by refreshMediaBrowser() on every
    // bin change, and gtk_widget_set_parent() makes a popover a real
    // child in the generic widget tree -- parenting it to
    // m_mediaBrowserList would put it right in the path of
    // refreshMediaBrowser()'s own "walk every child of the list and
    // remove it" loop (confirmed empirically: it was, and got swept up
    // and destroyed by the very first refresh after buildUi(), leaving
    // this member dangling -- a real, reproduced use-after-free crash in
    // gtk_popover_set_pointing_to() the first time a row was right-
    // clicked, via coredumpctl + gdb backtrace, 2026-09-23). The outer
    // scroller is never touched by that loop -- only its one designated
    // child (m_mediaBrowserList, set via gtk_scrolled_window_set_child)
    // is -- so it's a safe, stable parent.
    m_mediaBrowserContextMenu = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_mediaBrowserContextMenu), m_mediaBrowserPanel);
    GtkWidget *mediaContextBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    m_removeAssetButton = gtk_button_new_with_label("Remove from Project");
    gtk_widget_add_css_class(m_removeAssetButton, "flat");
    setTooltip(m_removeAssetButton, "media.remove");
    g_signal_connect(m_removeAssetButton, "clicked", G_CALLBACK(&AppWindow::removeAssetClickedTrampoline), this);
    gtk_box_append(GTK_BOX(mediaContextBox), m_removeAssetButton);
    m_deleteAssetFileButton = gtk_button_new_with_label("Move File to Trash…");
    gtk_widget_add_css_class(m_deleteAssetFileButton, "flat");
    setTooltip(m_deleteAssetFileButton, "media.trash");
    g_signal_connect(m_deleteAssetFileButton, "clicked", G_CALLBACK(&AppWindow::deleteAssetFileClickedTrampoline),
                     this);
    gtk_box_append(GTK_BOX(mediaContextBox), m_deleteAssetFileButton);
    addProxyMenuItems(mediaContextBox);
    gtk_popover_set_child(m_mediaBrowserContextMenu, mediaContextBox);
    // A popover parented with gtk_widget_set_parent() has to be unparented
    // by hand before its parent goes (post-M3 audit P10: GTK warned on every
    // close). The timeline's popovers are handled by UsTimelineView's dispose.
    g_signal_connect(m_mediaBrowserPanel, "destroy", G_CALLBACK(&AppWindow::unparentPopoverTrampoline),
                     m_mediaBrowserContextMenu);

    GtkWidget *previewFrame = gtk_frame_new(nullptr);
    m_previewFrame = previewFrame;
    gtk_widget_add_css_class(previewFrame, "preview-frame");
    gtk_widget_set_hexpand(previewFrame, TRUE);
    m_preview = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(m_preview, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(GTK_WIDGET(m_preview), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(m_preview), TRUE);
    gtk_frame_set_child(GTK_FRAME(previewFrame), GTK_WIDGET(m_preview));
    addTextFocusRelease(GTK_WIDGET(m_preview));
    gtk_box_append(GTK_BOX(previewRow), previewFrame);

    gtk_paned_set_start_child(GTK_PANED(paned), previewRow);

    // Timeline + transport
    GtkWidget *bottomBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(bottomBox, 6);
    gtk_widget_set_margin_bottom(bottomBox, 6);
    gtk_widget_set_margin_start(bottomBox, 6);
    gtk_widget_set_margin_end(bottomBox, 6);

    m_timeline = timeline::newTimelineView(
        [this](GtkSnapshot *snapshot, int width, int height) { snapshotTimelineView(snapshot, width, height); },
        [this](int, int) {
            updateViewportGeometry();
            onTimelineViewportChanged();
        });
    gtk_widget_set_hexpand(GTK_WIDGET(m_timeline), TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(m_timeline), -1, static_cast<int>(kTrackRowHeight));
    gtk_widget_add_css_class(GTK_WIDGET(m_timeline), "timeline-area");

    // Ctrl+wheel zoom and sideways scrolling (onTimelineScroll). A plain
    // vertical wheel isn't claimed, so it reaches the vertical scroller
    // around the timeline below.
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(&AppWindow::timelineScrollTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), scroll);
    // The scroll event carries no position; Ctrl+wheel zoom anchors on the
    // last pointer x seen here.
    // Pinch to zoom (touchpad or touchscreen), anchored between the fingers.
    GtkGesture *pinch = gtk_gesture_zoom_new();
    g_signal_connect(pinch, "begin", G_CALLBACK(&AppWindow::timelinePinchBeginTrampoline), this);
    g_signal_connect(pinch, "scale-changed", G_CALLBACK(&AppWindow::timelinePinchTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(pinch));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(&AppWindow::timelineMotionTrampoline), this);
    g_signal_connect(motion, "leave", G_CALLBACK(&AppWindow::timelineLeaveTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), motion);

    addTextFocusRelease(GTK_WIDGET(m_timeline));
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&AppWindow::timelineClickTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(click));

    GtkGesture *rightClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rightClick, "pressed", G_CALLBACK(&AppWindow::timelineRightClickTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(rightClick));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(&AppWindow::trackDragBeginTrampoline), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(&AppWindow::trackDragUpdateTrampoline), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(&AppWindow::trackDragEndTrampoline), this);
    // GTK cancels a gesture (a grab elsewhere, focus lost) with "cancel"
    // before its "drag-end": a drop-in's drag ends unfinished then.
    g_signal_connect(drag, "cancel", G_CALLBACK(&AppWindow::trackDragCancelTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(drag));

    // Drop target for dragging a media browser row onto the timeline
    // (refreshMediaBrowser() puts a matching GtkDragSource, carrying the
    // asset's AssetId::value as a G_TYPE_INT64, on each row).
    GtkDropTarget *dropTarget = gtk_drop_target_new(G_TYPE_INT64, GDK_ACTION_COPY);
    g_signal_connect(dropTarget, "drop", G_CALLBACK(&AppWindow::timelineDropTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(dropTarget));

    // Enhancement #7: a second, independent drop target for files dragged
    // in from outside the app (the file manager, most likely) -- a
    // different GType (GDK_TYPE_FILE_LIST) from the asset-row drag
    // above, so both controllers coexist on the same widget without
    // conflicting; GTK dispatches whichever one's type the actual drag
    // content matches.
    GtkDropTarget *fileDropTarget = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(fileDropTarget, "drop", G_CALLBACK(&AppWindow::timelineFileDropTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(fileDropTarget));

    // The clip tooltip (see m_hoverPreview's declaration): never takes focus
    // or the pointer, and sits a little above the pointer so moving onto it
    // doesn't leave the timeline.
    m_hoverPreview = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_autohide(m_hoverPreview, FALSE);
    gtk_popover_set_has_arrow(m_hoverPreview, FALSE);
    gtk_popover_set_position(m_hoverPreview, GTK_POS_TOP);
    gtk_popover_set_offset(m_hoverPreview, 0, -16);
    gtk_widget_set_can_target(GTK_WIDGET(m_hoverPreview), FALSE);
    gtk_widget_set_can_focus(GTK_WIDGET(m_hoverPreview), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(m_hoverPreview), "hover-preview");
    GtkWidget *hoverBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    m_hoverPicture = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(m_hoverPicture, GTK_CONTENT_FIT_SCALE_DOWN); // never larger than it is
    gtk_widget_set_halign(GTK_WIDGET(m_hoverPicture), GTK_ALIGN_CENTER);     // its own size, not the text's width
    gtk_box_append(GTK_BOX(hoverBox), GTK_WIDGET(m_hoverPicture));
    m_hoverTimecode = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(m_hoverTimecode), "timecode-label");
    gtk_box_append(GTK_BOX(hoverBox), GTK_WIDGET(m_hoverTimecode));
    m_hoverText = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(m_hoverText, 0.0f);
    // Long source paths wrap, as a GTK tooltip's would.
    gtk_label_set_wrap(m_hoverText, TRUE);
    gtk_label_set_wrap_mode(m_hoverText, PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(m_hoverText, 60);
    gtk_box_append(GTK_BOX(hoverBox), GTK_WIDGET(m_hoverText));
    gtk_popover_set_child(m_hoverPreview, hoverBox);
    gtk_widget_set_parent(GTK_WIDGET(m_hoverPreview), GTK_WIDGET(m_timeline));
    // Gone with the timeline when the window closes, before shutdown's
    // hideHoverPreview() runs: that read a dangling pointer (a
    // Gtk-CRITICAL, gtk_widget_get_visible, at every quit).
    g_signal_connect(m_hoverPreview, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer self) {
                         static_cast<AppWindow *>(self)->m_hoverPreview = nullptr;
                     }),
                     this);

    // One popover, three possible actions — onTimelineRightClicked decides
    // which single one is relevant (clip under the cursor -> Delete Clip;
    // gap under the cursor -> Close Gap; otherwise -> Remove Track) and
    // shows only that button.
    m_trackContextMenu = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_trackContextMenu), GTK_WIDGET(m_timeline));
    GtkWidget *contextMenuBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    m_deleteClipButton = gtk_button_new_with_label("Delete Clip");
    gtk_widget_add_css_class(m_deleteClipButton, "flat");
    setTooltip(m_deleteClipButton, "track-menu.delete-clip");
    g_signal_connect(m_deleteClipButton, "clicked", G_CALLBACK(&AppWindow::deleteClipClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_deleteClipButton);

    m_splitAudioButton = gtk_button_new_with_label("Split Audio");
    gtk_widget_add_css_class(m_splitAudioButton, "flat");
    setTooltip(m_splitAudioButton, "track-menu.split-audio");
    g_signal_connect(m_splitAudioButton, "clicked", G_CALLBACK(&AppWindow::splitAudioClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_splitAudioButton);

    m_closeGapButton = gtk_button_new_with_label("Close Gap");
    gtk_widget_add_css_class(m_closeGapButton, "flat");
    setTooltip(m_closeGapButton, "track-menu.close-gap");
    g_signal_connect(m_closeGapButton, "clicked", G_CALLBACK(&AppWindow::closeGapClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_closeGapButton);

    // Volume + Lock/Unlock + Remove Track: all whole-track actions, shown
    // together whenever the right-click landed on empty track space (see
    // onTimelineRightClicked) rather than on a clip or a gap.
    GtkWidget *volumeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(volumeRow), gtk_label_new("Track volume"));
    setTooltip(volumeRow, "track-menu.volume");
    m_trackVolumeScale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05));
    gtk_scale_set_draw_value(m_trackVolumeScale, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(m_trackVolumeScale), TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(m_trackVolumeScale), 120, -1);
    // Not focusable (audit A7): same reasoning as m_seekScale's own comment
    // further down this function -- GtkRange's own Left/Right/Home/End key
    // bindings would otherwise compete with the window-level transport
    // shortcuts once this widget (shown in the track right-click menu) has
    // focus.
    gtk_widget_set_focusable(GTK_WIDGET(m_trackVolumeScale), FALSE);
    g_signal_connect(m_trackVolumeScale, "value-changed", G_CALLBACK(&AppWindow::trackVolumeChangedTrampoline), this);
    gtk_box_append(GTK_BOX(volumeRow), GTK_WIDGET(m_trackVolumeScale));
    gtk_box_append(GTK_BOX(contextMenuBox), volumeRow);

    m_toggleLockButton = gtk_button_new_with_label("Lock Track");
    gtk_widget_add_css_class(m_toggleLockButton, "flat");
    setTooltip(m_toggleLockButton, "track-menu.lock");
    g_signal_connect(m_toggleLockButton, "clicked", G_CALLBACK(&AppWindow::toggleLockClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_toggleLockButton);

    m_toggleHideButton = gtk_button_new_with_label("Hide Track");
    gtk_widget_add_css_class(m_toggleHideButton, "flat");
    setTooltip(m_toggleHideButton, "track-menu.hide");
    g_signal_connect(m_toggleHideButton, "clicked", G_CALLBACK(&AppWindow::toggleHideClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_toggleHideButton);

    m_toggleMuteButton = gtk_button_new_with_label("Mute Track");
    gtk_widget_add_css_class(m_toggleMuteButton, "flat");
    setTooltip(m_toggleMuteButton, "track-menu.mute");
    g_signal_connect(m_toggleMuteButton, "clicked", G_CALLBACK(&AppWindow::toggleMuteClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_toggleMuteButton);

    m_removeTrackButton = gtk_button_new_with_label("Remove Track");
    gtk_widget_add_css_class(m_removeTrackButton, "flat");
    setTooltip(m_removeTrackButton, "track-menu.remove-track");
    g_signal_connect(m_removeTrackButton, "clicked", G_CALLBACK(&AppWindow::removeTrackClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeTrackButton);

    m_editTrackNameButton = gtk_button_new_with_label("Edit Track Name");
    gtk_widget_add_css_class(m_editTrackNameButton, "flat");
    setTooltip(m_editTrackNameButton, "track-menu.track-name");
    g_signal_connect(m_editTrackNameButton, "clicked", G_CALLBACK(&AppWindow::editTrackNameClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_editTrackNameButton);

    // Edit/Remove Clip Name: shown only when the right-click landed on a
    // clip (see onTimelineRightClicked); label text on
    // m_editClipNameButton ("Edit Clip Name" vs "Add Clip Name") and the
    // visibility of m_removeClipNameButton both depend on whether that
    // clip currently has a name.
    m_editClipNameButton = gtk_button_new_with_label("Edit Clip Name");
    gtk_widget_add_css_class(m_editClipNameButton, "flat");
    setTooltip(m_editClipNameButton, "track-menu.clip-name");
    g_signal_connect(m_editClipNameButton, "clicked", G_CALLBACK(&AppWindow::editClipNameClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_editClipNameButton);

    m_removeClipNameButton = gtk_button_new_with_label("Remove Clip Name");
    gtk_widget_add_css_class(m_removeClipNameButton, "flat");
    setTooltip(m_removeClipNameButton, "track-menu.remove-clip-name");
    g_signal_connect(m_removeClipNameButton, "clicked", G_CALLBACK(&AppWindow::removeClipNameClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeClipNameButton);

    // Shown when the right-click landed inside a dissolve transition's
    // overlap region (see onTimelineRightClicked) -- alongside whatever
    // clip buttons above also matched, since the overlap sits inside a
    // clip's own rectangle too.
    m_removeTransitionButton = gtk_button_new_with_label("Remove Transition");
    gtk_widget_add_css_class(m_removeTransitionButton, "flat");
    setTooltip(m_removeTransitionButton, "track-menu.remove-transition");
    g_signal_connect(m_removeTransitionButton, "clicked", G_CALLBACK(&AppWindow::removeTransitionClickedTrampoline),
                     this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeTransitionButton);

    m_syncClipsButton = gtk_button_new_with_label("Sync Tracks (Audio)");
    gtk_widget_add_css_class(m_syncClipsButton, "flat");
    setTooltip(m_syncClipsButton, "track-menu.sync-audio");
    g_signal_connect(m_syncClipsButton, "clicked", G_CALLBACK(&AppWindow::syncClipsClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_syncClipsButton);

    // Shown when the right-click landed near the boundary between two
    // touching, not-yet-linked clips (see onTimelineRightClicked) -- an
    // alternative to dragging a clip's edge past its neighbour.
    m_addTransitionButton = gtk_button_new_with_label("Add Transition");
    gtk_widget_add_css_class(m_addTransitionButton, "flat");
    setTooltip(m_addTransitionButton, "track-menu.add-transition");
    g_signal_connect(m_addTransitionButton, "clicked", G_CALLBACK(&AppWindow::addTransitionClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_addTransitionButton);

    gtk_popover_set_child(m_trackContextMenu, contextMenuBox);

    // Shared inline name-edit popover, reused for both a track's label
    // strip and a clip (see InlineEditKind / m_inlineEditKind): one entry,
    // repositioned and refilled per use by showInlineNameEditor. Enter
    // (the entry's "activate") and clicking away (the popover's "closed",
    // which "activate" triggers by popping the popover down) both funnel
    // into onInlineNameEditClosed to commit; Escape sets m_inlineEditCancelled
    // first so that same "closed" handler discards instead.
    m_inlineNameEditPopover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_inlineNameEditPopover), GTK_WIDGET(m_timeline));
    m_inlineNameEditEntry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(m_inlineNameEditEntry), 160, -1);
    g_signal_connect(m_inlineNameEditEntry, "activate", G_CALLBACK(&AppWindow::inlineNameEditActivateTrampoline), this);
    GtkEventController *inlineEditKey = gtk_event_controller_key_new();
    g_signal_connect(inlineEditKey, "key-pressed", G_CALLBACK(&AppWindow::inlineNameEditKeyTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_inlineNameEditEntry), inlineEditKey);
    gtk_popover_set_child(m_inlineNameEditPopover, GTK_WIDGET(m_inlineNameEditEntry));
    g_signal_connect(m_inlineNameEditPopover, "closed", G_CALLBACK(&AppWindow::inlineNameEditClosedTrampoline), this);

    // Enhancement #12: a fixed-height row ABOVE the track grid, not
    // overlapping it -- unlike the playhead overlay below, this doesn't
    // need to sit on top of m_timeline, so it's simplest as its own
    // widget in the layout rather than another GtkOverlay child. Keeps
    // it out of every row/y-coordinate calculation onTimelineClicked()/
    // onTrackDragBegin()/onTrackDragUpdate()/onTimelineRightClicked()
    // already do (see onRulerDraw()'s own comment).
    m_rulerArea = timeline::newTimelineView(
        [this](GtkSnapshot *snapshot, int width, int height) { snapshotRulerView(snapshot, width, height); });
    gtk_widget_set_size_request(GTK_WIDGET(m_rulerArea), -1, static_cast<int>(kRulerHeight));
    gtk_widget_add_css_class(GTK_WIDGET(m_rulerArea), "timeline-area");
    gtk_box_append(GTK_BOX(bottomBox), GTK_WIDGET(m_rulerArea));

    // Audit A5: the playhead line is drawn on its own overlay, stacked on
    // top of m_timeline, so redrawing it on every displayed frame during
    // playback doesn't also redraw every clip/waveform/label underneath
    // -- see onPlayheadOverlayDraw()'s declaration comment. Not a target
    // for pointer events, so m_timeline (below it in the overlay, still
    // the widget every gesture/drop-target/tooltip controller above is
    // attached to) keeps handling clicks/drags exactly as before.
    GtkWidget *timelineOverlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(timelineOverlay), GTK_WIDGET(m_timeline));

    m_playheadOverlay = timeline::newTimelineView(
        [this](GtkSnapshot *snapshot, int width, int height) { snapshotPlayheadOverlay(snapshot, width, height); });
    gtk_widget_set_can_target(GTK_WIDGET(m_playheadOverlay), FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(timelineOverlay), GTK_WIDGET(m_playheadOverlay));

    // Tracks scroll vertically once there are more than fit; sideways
    // scrolling is the Viewport's (m_timelineHAdjustment, below), so the
    // ruler and the playhead overlay can share it without being inside
    // this scroller.
    GtkWidget *timelineScroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(timelineScroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(timelineScroller), TRUE);
    // At least one row; the rest scroll, so the transport below is never
    // pushed out of a short window. Drag the divider above to see more.
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(timelineScroller),
                                               static_cast<int>(rowLayout().spanOf(0)));
    gtk_widget_set_vexpand(timelineScroller, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(timelineScroller), timelineOverlay);
    gtk_box_append(GTK_BOX(bottomBox), timelineScroller);

    m_timelineHAdjustment = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    g_signal_connect(m_timelineHAdjustment, "value-changed", G_CALLBACK(&AppWindow::timelineHScrollChangedTrampoline),
                     this);
    GtkWidget *timelineHScrollbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, m_timelineHAdjustment);
    setTooltip(timelineHScrollbar, "timeline.scroll");
    gtk_widget_set_margin_start(timelineHScrollbar, static_cast<int>(kHandleWidth));
    gtk_box_append(GTK_BOX(bottomBox), timelineHScrollbar);

    GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    // Transport buttons bound straight to the existing window actions
    // (action_registry.cpp), so each one does exactly what its shortcut
    // does and greys out with it (setTransportActionsEnabled). Not
    // focusable, like the sliders below (audit A7): a focused button
    // takes Space and the arrow keys for itself instead of letting them
    // reach the window's play/step shortcuts.
    auto addTransportButton = [&](const char *icon, const char *action, const char *hintId) {
        GtkWidget *button = gtk_button_new_from_icon_name(icon);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
        setTooltip(button, hintId);
        gtk_widget_set_focusable(button, FALSE);
        gtk_box_append(GTK_BOX(transport), button);
    };
    addTransportButton("media-skip-backward-symbolic", "win.seek-home", "transport.seek-home");
    addTransportButton("media-seek-backward-symbolic", "win.shuttle-reverse", "transport.shuttle-reverse");
    addTransportButton("go-previous-symbolic", "win.step-backward", "transport.step-backward");

    m_playButton = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-start-symbolic"));
    gtk_widget_add_css_class(GTK_WIDGET(m_playButton), "circular");
    setTooltip(GTK_WIDGET(m_playButton), "transport.play-pause");
    g_signal_connect(m_playButton, "clicked", G_CALLBACK(&AppWindow::playToggledTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_playButton));

    addTransportButton("media-playback-stop-symbolic", "win.shuttle-stop", "transport.stop");
    addTransportButton("go-next-symbolic", "win.step-forward", "transport.step-forward");
    addTransportButton("media-seek-forward-symbolic", "win.shuttle-forward", "transport.shuttle-forward");
    addTransportButton("media-skip-forward-symbolic", "win.seek-end", "transport.seek-end");

    GtkWidget *splitButton = gtk_button_new_from_icon_name("edit-cut-symbolic");
    setTooltip(splitButton, "transport.split");
    g_signal_connect(splitButton, "clicked", G_CALLBACK(&AppWindow::splitClickedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), splitButton);

    GtkWidget *rippleButton = gtk_toggle_button_new_with_label("Ripple");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(rippleButton), "win.ripple-mode");
    gtk_widget_set_focusable(rippleButton, FALSE); // audit A7, as the transport buttons
    setTooltip(rippleButton, "transport.ripple-mode");
    gtk_box_append(GTK_BOX(transport), rippleButton);

    m_seekScale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1));
    gtk_scale_set_draw_value(m_seekScale, FALSE);
    setTooltip(GTK_WIDGET(m_seekScale), "transport.seek-bar");
    gtk_widget_set_hexpand(GTK_WIDGET(m_seekScale), TRUE);
    // Not focusable: GtkRange's own key bindings would otherwise compete
    // with (and pre-empt, depending on focus) the window-level Left/Right/
    // Home/End actions below for frame-step/home/end -- there's no other
    // reason for this widget to hold keyboard focus, since seeking is
    // mouse-drag-driven.
    gtk_widget_set_focusable(GTK_WIDGET(m_seekScale), FALSE);
    g_signal_connect(m_seekScale, "value-changed", G_CALLBACK(&AppWindow::seekChangedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_seekScale));

    m_timecodeLabel = GTK_LABEL(gtk_label_new("00:00:00:00"));
    gtk_widget_add_css_class(GTK_WIDGET(m_timecodeLabel), "timecode-label");
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_timecodeLabel));

    GtkWidget *volumeIcon = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
    gtk_box_append(GTK_BOX(transport), volumeIcon);
    m_volumeScale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05));
    gtk_scale_set_draw_value(m_volumeScale, FALSE);
    gtk_range_set_value(GTK_RANGE(m_volumeScale), 1.0);
    gtk_widget_set_size_request(GTK_WIDGET(m_volumeScale), 90, -1);
    setTooltip(GTK_WIDGET(m_volumeScale), "transport.volume");
    gtk_widget_set_focusable(GTK_WIDGET(m_volumeScale), FALSE); // audit A7 -- see m_seekScale's comment above
    g_signal_connect(m_volumeScale, "value-changed", G_CALLBACK(&AppWindow::volumeChangedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_volumeScale));

    const char *previewScaleLabels[] = {"Auto", "Full", "Half", "Quarter", nullptr};
    GtkStringList *previewScaleModel = gtk_string_list_new(previewScaleLabels);
    m_previewScaleDropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(previewScaleModel), nullptr));
    // Settings dialog's "Default preview scale" (General tab) only sets
    // this starting selection -- changing the dropdown itself afterwards
    // is a per-session choice, same as before Settings existed, and does
    // not write back to it (see Settings::defaultPreviewScale's comment).
    const std::string defaultScale = m_settings->defaultPreviewScale();
    guint defaultScaleIndex = 0;
    if (defaultScale == "full")
        defaultScaleIndex = 1;
    else if (defaultScale == "half")
        defaultScaleIndex = 2;
    else if (defaultScale == "quarter")
        defaultScaleIndex = 3;
    gtk_drop_down_set_selected(m_previewScaleDropdown, defaultScaleIndex);
    setTooltip(GTK_WIDGET(m_previewScaleDropdown), "transport.preview-scale");
    // Audit A7: GtkDropDown handles Left/Right/Home/End itself while
    // focused (cycling/jumping between its own entries), the same
    // shortcut-stealing problem as the sliders above.
    gtk_widget_set_focusable(GTK_WIDGET(m_previewScaleDropdown), FALSE);
    g_signal_connect(m_previewScaleDropdown, "notify::selected", G_CALLBACK(&AppWindow::previewScaleChangedTrampoline),
                     this);
    // The selection above was set before the handler was connected, so the
    // Settings default never reached playback; apply it once here.
    onPreviewScaleChanged();
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_previewScaleDropdown));
    buildProxyToggle(transport);

    m_loopStatusLabel = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(m_loopStatusLabel), "dim-label");
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_loopStatusLabel));

    GtkWidget *clearLoopButton = gtk_button_new_from_icon_name("edit-clear-symbolic");
    setTooltip(clearLoopButton, "transport.clear-loop");
    g_signal_connect(clearLoopButton, "clicked", G_CALLBACK(&AppWindow::clearLoopClickedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), clearLoopButton);

    gtk_box_append(GTK_BOX(bottomBox), transport);

    m_statusLabel = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(m_statusLabel), GTK_ALIGN_START);
    // Never wider than the window gives it: a long status (an import of
    // many files) once widened the whole window past the screen. The full
    // text is its tooltip (showStatus()).
    gtk_label_set_ellipsize(m_statusLabel, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(m_statusLabel), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(m_statusLabel), "dim-label");
    gtk_box_append(GTK_BOX(bottomBox), GTK_WIDGET(m_statusLabel));

    gtk_paned_set_end_child(GTK_PANED(paned), bottomBox);

    m_mainPaned = paned;
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), paned);
    adw_application_window_set_content(m_window, toolbarView);
}

void AppWindow::installActions(GtkApplication *app)
{
    GSimpleAction *undoAction = g_simple_action_new("undo", nullptr);
    g_signal_connect(undoAction, "activate", G_CALLBACK(&AppWindow::undoActionActivated), this);
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(undoAction));
    g_object_unref(undoAction);

    GSimpleAction *redoAction = g_simple_action_new("redo", nullptr);
    g_signal_connect(redoAction, "activate", G_CALLBACK(&AppWindow::redoActionActivated), this);
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(redoAction));
    g_object_unref(redoAction);

    // Ripple mode: a boolean state with no parameter, which GIO's default
    // "activate" flips, so R and the transport's toggle button both just
    // activate it and the button shows the state.
    GSimpleAction *rippleAction = g_simple_action_new_stateful("ripple-mode", nullptr, g_variant_new_boolean(FALSE));
    g_signal_connect(rippleAction, "notify::state", G_CALLBACK(&AppWindow::rippleModeChangedTrampoline), this);
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(rippleAction));
    g_object_unref(rippleAction);

    // Every action's name/accels/handler (undo/redo included, for their
    // default accelerators) comes from the shared action_registry.h table
    // now, so it and the Help dialog's Keyboard Shortcuts tab
    // (buildShortcutsPage()) can never drift apart -- see the table's own
    // comment. undo/redo are the only entries with activated == nullptr:
    // their GSimpleActions are already created above (enabled/disabled
    // state tracks UndoStack::canUndo/canRedo via m_undoButton/
    // m_redoButton's sensitivity, not a GAction property), so only their
    // accelerators still need setting. ripple-mode is the same (stateful,
    // made above).
    for (const ActionSpec &spec : actionSpecs()) {
        if (spec.activated != nullptr)
            addAction(app, spec.name, spec.activated, spec.accels);
        else
            setAccelsForAction(app, spec.name, spec.accels);
    }
}

void AppWindow::addAction(GtkApplication *app, const char *name,
                          void (*activated)(GSimpleAction *, GVariant *, gpointer),
                          const std::vector<const char *> &accels)
{
    GSimpleAction *action = g_simple_action_new(name, nullptr);
    // Through one traced trampoline, so the stall monitor names the action
    // behind a slow iteration (doc 19 MT0); the data lives as long as the
    // action.
    g_signal_connect_data(
        action, "activate", G_CALLBACK(&AppWindow::tracedActionTrampoline), new TracedAction{this, activated, name},
        [](gpointer data, GClosure *) { delete static_cast<TracedAction *>(data); }, GConnectFlags{});
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(action));
    g_object_unref(action);
    setAccelsForAction(app, name, accels);
}

void AppWindow::setAccelsForAction(GtkApplication *app, const char *name, const std::vector<const char *> &accels)
{
    std::vector<const char *> accelsWithNull(accels.begin(), accels.end());
    accelsWithNull.push_back(nullptr);
    gtk_application_set_accels_for_action(app, ("win." + std::string(name)).c_str(), accelsWithNull.data());
}

void AppWindow::showSettingsDialog()
{
    AdwDialog *dialog = ADW_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_content_width(dialog, 720); // seven tabs fit side by side
    adw_dialog_set_content_height(dialog, 520);

    auto addPage = [dialog](const char *title, const char *iconName) {
        AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_title(page, title);
        adw_preferences_page_set_icon_name(page, iconName);
        adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);
        return page;
    };
    auto addGroup = [](AdwPreferencesPage *page, const char *title) {
        AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(group, title);
        adw_preferences_page_add(page, group);
        return group;
    };

    // --- General ---
    AdwPreferencesPage *generalPage = addPage("General", "preferences-system-symbolic");
    AdwPreferencesGroup *projectGroup = addGroup(generalPage, "Project");
    GtkSpinButton *autosaveRow = newNumberRow("Autosave delay (minutes)", 1.0, 30.0, 1.0);
    setTooltip(numberRowOf(autosaveRow), "settings.autosave-delay");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(numberRowOf(autosaveRow)),
                                "Minutes of inactivity after the last edit before an autosave is written");
    gtk_spin_button_set_digits(autosaveRow, 0);
    gtk_spin_button_set_value(autosaveRow, static_cast<double>(m_settings->autosaveDelayMinutes()));
    g_signal_connect(autosaveRow, "notify::value", G_CALLBACK(&AppWindow::settingsAutosaveDelayChangedTrampoline),
                     this);
    adw_preferences_group_add(projectGroup, numberRowOf(autosaveRow));

    GtkSpinButton *recentRow = newNumberRow("Recent projects list size", 1.0, 50.0, 1.0);
    setTooltip(numberRowOf(recentRow), "settings.recent-projects");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(numberRowOf(recentRow)),
                                "Entries shown in the header bar's recent-projects popover");
    gtk_spin_button_set_digits(recentRow, 0);
    gtk_spin_button_set_value(recentRow, static_cast<double>(m_settings->recentProjectsMax()));
    g_signal_connect(recentRow, "notify::value", G_CALLBACK(&AppWindow::settingsRecentProjectsMaxChangedTrampoline),
                     this);
    adw_preferences_group_add(projectGroup, numberRowOf(recentRow));

    AdwPreferencesGroup *shuttleGroup = addGroup(generalPage, "Shuttle");
    GtkSpinButton *shuttleRow = newNumberRow("Maximum shuttle speed", 2.0, 32.0, 1.0);
    setTooltip(numberRowOf(shuttleRow), "settings.shuttle-speed");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(numberRowOf(shuttleRow)),
                                "Upper bound (x normal speed) the J/K/L shuttle ramps up to");
    gtk_spin_button_set_digits(shuttleRow, 0);
    gtk_spin_button_set_value(shuttleRow, m_settings->shuttleMaxSpeed());
    g_signal_connect(shuttleRow, "notify::value", G_CALLBACK(&AppWindow::settingsShuttleMaxSpeedChangedTrampoline),
                     this);
    adw_preferences_group_add(shuttleGroup, numberRowOf(shuttleRow));

    // --- Toggles --- settingsToggleChangedTrampoline() finds the setting by
    // the key stored on the row.
    AdwPreferencesPage *togglesPage = addPage("Toggles", "checkbox-checked-symbolic");
    auto addToggle = [this](AdwPreferencesGroup *group, const char *key, const char *title, const char *hintKey,
                            bool value) -> GtkWidget * {
        GtkWidget *row = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        if (const HintSpec *hint = findHint(hintKey); hint && hint->detail)
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), hint->detail);
        setTooltip(row, hintKey);
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), value);
        g_object_set_data_full(G_OBJECT(row), "ustudio-setting", g_strdup(key), g_free);
        g_signal_connect(row, "notify::active", G_CALLBACK(&AppWindow::settingsToggleChangedTrampoline), this);
        adw_preferences_group_add(group, row);
        return row;
    };
    AdwPreferencesGroup *startupGroup = addGroup(togglesPage, "Startup");
    addToggle(startupGroup, "reopen-last-project", "Reopen last project on startup", "settings.reopen-last",
              m_settings->reopenLastProject());
    AdwPreferencesGroup *timelineGroup = addGroup(togglesPage, "Timeline");
    addToggle(timelineGroup, "snap-while-dragging", "Snap while dragging", "settings.snap", m_snapWhileDragging);
    addToggle(timelineGroup, "follow-playhead", "Follow playhead while playing", "settings.follow-playhead",
              m_followPlayhead);
    addToggle(timelineGroup, "show-timeline-thumbnails", "Show timeline thumbnails", "settings.timeline-thumbnails",
              m_showTimelineThumbnails);
    addToggle(timelineGroup, "show-waveforms", "Show waveforms", "settings.waveforms", m_showWaveforms);
    addToggle(timelineGroup, "show-hover-preview", "Thumbnails in clip tooltips", "settings.hover-preview",
              m_showHoverPreview);

    // --- Performance ---
    AdwPreferencesPage *performancePage = addPage("Performance", "power-profile-performance-symbolic");
    AdwPreferencesGroup *previewGroup = addGroup(performancePage, "Preview");
    const char *scaleLabels[] = {"Auto", "Full", "Half", "Quarter", nullptr};
    GtkStringList *scaleModel = gtk_string_list_new(scaleLabels);
    AdwComboRow *scaleRow = ADW_COMBO_ROW(adw_combo_row_new());
    adw_combo_row_set_model(scaleRow, G_LIST_MODEL(scaleModel));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scaleRow), "Default preview scale");
    setTooltip(GTK_WIDGET(scaleRow), "settings.preview-scale");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(scaleRow),
        "Used when the app starts -- the transport bar's own dropdown can still be changed per-session "
        "without affecting this");
    const std::string currentScale = m_settings->defaultPreviewScale();
    guint scaleIndex = 0;
    if (currentScale == "full")
        scaleIndex = 1;
    else if (currentScale == "half")
        scaleIndex = 2;
    else if (currentScale == "quarter")
        scaleIndex = 3;
    adw_combo_row_set_selected(scaleRow, scaleIndex);
    g_signal_connect(scaleRow, "notify::selected", G_CALLBACK(&AppWindow::settingsPreviewScaleChangedTrampoline), this);
    adw_preferences_group_add(previewGroup, GTK_WIDGET(scaleRow));
    addProxySettingsRow(previewGroup);
    // ADR-019: the GPU row's subtitle is its live status.
    AdwPreferencesGroup *hardwareGroup = addGroup(performancePage, "Hardware");
    m_gpuSettingsRow = addToggle(hardwareGroup, "gpu-acceleration", "GPU acceleration", "settings.gpu-acceleration",
                                 m_settings->gpuAcceleration());
    g_object_add_weak_pointer(G_OBJECT(m_gpuSettingsRow), reinterpret_cast<gpointer *>(&m_gpuSettingsRow));
    refreshGpuSettingsRow();
    addToggle(hardwareGroup, "hardware-decode", "Hardware video decoding", "settings.hardware-decode",
              m_settings->hardwareDecode());

    AdwPreferencesGroup *backgroundGroup = addGroup(performancePage, "Background work");
    // 0 is "Automatic (N)" (the output/input handlers below); the pool is
    // sized once, at startup.
    const double maxThreads = std::max(1.0, static_cast<double>(std::thread::hardware_concurrency()));
    GtkSpinButton *threadsRow = newNumberRow("Worker threads", 0.0, maxThreads, 1.0);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(numberRowOf(threadsRow)), "Applies after restart");
    setTooltip(numberRowOf(threadsRow), "settings.worker-threads");
    gtk_spin_button_set_digits(threadsRow, 0);
    g_signal_connect(threadsRow, "output", G_CALLBACK(&AppWindow::settingsWorkerThreadsOutputTrampoline), nullptr);
    g_signal_connect(threadsRow, "input", G_CALLBACK(&AppWindow::settingsWorkerThreadsInputTrampoline), nullptr);
    gtk_spin_button_set_value(threadsRow, static_cast<double>(m_settings->workerThreads()));
    // The row starts at 0, so setting 0 changes nothing and never re-runs
    // the output handler; the default width fits two digits, not the label;
    // and a numeric spin row (the default) drops the label's letters
    // (confirmed: gtk_editable_set_text left it empty until this was off).
    gtk_spin_button_set_numeric(threadsRow, FALSE);
    gtk_editable_set_width_chars(GTK_EDITABLE(threadsRow), 13);
    gtk_spin_button_update(threadsRow);
    g_signal_connect(threadsRow, "notify::value", G_CALLBACK(&AppWindow::settingsWorkerThreadsChangedTrampoline), this);
    adw_preferences_group_add(backgroundGroup, numberRowOf(threadsRow));

    // 0 is "Automatic" (half the worker threads); read when the window
    // creates its caches.
    GtkSpinButton *cacheJobsRow = newNumberRow("Thumbnail and waveform jobs", 0.0, maxThreads, 1.0);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(numberRowOf(cacheJobsRow)), "Applies after restart");
    setTooltip(numberRowOf(cacheJobsRow), "settings.cache-jobs");
    gtk_spin_button_set_digits(cacheJobsRow, 0);
    g_signal_connect(cacheJobsRow, "output", G_CALLBACK(&AppWindow::settingsCacheJobsOutputTrampoline), nullptr);
    g_signal_connect(cacheJobsRow, "input", G_CALLBACK(&AppWindow::settingsWorkerThreadsInputTrampoline), nullptr);
    gtk_spin_button_set_value(cacheJobsRow, static_cast<double>(m_settings->cacheJobs()));
    gtk_spin_button_set_numeric(cacheJobsRow, FALSE); // as the worker-threads row above
    gtk_editable_set_width_chars(GTK_EDITABLE(cacheJobsRow), 13);
    gtk_spin_button_update(cacheJobsRow);
    g_signal_connect(cacheJobsRow, "notify::value", G_CALLBACK(&AppWindow::settingsCacheJobsChangedTrampoline), this);
    adw_preferences_group_add(backgroundGroup, numberRowOf(cacheJobsRow));

    // --- Locations --- Each row keeps its key ("project"/"export") and its
    // clear button for the trampolines.
    AdwPreferencesPage *locationsPage = addPage("Locations", "folder-symbolic");
    AdwPreferencesGroup *foldersGroup = addGroup(locationsPage, "Folders");
    auto addFolderRow = [this, foldersGroup](const char *key, const char *title, const char *hintKey) {
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        setTooltip(GTK_WIDGET(row), hintKey);
        g_object_set_data_full(G_OBJECT(row), "ustudio-setting", g_strdup(key), g_free);
        GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
        gtk_widget_set_tooltip_text(clear, "Clear");
        gtk_widget_set_valign(clear, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(clear, "flat");
        g_object_set_data(G_OBJECT(clear), "ustudio-row", row);
        g_signal_connect(clear, "clicked", G_CALLBACK(&AppWindow::settingsClearFolderClickedTrampoline), this);
        g_object_set_data(G_OBJECT(row), "ustudio-clear", clear);
        GtkWidget *choose = gtk_button_new_from_icon_name("folder-open-symbolic");
        gtk_widget_set_tooltip_text(choose, "Choose Folder…");
        gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(choose, "flat");
        g_object_set_data(G_OBJECT(choose), "ustudio-row", row);
        g_signal_connect(choose, "clicked", G_CALLBACK(&AppWindow::settingsChooseFolderClickedTrampoline), this);
        adw_action_row_add_suffix(row, clear);
        adw_action_row_add_suffix(row, choose);
        adw_preferences_group_add(foldersGroup, GTK_WIDGET(row));
        const std::string current =
            std::string(key) == "project" ? m_settings->defaultProjectFolder() : m_settings->defaultExportFolder();
        setDefaultFolder(key, current, row);
    };
    addFolderRow("project", "Default project folder", "settings.project-folder");
    addFolderRow("export", "Default export folder", "settings.export-folder");

    // --- Render ---
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               buildRenderProfilesPage(*m_renderProfiles, *m_settings,
                                                       engine::h264HasQualityMode().value_or(true),
                                                       [this](int percent) { m_renderThreadsPercent = percent; }));

    // --- Drop-ins (doc 17) ---
    adw_preferences_dialog_add(
        ADW_PREFERENCES_DIALOG(dialog), buildDropInsPage(dropins::DropInRegistry::current(), *m_settings, [dialog] {
            adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
                                             adw_toast_new("Restart U-Stu to apply drop-in changes"));
        }));

    // --- Keyboard Shortcuts (placeholder -- see action_registry.h's own
    // comment on the intended shape of the real rebinding UI later) ---
    AdwPreferencesPage *shortcutsPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(shortcutsPage, "Shortcuts");
    adw_preferences_page_set_icon_name(shortcutsPage, "preferences-desktop-keyboard-shortcuts-symbolic");
    AdwPreferencesGroup *comingSoonGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(comingSoonGroup, "Coming Soon");
    adw_preferences_group_set_description(
        comingSoonGroup, "Rebinding actions to custom keys isn't implemented yet. See the Help dialog's "
                         "Keyboard Shortcuts tab for the bindings available today.");
    adw_preferences_page_add(shortcutsPage, comingSoonGroup);
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), shortcutsPage);

    if (!m_settings->isPersistent())
        adw_preferences_dialog_add_toast(
            ADW_PREFERENCES_DIALOG(dialog),
            adw_toast_new("Changes aren't being saved this session (GSettings schema not found)"));

    adw_dialog_present(dialog, GTK_WIDGET(m_window));
}

void AppWindow::onSettingsToggleChanged(const std::string &key, bool active)
{
    if (key == "reopen-last-project") {
        m_settings->setReopenLastProject(active);
        return;
    }
    if (key == "gpu-acceleration") {
        m_gpu->setEnabled(active);
        return;
    }
    if (key == "hardware-decode") {
        m_gpu->setHardwareDecode(active);
        return;
    }
    if (key == "snap-while-dragging") {
        m_snapWhileDragging = active;
        m_settings->setSnapWhileDragging(active);
    } else if (key == "follow-playhead") {
        m_followPlayhead = active;
        m_settings->setFollowPlayhead(active);
    } else if (key == "show-timeline-thumbnails") {
        m_showTimelineThumbnails = active;
        m_settings->setShowTimelineThumbnails(active);
    } else if (key == "show-waveforms") {
        m_showWaveforms = active;
        m_settings->setShowWaveforms(active);
    } else if (key == "show-hover-preview") {
        m_showHoverPreview = active;
        m_settings->setShowHoverPreview(active);
        if (!active)
            hideHoverPreview();
    }
    if (m_timeline)
        gtk_widget_queue_draw(m_timeline);
}

void AppWindow::onTimelineHover(double x, double y)
{
    // Showing the popover makes the compositor send a motion (and a
    // leave) at the same spot; treating that as a move hid it and re-armed
    // it, over and over (the tooltip flashed). Only a real move re-arms.
    const bool pending = m_hoverTimerId != 0 || (m_hoverPreview && gtk_widget_get_visible(GTK_WIDGET(m_hoverPreview)));
    if (pending && std::abs(x - m_hover.x) < 3 && std::abs(y - m_hover.y) < 3)
        return;
    // Moving re-arms the delay; the tooltip only shows once the pointer rests.
    hideHoverPreview();
    if (m_timelineController.mode() != timeline::TimelineController::Mode::None)
        return;
    const timeline::ContextTarget target = m_timelineController.contextTargetAt(timelineContext(), x, y);
    if (!m_model.hasClip(target.clip) || target.frame < 0)
        return;
    m_hover = HoverTarget{};
    m_hover.text = clipTooltipText(target.clip);
    if (m_hover.text.empty())
        return;
    m_hover.timelineFrame = target.frame;
    m_hover.x = x;
    m_hover.y = y;
    const core::Clip &clip = m_model.clip(target.clip);
    if (m_showHoverPreview && m_model.track(clip.track).kind == core::Track::Kind::Video && clip.videoEnabled &&
        m_model.hasAsset(clip.asset)) {
        const core::Asset &asset = m_model.asset(clip.asset);
        if (!asset.path.empty() && asset.info.hasVideo) {
            const core::FrameIndex offset =
                std::clamp<core::FrameIndex>(target.frame - clip.position, 0, clip.out - clip.in);
            m_hover.resource = asset.path;
            m_hover.sourceFrame = static_cast<int>(clip.in + offset);
        }
    }
    m_hoverTimerId = g_timeout_add(kHoverPreviewDelayMs, &AppWindow::hoverPreviewTimerTrampoline, this);
}

void AppWindow::showHoverPreview()
{
    gtk_label_set_text(m_hoverText, m_hover.text.c_str());
    gtk_label_set_text(m_hoverTimecode, formatTimecode(static_cast<int>(m_hover.timelineFrame)).c_str());
    gtk_widget_set_visible(GTK_WIDGET(m_hoverTimecode), !m_hover.resource.empty());
    gtk_widget_set_visible(GTK_WIDGET(m_hoverPicture), FALSE);
    m_hover.waiting = !m_hover.resource.empty();
    const GdkRectangle at{static_cast<int>(m_hover.x), static_cast<int>(m_hover.y), 1, 1};
    gtk_popover_set_pointing_to(m_hoverPreview, &at);
    gtk_popover_popup(m_hoverPreview);
    m_hoverShownAt = g_get_monotonic_time();
    showHoverPreviewIfReady();
}

void AppWindow::showHoverPreviewIfReady()
{
    if (!m_hover.waiting || !m_hoverThumbnails)
        return;
    const core::Rational fps = m_model.sequence().profile.fps;
    const engine::ThumbnailCache::Data *data =
        m_hoverThumbnails->frameThumbnail(m_hover.resource, m_hover.sourceFrame, fps.num, fps.den);
    if (!data)
        return; // the cache's onReady calls back here
    GdkTexture *texture = m_hoverTextures.get(m_hover.resource + '\n' + std::to_string(m_hover.sourceFrame) + '@' +
                                                  std::to_string(fps.num) + '/' + std::to_string(fps.den),
                                              data->rgba, data->width, data->height);
    if (!texture)
        return;
    m_hover.waiting = false;
    gtk_picture_set_paintable(m_hoverPicture, GDK_PAINTABLE(texture));
    gtk_widget_set_size_request(GTK_WIDGET(m_hoverPicture), data->width, data->height);
    gtk_widget_set_visible(GTK_WIDGET(m_hoverPicture), TRUE);
}

void AppWindow::hideHoverPreview()
{
    if (m_hoverTimerId != 0) {
        g_source_remove(m_hoverTimerId);
        m_hoverTimerId = 0;
    }
    m_hover.waiting = false;
    if (m_hoverPreview && gtk_widget_get_visible(GTK_WIDGET(m_hoverPreview)))
        gtk_popover_popdown(m_hoverPreview);
}

void AppWindow::onProjectFrameRateClicked()
{
    const core::Rational current = m_model.sequence().profile.fps;
    std::vector<core::Rational> rates;
    GtkStringList *labels = gtk_string_list_new(nullptr);
    guint selected = 0;
    for (const core::Rational &rate : core::renderFrameRates()) {
        if (rate.num <= 0)
            continue; // "Project" means nothing here
        if (static_cast<int64_t>(rate.num) * current.den == static_cast<int64_t>(current.num) * rate.den)
            selected = static_cast<guint>(rates.size());
        rates.push_back(rate);
        gtk_string_list_append(labels, (core::formatFps(rate) + " fps").c_str());
    }
    GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(labels), nullptr);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);

    // The background: the colour wherever no clip covers the frame.
    const uint32_t background = m_model.sequence().background;
    GdkRGBA rgba{static_cast<float>(background >> 16 & 0xff) / 255.0f,
                 static_cast<float>(background >> 8 & 0xff) / 255.0f, static_cast<float>(background & 0xff) / 255.0f,
                 1.0f};
    GtkColorDialog *colorDialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(colorDialog, FALSE);
    gtk_color_dialog_set_title(colorDialog, "Background colour");
    GtkWidget *colorButton = gtk_color_dialog_button_new(colorDialog); // takes colorDialog
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
    gtk_widget_set_tooltip_text(colorButton, "The colour wherever no clip covers the frame, in preview and export");
    GtkWidget *colorLabel = gtk_label_new("Background");
    gtk_widget_set_hexpand(colorLabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(colorLabel), 0.0f);
    GtkWidget *colorRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(colorRow), colorLabel);
    gtk_box_append(GTK_BOX(colorRow), colorButton);
    GtkWidget *extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_append(GTK_BOX(extra), dropdown);
    gtk_box_append(GTK_BOX(extra), colorRow);

    AdwDialog *dialog = adw_alert_dialog_new(
        "Project frame rate and background",
        ("Now " + core::formatFps(current) +
         " fps. Changing it keeps every clip, dissolve, fade and marker at its time: positions move to the "
         "nearest frame at the new rate. You can undo either change.")
            .c_str());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), extra);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "change", "Apply");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "change", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct Context
    {
        AppWindow *self;
        std::vector<core::Rational> rates;
        GtkDropDown *dropdown;
        GtkColorDialogButton *colorButton;
    };
    auto *ctx = new Context{this, std::move(rates), GTK_DROP_DOWN(g_object_ref(dropdown)),
                            GTK_COLOR_DIALOG_BUTTON(g_object_ref(colorButton))};
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<Context> owned(static_cast<Context *>(userData));
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            const guint index = gtk_drop_down_get_selected(owned->dropdown);
            const GdkRGBA *picked = gtk_color_dialog_button_get_rgba(owned->colorButton);
            auto channel = [](float value) {
                return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            };
            const uint32_t colour = channel(picked->red) << 16 | channel(picked->green) << 8 | channel(picked->blue);
            g_object_unref(owned->dropdown);
            g_object_unref(owned->colorButton);
            if (response != "change")
                return;
            AppWindow *self = owned->self;
            std::string status;
            // Each change is its own undo step.
            if (self->m_undoStack.execute(std::make_unique<core::SetSequenceBackground>(colour)))
                status = "The background is now " + core::backgroundHex(colour) + ". ";
            if (index < owned->rates.size()) {
                const core::Rational fps = owned->rates[index];
                if (self->m_undoStack.execute(std::make_unique<core::ChangeSequenceFrameRate>(fps)))
                    status += "The project is now " + core::formatFps(fps) + " fps.";
                else if (status.empty())
                    status = "Nothing changed: the project is already " + core::formatFps(fps) + " fps.";
            }
            if (!status.empty())
                self->showStatus(status);
        },
        ctx);
    // No unref of `labels`: gtk_drop_down_new() took it (transfer full).
    // Dropping it here too freed the model under the dialog's dropdown,
    // and the dialog's delayed dispose crashed in g_list_model_get_n_items
    // (the demo tour's frame-rate change then undo, 0.50.0-beta.4).
}

void AppWindow::setDefaultFolder(const std::string &key, const std::string &folder, AdwActionRow *row)
{
    if (key == "project")
        m_settings->setDefaultProjectFolder(folder);
    else
        m_settings->setDefaultExportFolder(folder);
    std::string subtitle = folder;
    if (folder.empty())
        subtitle =
            key == "project" ? "Not set: the file chooser decides" : "Not set: the project's folder, then Videos";
    adw_action_row_set_subtitle(row, subtitle.c_str());
    if (auto *clear = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(row), "ustudio-clear")))
        gtk_widget_set_visible(clear, !folder.empty());
}

void AppWindow::chooseDefaultFolder(const std::string &key, AdwActionRow *row)
{
    struct FolderContext
    {
        AppWindow *self;
        std::string key;
        AdwActionRow *row; // a ref is held until the dialog finishes
    };
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, key == "project" ? "Default Project Folder" : "Default Export Folder");
    auto *ctx = new FolderContext{this, key, ADW_ACTION_ROW(g_object_ref(row))};
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<FolderContext> owned(static_cast<FolderContext *>(userData));
            GError *error = nullptr;
            GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
            if (folder) {
                std::string path = localPathFor(folder, false);
                if (!path.empty())
                    owned->self->setDefaultFolder(owned->key, path, owned->row);
                g_object_unref(folder);
            }
            if (error)
                g_error_free(error);
            g_object_unref(owned->row);
        },
        ctx);
    g_object_unref(dialog);
}

void AppWindow::setTransportActionsEnabled(bool enabled)
{
    m_inlineEditOpen = !enabled;
    applyTransportActionsEnabled();
}

void AppWindow::onFocusWidgetChanged()
{
    // Entries, spin buttons and AdwEntryRow all type through an inner
    // GtkText; a GtkTextView is the multi-line case.
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(m_window));
    // A read-only one (a copyable path) types nothing, so it keeps them.
    m_textHasFocus = (focus && GTK_IS_TEXT(focus) && gtk_editable_get_editable(GTK_EDITABLE(focus))) ||
                     (focus && GTK_IS_TEXT_VIEW(focus) && gtk_text_view_get_editable(GTK_TEXT_VIEW(focus)));
    applyTransportActionsEnabled();
}

void AppWindow::addTextFocusRelease(GtkWidget *widget)
{
    // Capture phase, any button, never claimed: the widget's own gestures
    // still see the press.
    GtkGesture *press = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
    g_signal_connect(press, "pressed", G_CALLBACK(&AppWindow::textFocusReleaseTrampoline), this);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(press));
}

void AppWindow::applyTransportActionsEnabled()
{
    const bool enabled = !m_inlineEditOpen && !m_textHasFocus;
    if (enabled == m_transportActionsEnabled)
        return;
    m_transportActionsEnabled = enabled;
    Log::debug(std::string("[app] single-key shortcuts ") + (enabled ? "on" : "off (text entry)"));
    static const char *kTransportActions[] = {
        "shuttle-forward",
        "shuttle-reverse",
        "shuttle-stop",
        "step-forward",
        "step-backward",
        "seek-home",
        "seek-end",
        "loop-set-in",
        "loop-set-out",
        "seek-previous-cut",
        "seek-next-cut",
        "active-track-up",
        "seek-previous-cut-on-active-track",
        "seek-next-cut-on-active-track",
        "active-track-top",
        "active-track-bottom",
        "active-track-down",
        "step-forward-10",
        "step-backward-10",
        "step-forward-minute",
        "step-backward-minute",
        // Enhancements #1/#3: bare Space and Delete, and X (split at
        // playhead), same reasoning as every action above -- a text entry
        // needs all three for perfectly ordinary typing (a space or an
        // "x" in a track/clip name, Delete removing a character), so
        // they're disabled here too. Ctrl+S/Shift+S/O/N/I
        // below are deliberately NOT in this list, same as Ctrl+Z/Shift+Z:
        // modifier combos a text entry never needs for itself.
        "play-pause",
        "delete-selected-clip",
        "split-at-playhead",
        // + - = 0 are ordinary characters in a name entry.
        "zoom-in",
        "zoom-out",
        "zoom-fit",
        // Ctrl+A selects the entry's text; Escape cancels the rename.
        "select-all",
        "clear-selection",
        // Shift+Delete cuts text; m and M are letters.
        "ripple-delete-selected",
        "add-marker",
        "remove-marker",
        // Tab moves between fields; , . and r are characters.
        "select-next-clip",
        "select-previous-clip",
        "nudge-left",
        "nudge-right",
        "nudge-left-10",
        "nudge-right-10",
        "ripple-mode",
    };
    for (const char *name : kTransportActions) {
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(m_window), name);
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
    for (const std::string &name : m_typingKeyActions)
        if (GAction *action = g_action_map_lookup_action(G_ACTION_MAP(m_window), name.c_str()))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

namespace {
// Enhancement #6: video/audio/image, matching what makeImportedAsset()
// above and probeMedia() actually handle -- "*" wildcards are supported
// by gtk_file_filter_add_mime_type() per its own documentation ("could
// be a pattern with '*'"), so this is three patterns, not an exhaustive
// per-codec MIME list. An "All Files" fallback keeps anything with an
// unrecognised/missing MIME type (some still-image formats on certain
// systems) reachable rather than hidden.
GtkFileFilter *newMediaFilter()
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Media (video, audio, images)");
    gtk_file_filter_add_mime_type(filter, "video/*");
    gtk_file_filter_add_mime_type(filter, "audio/*");
    gtk_file_filter_add_mime_type(filter, "image/*");
    return filter;
}
} // namespace

void AppWindow::onImportClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Media");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *mediaFilter = newMediaFilter();
    for (const ImportHandler &handler : m_importHandlers) // IP5: a drop-in's files are media too
        for (const std::string &extension : handler.extensions)
            gtk_file_filter_add_suffix(mediaFilter, extension.c_str());
    g_list_store_append(filters, mediaFilter);
    g_object_unref(mediaFilter);
    for (const ImportHandler &handler : m_importHandlers) { // and each on its own
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, handler.description.c_str());
        for (const std::string &extension : handler.extensions)
            gtk_file_filter_add_suffix(filter, extension.c_str());
        g_list_store_append(filters, filter);
        g_object_unref(filter);
    }
    GtkFileFilter *allFilter = gtk_file_filter_new();
    gtk_file_filter_set_name(allFilter, "All Files");
    gtk_file_filter_add_pattern(allFilter, "*");
    g_list_store_append(filters, allFilter);
    g_object_unref(allFilter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);

    // Enhancement #5: multi-select.
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::fileOpenedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onImportFolderClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Folder");
    // Into the bin: a folder is usually more than belongs on one track.
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            auto *self = static_cast<AppWindow *>(userData);
            GError *error = nullptr;
            GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
            if (!folder) {
                if (error)
                    g_error_free(error);
                return;
            }
            std::string path = localPathFor(folder);
            g_object_unref(folder);
            if (!path.empty())
                self->startImport({std::move(path)}, std::nullopt, std::nullopt);
        },
        this);
    g_object_unref(dialog);
}

void AppWindow::onImportImageSequenceClicked()
{
    // M4 E: an explicit command, never guessed on import (camera photos
    // are numbered too). Any file of the sequence will do.
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Image Sequence: pick any of its images");
    GtkFileFilter *images = gtk_file_filter_new();
    gtk_file_filter_set_name(images, "Images");
    gtk_file_filter_add_mime_type(images, "image/*");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, images);
    g_object_unref(images);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            GError *error = nullptr;
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
            if (!file) {
                if (error)
                    g_error_free(error);
                return;
            }
            std::string path = localPathFor(file);
            g_object_unref(file);
            if (!path.empty())
                static_cast<AppWindow *>(userData)->importImageSequence(path);
        },
        this);
    g_object_unref(dialog);
}

void AppWindow::importImageSequence(const std::string &pickedFile)
{
    // The folder scan and the probe of its first image run on the pool.
    const uint64_t generation = m_projectGeneration;
    m_pool->submit([this, pickedFile, profile = m_model.sequence().profile, generation,
                    token = std::weak_ptr<void>(m_lifetime)](std::stop_token) {
        std::optional<core::ImageSequence> sequence = core::findImageSequence(pickedFile);
        engine::EngineSync::ProbedMedia probed;
        if (sequence) {
            const std::string first = core::imageSequenceFile(sequence->pattern, sequence->begin);
            probed = engine::EngineSync::probeMediaFile(profile, first);
            probed.fingerprint = core::fileFingerprint(first);
        }
        engine::MainThreadDispatcher::post(token, [this, pickedFile, sequence, probed, generation] {
            if (generation != m_projectGeneration)
                return;
            const std::string name = core::utf8String(core::pathFromUtf8(pickedFile).filename());
            if (!sequence) {
                showStatus(name + " isn't part of a numbered sequence (its neighbours need the same name, "
                                  "numbered on without a gap).");
                return;
            }
            if (probed.length <= 0 || probed.width <= 0) {
                showStatus("Couldn't open the images of " + sequence->displayName + ".");
                return;
            }
            const core::Rational fps = m_model.sequence().profile.fps;
            core::Asset asset = makeImportedAsset(sequence->pattern, sequence->count, probed, fps);
            asset.displayName = sequence->displayName;
            asset.info.isStillImage = false;
            asset.info.isImageSequence = true;
            asset.info.sequenceBegin = sequence->begin;
            asset.info.hasAudio = false;
            if (m_undoStack.execute(std::make_unique<core::AddAsset>(std::move(asset)))) {
                showStatus("Imported " + sequence->displayName + ": " + std::to_string(sequence->count) +
                           " images, one per frame. Drag it to the timeline.");
                queueRefresh();
            }
        });
    });
}

void AppWindow::showImportReport(size_t imported, size_t total, const std::vector<std::string> &failures,
                                 const std::string &notes)
{
    const std::string summary = "Imported " + std::to_string(imported) + " of " + std::to_string(total) + " files.";
    showStatus(summary + " " + std::to_string(failures.size()) + " couldn't be imported." + notes);
    AdwDialog *dialog = adw_alert_dialog_new(summary.c_str(), nullptr);
    adw_alert_dialog_format_body(
        ADW_ALERT_DIALOG(dialog), "%s",
        (std::to_string(failures.size()) +
         (failures.size() == 1 ? " file couldn't be imported:" : " files couldn't be imported:") + notes)
            .c_str());
    std::string list;
    for (const std::string &failure : failures)
        list += (list.empty() ? "" : "\n") + failure;
    GtkWidget *label = gtk_label_new(list.c_str());
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "import-report");
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 260);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), label);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroller);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "OK");
    adw_dialog_present(dialog, GTK_WIDGET(m_window));
}

void AppWindow::onFileOpened(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!files) {
        if (error) {
            Log::debug(std::string("[app] Import file dialog closed without a selection: ") + error->message);
            g_error_free(error);
        }
        return;
    }

    if (m_model.sequence().tracks.empty()) {
        showStatus("Add a track first.");
        g_object_unref(files);
        return;
    }

    std::vector<std::string> paths;
    guint n = g_list_model_get_n_items(files);
    for (guint i = 0; i < n; ++i) {
        auto *file = static_cast<GFile *>(g_list_model_get_item(files, i));
        std::string path = localPathFor(file);
        if (!path.empty())
            paths.push_back(std::move(path));
        g_object_unref(file);
    }
    g_object_unref(files);
    Log::debug("[import] dialog picked " + std::to_string(n) + " file(s), " + std::to_string(paths.size()) +
               " with a local path");
    startImport(std::move(paths), trackIdForRow(m_activeTrack), std::nullopt);
}

void AppWindow::startImport(std::vector<std::string> paths, std::optional<core::TrackId> trackId,
                            std::optional<core::FrameIndex> position)
{
    // IP5: files a drop-in opens itself go to it first, in order; the rest
    // follow them on the track.
    if (!m_importHandlers.empty())
        paths = importWithHandlers(std::move(paths), trackId, position);
    if (paths.empty() || !m_importQueue)
        return;
    // Folders (Import Folder…, or dropped from Files) are walked on the
    // pool, then imported as their files. One stat each here.
    std::error_code ec;
    if (std::any_of(paths.begin(), paths.end(), [&](const std::string &path) {
            return std::filesystem::is_directory(core::pathFromUtf8(path), ec);
        })) {
        showStatus("Looking for files to import…");
        m_pool->submit([this, paths = std::move(paths), trackId, position, generation = m_projectGeneration,
                        token = std::weak_ptr<void>(m_lifetime)](std::stop_token) {
            bool truncated = false;
            std::vector<std::string> files = expandImportPaths(paths, kImportFileLimit, &truncated);
            engine::MainThreadDispatcher::post(
                token, [this, files = std::move(files), truncated, trackId, position, generation]() mutable {
                    if (generation != m_projectGeneration)
                        return; // another project now
                    if (files.empty()) {
                        showStatus("There are no files to import there.");
                        return;
                    }
                    if (truncated)
                        m_importNotes.push_back("Only the first " + std::to_string(kImportFileLimit) +
                                                " files were imported.");
                    startImport(std::move(files), trackId, position);
                });
        });
        return;
    }
    struct State
    {
        std::optional<core::FrameIndex> position;
        size_t imported = 0;
        std::string lastImported;
        std::vector<std::string> failures;
    };
    auto state = std::make_shared<State>();
    state->position = position;
    const size_t total = paths.size();
    // Every file of this import joins one undo step (CompositeCommand's
    // merge key), not one per file.
    const uint64_t batchKey = ++m_importBatchSerial;
    showStatus(total == 1 ? "Importing " + paths.front() + "…" : "Importing 0 of " + std::to_string(total) + "…");

    m_importQueue->start(
        std::move(paths),
        // Pool thread: a copy of the profile, nothing of the live project.
        // Checks what a probe can't say clearly first, and takes the
        // fingerprint relink will need (doc 07).
        [profile = m_model.sequence().profile](const std::string &path) {
            engine::EngineSync::ProbedMedia probed;
            std::error_code statError;
            const std::filesystem::path file = core::pathFromUtf8(path);
            if (std::filesystem::is_directory(file, statError)) {
                probed.error = "it's a folder";
                return probed;
            }
            const auto size = std::filesystem::file_size(file, statError);
            if (statError) {
                probed.error = "it can't be read";
                return probed;
            }
            if (size == 0) {
                probed.error = "it's empty (0 bytes)";
                return probed;
            }
            probed = engine::EngineSync::probeMediaFile(profile, path);
            if (probed.length <= 0)
                probed.error = "it isn't a video, audio or image file U-Stu can open";
            probed.fingerprint = core::fileFingerprint(path);
            return probed;
        },
        [this, state, trackId, batchKey](size_t, const std::string &path,
                                         const engine::EngineSync::ProbedMedia &probed) {
            std::string failure;
            if (!probed.error.empty() || probed.length <= 0) {
                failure = core::utf8String(core::pathFromUtf8(path).filename()) + ": " +
                          (probed.error.empty() ? "it couldn't be opened" : probed.error);
            } else if (!trackId) {
                if (auto added = importProbedAssetOnly(path, probed, batchKey); !added)
                    failure = added.error();
            } else if (!m_model.hasTrack(*trackId)) {
                failure = "Can't import " + path + ": its track was deleted.";
            } else {
                const core::Track &track = m_model.track(*trackId);
                core::FrameIndex at = state->position       ? *state->position
                                      : track.clips.empty() ? 0
                                                            : m_model.clip(track.clips.back()).end();
                if (auto end = importProbedToTrack(path, probed, *trackId, at, batchKey)) {
                    if (state->position)
                        state->position = *end;
                } else {
                    failure = end.error();
                }
            }
            if (failure.empty()) {
                ++state->imported;
                state->lastImported = path;
            } else {
                Log::warn("[import] " + failure);
                state->failures.push_back(std::move(failure));
            }
        },
        [this, total](size_t finished, size_t) {
            if (total > 1)
                showStatus("Importing " + std::to_string(finished) + " of " + std::to_string(total) + "…");
        },
        [this, state, total] {
            // Format notes (doc 13 R7) ride on the summary: the first video
            // set the project's format, or a clip's rate differs from it.
            std::string notes;
            std::vector<std::string> collected = std::exchange(m_importNotes, {});
            // Grouped by rate, one short sentence however many files.
            if (std::string rates =
                    core::frameRateSummary(std::exchange(m_importRates, {}), m_model.sequence().profile.fps);
                !rates.empty())
                collected.push_back(std::move(rates));
            for (const std::string &note : collected)
                notes += " " + note + (note.ends_with(".") ? "" : ".");
            if (state->failures.empty())
                showStatus((total == 1 ? "Imported: " + state->lastImported
                                       : "Imported " + std::to_string(total) + " files.") +
                           notes);
            else if (total == 1)
                showStatus("Couldn't import " + state->failures.front());
            else
                showImportReport(state->imported, total, state->failures, notes);
            offerProxiesAfterImport();
        });
}

std::expected<core::FrameIndex, std::string>
AppWindow::importProbedToTrack(const std::string &path, const engine::EngineSync::ProbedMedia &probed,
                               core::TrackId trackId, core::FrameIndex position, uint64_t batchKey)
{
    // A still image is boundless (MediaInfo::isBoundless()) -- MLT's own
    // default (15000 frames via pixbuf, verified empirically) has
    // nothing to do with how long a clip cut from it should be. Default
    // to spanning the rest of the *current* project length from the
    // insert point, so dropping a logo/watermark PNG onto an otherwise-
    // empty top track immediately covers the whole timeline, matching
    // what a still image is for -- no manual trim-to-fit needed. Falls
    // back to a modest 10s default when there's nothing yet to cover (an
    // empty project, or inserting past the current end).
    std::vector<std::unique_ptr<core::Command>> steps;
    std::optional<core::Profile> adopt = profileToAdopt(probed);
    if (adopt)
        steps.push_back(std::make_unique<core::SetSequenceProfile>(*adopt));
    const core::Rational fps = adopt ? adopt->fps : m_model.sequence().profile.fps;
    const core::FrameIndex probedLength = lengthAtRate(probed, fps);
    if (adopt)
        position = core::retimeFrame(position, m_model.sequence().profile.fps, fps);
    core::FrameIndex length = effectiveInsertLength(probed.isStillImage, probedLength, position);

    if (!m_model.isRangeFree(trackId, position, position + length))
        return std::unexpected("Can't import " + path + " there: it would overlap another clip.");

    // AddAsset applies first (below) and, with no reuseId, allocates
    // exactly model.project().nextId as read here -- nothing else can
    // allocate an id between this read and that apply(), so InsertClip
    // can be built against it up front even though AddAsset hasn't run
    // yet.
    core::AssetId predictedAssetId{m_model.project().nextId};

    steps.push_back(std::make_unique<core::AddAsset>(makeImportedAsset(path, length, probed, fps)));
    steps.push_back(std::make_unique<core::InsertClip>(trackId, predictedAssetId, position, 0, length - 1));

    auto composite =
        std::make_unique<core::CompositeCommand>("Import clip", std::move(steps), batchKey, "Import files");

    if (!m_undoStack.execute(std::move(composite)))
        return std::unexpected("Could not import: " + path);
    noteImportedRate(path, probed, adopt.has_value());
    queueRefresh();
    return position + length;
}

std::optional<core::Profile> AppWindow::profileToAdopt(const engine::EngineSync::ProbedMedia &probed) const
{
    if (probed.isStillImage || probed.width <= 0 || probed.height <= 0 || probed.fps.num <= 0 || probed.fps.den <= 0 ||
        !core::sequenceTakesProfileFromMedia(m_model.project()))
        return std::nullopt;
    core::Profile profile = core::profileForMedia(m_model.sequence().profile, probed.width, probed.height, probed.fps);
    if (profile == m_model.sequence().profile)
        return std::nullopt;
    return profile;
}

core::FrameIndex AppWindow::lengthAtRate(const engine::EngineSync::ProbedMedia &probed, core::Rational fps) const
{
    if (probed.sequenceFps.num <= 0 || probed.sequenceFps.den <= 0 || probed.sequenceFps == fps)
        return probed.length;
    return std::max<core::FrameIndex>(1, core::retimeFrame(probed.length, probed.sequenceFps, fps));
}

void AppWindow::noteImportedRate(const std::string &path, const engine::EngineSync::ProbedMedia &probed, bool adopted)
{
    const std::string name = core::utf8String(core::pathFromUtf8(path).filename());
    const core::Profile &profile = m_model.sequence().profile;
    if (adopted) {
        m_importNotes.push_back("The project now matches " + name + ": " + std::to_string(profile.width) + "×" +
                                std::to_string(profile.height) + " at " + core::formatFps(profile.fps) + " fps.");
        return;
    }
    if (!probed.isStillImage)
        m_importRates.emplace_back(name, probed.fps);
}

std::expected<void, std::string> AppWindow::importProbedAssetOnly(const std::string &path,
                                                                  const engine::EngineSync::ProbedMedia &probed,
                                                                  uint64_t batchKey)
{
    std::vector<std::unique_ptr<core::Command>> steps;
    std::optional<core::Profile> adopt = profileToAdopt(probed);
    if (adopt)
        steps.push_back(std::make_unique<core::SetSequenceProfile>(*adopt));
    const core::Rational fps = adopt ? adopt->fps : m_model.sequence().profile.fps;
    core::FrameIndex length = effectiveInsertLength(probed.isStillImage, lengthAtRate(probed, fps), 0);
    steps.push_back(std::make_unique<core::AddAsset>(makeImportedAsset(path, length, probed, fps)));
    if (!m_undoStack.execute(
            std::make_unique<core::CompositeCommand>("Import media", std::move(steps), batchKey, "Import files")))
        return std::unexpected("Could not import: " + path);
    noteImportedRate(path, probed, adopt.has_value());
    queueRefresh();
    return {};
}

void AppWindow::queueRefresh()
{
    // One refresh for a whole burst of imports, not one per file: each
    // refreshMediaBrowser() rebuilds every row, so fifty imports cost
    // fifty growing rebuilds (17 ms each by the end, doc 19 MT2). Idle
    // priority runs after the paced applies (G_PRIORITY_DEFAULT posts).
    // While an import is still applying, at most every 400 ms: applies are
    // paced one per iteration, so an idle refresh ran between nearly every
    // pair, and at 190 rows a rebuild takes 20-36 ms (M4 A, 200 files).
    if (m_refreshSourceId != 0)
        return;
    if (m_importQueue && m_importQueue->activeBatches() > 0)
        m_refreshSourceId = g_timeout_add(kImportRefreshIntervalMs, &AppWindow::queuedRefreshTrampoline, this);
    else
        m_refreshSourceId =
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &AppWindow::queuedRefreshTrampoline, this, nullptr);
}

void AppWindow::cancelProjectJobs()
{
    ++m_projectGeneration;
    if (m_importQueue)
        m_importQueue->cancelAll();
    if (m_projectLoader)
        m_projectLoader->cancel();
}

void AppWindow::onSaveQueueSettled()
{
    if (m_closeWhenSaved && m_saveQueue && !m_saveQueue->busy()) {
        m_closeWhenSaved = false;
        gtk_window_close(GTK_WINDOW(m_window)); // clean now, or asks as usual
    }
}

void AppWindow::onSaveClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Project");
    gtk_file_dialog_set_initial_name(dialog, "project.ustudio");
    setInitialProjectFolder(dialog);
    gtk_file_dialog_save(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::saveFinishedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onSaveFinished(GObject *sourceObject, GAsyncResult *result)
{
    // Audit A2: consumed unconditionally, regardless of how this save
    // turns out -- a cancelled or refused save must NOT close the window
    // (the user is left in the editor to sort it out), and clearing it up
    // front means a later, unrelated save can never inherit a stale
    // "close when done" from this one.
    bool closeAfterSave = m_closeAfterSave;
    m_closeAfterSave = false;

    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }

    std::string path = localPathFor(file, true);
    if (!path.empty())
        performSaveToPath(path, closeAfterSave);
    g_object_unref(file);
}

bool AppWindow::performSaveToPath(const std::string &requestedPath, bool closeAfterSave)
{
    // Ctrl+S reuses m_currentProjectPath, which a project opened before the
    // portal fix (or via an old recent-projects entry) may still hold as a
    // document-portal path -- see portal_path.h.
    const std::string path = portal::resolveHostPath(requestedPath);
    if (pathIsProjectAsset(path)) {
        showStatus("Refusing to save over a file already in this project's media: " + path);
        return false;
    }
    if (!m_saveQueue)
        return false; // shutting down
    // The snapshot and the undo state are taken now; the write lands later,
    // and only marks clean what it actually wrote.
    std::shared_ptr<const core::Project> snapshot = m_model.snapshot();
    core::UndoStack::State savedState = m_undoStack.state();
    uint64_t generation = m_projectGeneration;
    showStatus("Saving " + path + "…");
    m_saveQueue->save(
        [snapshot, path] {
            core::Model model(*snapshot);
            // Audit T1 stop-gap: saveProject() doesn't run check() itself,
            // and loadProject() refuses any file that fails it -- writing an
            // invalid model out would produce a file that looks saved but
            // can never be reopened. Checked here rather than inside
            // saveProject() (used by autosave and render too, where refusing
            // outright would be worse than writing best-effort) so this is
            // the one place a human sees the message. On the pool: ~50 ms
            // for 5,000 clips.
            std::vector<std::string> problems = model.check();
            if (!problems.empty()) {
                Log::error("[app] refusing to save an invalid model: " + problems.front());
                return "Can't save: the project has an internal inconsistency (" + problems.front() +
                       "). This is a bug -- please report it.";
            }
            // Explicit saves only (autosave writes its own file). A failed
            // backup doesn't stop the save the user asked for.
            std::string backupError = core::backupBeforeOverwrite(path, std::time(nullptr));
            if (!backupError.empty())
                Log::warn("[app] " + backupError);
            return core::saveProject(model, path);
        },
        [this, path, savedState, generation, closeAfterSave](const std::string &error) {
            if (!error.empty()) {
                showStatus(error);
                onSaveQueueSettled();
                return;
            }
            if (generation == m_projectGeneration) {
                const std::string previousPath = std::exchange(m_currentProjectPath, path);
                m_undoStack.setCleanPoint(savedState); // emits changed -- updateWindowTitle() follows
                // The untitled (or old path's) autosave is stale once the work
                // has a name, and this path's too if nothing changed since.
                removeStaleAutosaves(m_ownAutosaves->saved(previousPath, path, m_undoStack.isClean()));
                // A successful manual Save is the one point A2 designates
                // safe to remove a recovered autosave: the recovered content
                // now has a durable copy of its own at `path`.
                if (!m_pendingAutosaveCleanupPath.empty()) {
                    std::remove(m_pendingAutosaveCleanupPath.c_str());
                    std::remove(m_pendingAutosaveCleanupMetaPath.c_str());
                    m_pendingAutosaveCleanupPath.clear();
                    m_pendingAutosaveCleanupMetaPath.clear();
                }
            }
            showStatus("Saved: " + path);
            recordRecentProject(path); // enhancement #15
            if (closeAfterSave)
                gtk_window_destroy(GTK_WINDOW(m_window));
            else
                onSaveQueueSettled();
        });
    return true;
}

void AppWindow::saveInPlaceOrPrompt(bool closeAfterSave)
{
    if (m_currentProjectPath.empty()) {
        m_closeAfterSave = closeAfterSave;
        onSaveClicked();
        return;
    }
    performSaveToPath(m_currentProjectPath, closeAfterSave);
}

void AppWindow::onOpenProjectClicked()
{
    confirmDiscardIfDirty([this] {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Open Project");
        setInitialProjectFolder(dialog);

        // Enhancement #6.
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        GtkFileFilter *projectFilter = gtk_file_filter_new();
        gtk_file_filter_set_name(projectFilter, "U-Stu Projects (*.ustudio)");
        gtk_file_filter_add_suffix(projectFilter, "ustudio");
        g_list_store_append(filters, projectFilter);
        g_object_unref(projectFilter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        g_object_unref(filters);

        gtk_file_dialog_open(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::openProjectFinishedTrampoline, this);
        g_object_unref(dialog);
    });
}

void AppWindow::onOpenProjectFinished(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }

    std::string path = localPathFor(file);
    if (!path.empty())
        loadProjectFromPath(path);
    g_object_unref(file);
}

void AppWindow::loadProjectFromPath(const std::string &requestedPath)
{
    // Covers recent-projects entries recorded as document-portal paths
    // before portal::resolveHostPath() existed, not just fresh dialog picks.
    const std::string path = portal::resolveHostPath(requestedPath);
    showStatus("Opening " + path + "…");
    loadProjectAsync(
        path,
        [this, path](core::Model model) {
            replaceProject(std::move(model));
            m_currentProjectPath = path;
            m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
            // Audit A1: a pending recovered-autosave cleanup is only
            // safe to act on once ITS OWN content has been durably
            // saved (onSaveFinished()'s own comment) -- switching away
            // to a different project via Open, same as New or Reload,
            // must forget it rather than let a later Save of THIS
            // project delete the still-only copy of whatever was
            // recovered. The files themselves are left alone; a later
            // launch (or offerRecoveryIfAny() looping later in this one)
            // can still find and offer them.
            m_pendingAutosaveCleanupPath.clear();
            m_pendingAutosaveCleanupMetaPath.clear();
            refreshTimeline();
            refreshMediaBrowser();
            showStatus("Opened: " + path + unplayedEffectsNotice() + missingMediaNotice());
            offerMissingMediaHelp(); // one dialog with every way to find them
            // Enhancement #15: recorded regardless of how the project got
            // opened (dialog or the recent-projects menu itself), so
            // re-opening it later keeps bumping it back to the top.
            recordRecentProject(path);
        },
        [this, path](const core::ProjectLoadError &error) { showProjectLoadError(path, error); });
}

void AppWindow::showProjectLoadError(const std::string &path, const core::ProjectLoadError &error)
{
    using Kind = core::ProjectLoadError::Kind;
    const std::string name = std::filesystem::path(path).filename().string();
    std::string heading, body;
    switch (error.kind) {
    case Kind::Missing:
        heading = "Project not found";
        body = "There's no file at this path any more: it may have been moved, renamed or deleted. Copies from "
               "earlier saves are kept in the .ustudio-backups folder beside it.";
        break;
    case Kind::TooNew:
        heading = "Saved by a newer U-Stu";
        body = name + " was saved by " +
               (error.savedBy.empty() ? std::string("a newer version of U-Stu Video Editor")
                                      : "U-Stu Video Editor " + error.savedBy) +
               ". Update this one (" USTUDIO_VERSION ") to open it. It was left as it is.";
        break;
    case Kind::TooOld:
        heading = "Project too old to open";
        body = name + " uses a project format from before this version's oldest. It was left as it is.";
        break;
    case Kind::NotAProject:
        heading = "Not a U-Stu project";
        body = name + " isn't a project this version of U-Stu Video Editor opens. It was left as it is.";
        break;
    case Kind::Unreadable:
        heading = "Can't read the project";
        body = name + " couldn't be read. It was left as it is.";
        break;
    case Kind::Invalid:
        heading = "The project is damaged";
        body = name + " was read, but its contents don't fit together. It was left as it is; a copy from an "
                      "earlier save may be in the .ustudio-backups folder beside it.";
        break;
    }
    showStatus(heading + ": " + path);
    AdwDialog *dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    GtkWidget *details = gtk_label_new(error.message.c_str());
    gtk_label_set_selectable(GTK_LABEL(details), TRUE);
    gtk_label_set_wrap(GTK_LABEL(details), TRUE);
    gtk_label_set_xalign(GTK_LABEL(details), 0.0f);
    gtk_widget_add_css_class(details, "dim-label");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), details);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
    adw_dialog_present(dialog, GTK_WIDGET(m_window));
}

void AppWindow::loadProjectAsync(const std::string &path, std::function<void(core::Model)> adopt,
                                 std::function<void(const core::ProjectLoadError &)> failed)
{
    if (!m_projectLoader)
        return; // shutting down
    core::UndoStack::State startState = m_undoStack.state();
    m_projectLoader->load(
        path, [this, startState, adopt = std::move(adopt), failed = std::move(failed)](ProjectLoader::Result loaded) {
            if (!loaded) {
                // Owner P0 (2026-10-08): a project that didn't open left
                // nothing in the log.
                Log::warn("[app] couldn't open a project: " + loaded.error().message);
                failed(loaded.error());
                return;
            }
            auto model = std::make_shared<core::Model>(std::move(*loaded));
            auto swap = [adopt, model] { adopt(std::move(*model)); };
            // The discard confirmation (if any) came before the parse; edits
            // made while it ran would otherwise vanish without a word.
            if (m_undoStack.state() != startState)
                confirmDiscardIfDirty(swap);
            else
                swap();
        });
}

int AppWindow::sequenceFrames() const
{
    return static_cast<int>(std::max<core::FrameIndex>(m_model.sequence().length(), 1));
}

double AppWindow::sequenceFps() const
{
    const core::Rational &fps = m_model.sequence().profile.fps;
    return fps.den > 0 ? static_cast<double>(fps.num) / fps.den : 0.0;
}

void AppWindow::replaceProject(core::Model model)
{
    cancelProjectJobs();
    m_model = std::move(model); // m_undoStack holds a reference to m_model, not a copy --
                                // reassigning its contents leaves it pointing at the right object
    // Before clear(): its `changed` publishes the same snapshot again, a
    // no-op, rather than rebuilding the new project with the old project's
    // cached masters and then again here.
    m_engine->reset(m_model.snapshot());
    m_undoStack.clear();
    m_activeTrack = 0;
    m_timelineController.selection().clear();
    refreshMissingBanner();
}

void AppWindow::onReloadProjectClicked()
{
    if (m_currentProjectPath.empty()) {
        showStatus("Nothing to reload -- this project hasn't been saved or opened yet.");
        return;
    }
    confirmDiscardIfDirty([this] { performReload(); });
}

void AppWindow::performReload()
{
    const std::string path = m_currentProjectPath;
    showStatus("Reloading " + path + "…");
    loadProjectAsync(
        path,
        [this, path](core::Model model) {
            replaceProject(std::move(model));
            m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
            // Audit A1 -- see loadProjectFromPath()'s own comment.
            m_pendingAutosaveCleanupPath.clear();
            m_pendingAutosaveCleanupMetaPath.clear();
            refreshTimeline();
            refreshMediaBrowser();
            showStatus("Reloaded: " + path);
        },
        [this, path](const core::ProjectLoadError &error) { showProjectLoadError(path, error); });
}

void AppWindow::onNewProjectClicked()
{
    confirmDiscardIfDirty([this] { performNewProject(); });
}

void AppWindow::performNewProject()
{
    cancelProjectJobs();
    m_model = core::Model::createEmpty();
    // Pristine starting state, matching the constructor's own initial
    // track -- not through the UndoStack, since there's nothing to undo
    // back out of on a project that's just been reset.
    m_model.addTrack(core::Track::Kind::Video, 0, "V1");
    m_currentProjectPath.clear();
    m_engine->reset(m_model.snapshot()); // first: see replaceProject()
    m_undoStack.clear();
    m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
    m_activeTrack = 0;
    m_timelineController.selection().clear();
    // Audit A1 -- see onOpenProjectFinished's own comment.
    m_pendingAutosaveCleanupPath.clear();
    m_pendingAutosaveCleanupMetaPath.clear();
    refreshTimeline();
    refreshMediaBrowser();
    showStatus("New project.");
}

core::RenderProfile AppWindow::defaultRenderProfile() const
{
    return m_renderProfiles->find(m_settings->defaultRenderProfile()).value_or(core::builtInRenderProfiles()[0]);
}

std::string AppWindow::missingMediaNotice() const
{
    const size_t missing = missingAssets(false).size();
    if (missing == 0)
        return {};
    return missing == 1 ? " 1 media file is missing." : " " + std::to_string(missing) + " media files are missing.";
}

std::string AppWindow::unplayedEffectsNotice() const
{
    // IP2 (doc 15, "Gating"): effects whose drop-in isn't loaded are kept and
    // saved unchanged, but don't play; say so, once, in one line.
    const dropins::DropInRegistry *dropIns = dropins::DropInRegistry::current();
    std::set<std::string> missing;
    auto note = [&](const std::vector<core::Effect> &effects) {
        for (const core::Effect &effect : effects)
            if (!dropIns || !dropIns->has(effect.owner))
                missing.insert(effect.owner.empty() ? "an unknown drop-in" : "“" + effect.owner + "”");
    };
    const core::Sequence &seq = m_model.sequence();
    for (const auto &[id, clip] : seq.clips)
        note(clip.effects);
    for (const core::Track &track : seq.tracks)
        note(track.effects);
    note(seq.effects);
    for (const core::AdjustmentBlock &block : seq.adjustmentBlocks)
        note(block.effects);
    if (missing.empty())
        return {};
    std::string owners;
    for (const std::string &owner : missing)
        owners += (owners.empty() ? "" : ", ") + owner;
    return ". It has effects from " + owners + ", not installed: it plays without them.";
}

void AppWindow::setInitialProjectFolder(GtkFileDialog *dialog) const
{
    const std::string folder = m_settings->defaultProjectFolder();
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec))
        return;
    GFile *file = g_file_new_for_path(folder.c_str());
    gtk_file_dialog_set_initial_folder(dialog, file);
    g_object_unref(file);
}

std::string AppWindow::exportFolder() const
{
    std::error_code ec;
    auto usable = [&ec](const std::string &folder) {
        return !folder.empty() && std::filesystem::is_directory(folder, ec);
    };
    if (std::string folder = m_settings->defaultExportFolder(); usable(folder))
        return folder;
    if (!m_currentProjectPath.empty()) {
        if (std::string folder = std::filesystem::path(m_currentProjectPath).parent_path().string(); usable(folder))
            return folder;
    }
    if (const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS); videos && usable(videos))
        return videos;
    return g_get_home_dir();
}

void AppWindow::onRenderClicked()
{
    if (m_renderQueue->busy()) {
        askCancelOrQueueRender();
        return;
    }
    if (!m_lastRenderedPath.empty()) {
        openLastRender();
        return;
    }
    const std::filesystem::path suggested = autoRenderPath(defaultRenderProfile());
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Render Project");
    gtk_file_dialog_set_initial_name(dialog, suggested.filename().c_str());
    GFile *folder = g_file_new_for_path(suggested.parent_path().c_str());
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_object_unref(folder);
    gtk_file_dialog_save(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::renderFinishedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onRenderFinished(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }
    std::string path = localPathFor(file, true);
    g_object_unref(file);
    if (!path.empty())
        queueRender(defaultRenderProfile(), path);
}

namespace {
// "High quality" -> "high-quality", for file names.
std::string fileNameSlug(const std::string &text)
{
    std::string slug;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    return slug.empty() ? "render" : slug;
}
} // namespace

std::string AppWindow::autoRenderPath(const core::RenderProfile &profile) const
{
    const std::string project =
        m_currentProjectPath.empty() ? "untitled" : std::filesystem::path(m_currentProjectPath).stem().string();
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    const std::filesystem::path folder = exportFolder();
    const std::string base = project + "-" + fileNameSlug(profile.name) + "-" + stamp;
    // Two renders queued within a second: the second gets "-2", and so on.
    auto taken = [this](const std::string &path) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return true;
        const RenderJob *running = m_renderQueue ? m_renderQueue->running() : nullptr;
        return (running && running->outputPath == path) || (m_renderQueue && m_renderQueue->isQueued(path));
    };
    std::string path = (folder / (base + ".mp4")).string();
    for (int n = 2; taken(path); ++n)
        path = (folder / (base + "-" + std::to_string(n) + ".mp4")).string();
    return path;
}

void AppWindow::queueRender(const core::RenderProfile &profile, const std::string &path, bool missingConfirmed)
{
    // Never export red placeholders without asking (M4 B).
    if (!missingConfirmed) {
        if (const size_t missing = missingAssets(true).size(); missing > 0) {
            struct Pending
            {
                AppWindow *self;
                core::RenderProfile profile;
                std::string path;
            };
            const std::string heading =
                missing == 1 ? "1 file is missing" : std::to_string(missing) + " files are missing";
            AdwDialog *dialog =
                adw_alert_dialog_new(heading.c_str(), "The render will show red frames where they're used.");
            adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "relink", "Relink First", "render",
                                           "Render Anyway", nullptr);
            adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "relink");
            adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "relink");
            adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "render", ADW_RESPONSE_DESTRUCTIVE);
            adw_alert_dialog_choose(
                ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
                [](GObject *source, GAsyncResult *result, gpointer data) {
                    std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
                    const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
                    if (response == "render")
                        pending->self->queueRender(pending->profile, pending->path, true);
                    else
                        pending->self->showRelinkDialog();
                },
                new Pending{this, profile, path});
            return;
        }
    }
    if (pathIsProjectAsset(path)) {
        showStatus("Refusing to render over a file already in this project's media: " + path);
        return;
    }
    const bool startsNow = !m_renderQueue->busy();
    RenderJob job;
    job.snapshot = m_model.snapshot(); // the project as it is now, whatever happens to it later
    job.profile = profile;
    job.outputPath = path;
    m_renderQueue->enqueue(std::move(job));
    if (!startsNow)
        showStatus("Queued “" + profile.name + "” to " + path + " (" + std::to_string(m_renderQueue->queued()) +
                   " waiting).");
    updateRenderButton();
}

void AppWindow::askCancelOrQueueRender()
{
    const RenderJob *running = m_renderQueue->running();
    const std::string body = "“" + std::filesystem::path(running ? running->outputPath : "").filename().string() +
                             "” is rendering. You can stop it, or queue the project as it is now with the " +
                             "default profile.";
    AdwDialog *dialog = adw_alert_dialog_new("Cancel render or queue another?", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "neither", "Neither");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel Render");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "queue", "Queue Another");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "cancel", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "queue", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "neither");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "neither");
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            auto *self = static_cast<AppWindow *>(userData);
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response == "cancel") {
                self->m_renderQueue->cancelCurrent();
                self->showStatus("Cancelling the render…");
            } else if (response == "queue") {
                const core::RenderProfile profile = self->defaultRenderProfile();
                self->queueRender(profile, self->autoRenderPath(profile));
            }
        },
        this);
}

void AppWindow::openLastRender()
{
    const std::string path = std::exchange(m_lastRenderedPath, {});
    updateRenderButton();
    gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    GError *error = nullptr;
    if (!uri || !g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
        showStatus("Couldn't open " + path + (error ? std::string(": ") + error->message : std::string()));
        if (error)
            g_error_free(error);
    }
    g_free(uri);
}

void AppWindow::updateRenderButton()
{
    if (!m_renderButton)
        return;
    const bool busy = m_renderQueue && m_renderQueue->busy();
    const size_t queued = m_renderQueue ? m_renderQueue->queued() : 0;
    const bool done = !busy && !m_lastRenderedPath.empty();
    gtk_widget_set_visible(GTK_WIDGET(m_renderProgress), busy);
    if (busy)
        gtk_widget_add_css_class(m_renderButton, "rendering");
    else
        gtk_widget_remove_css_class(m_renderButton, "rendering");
    if (done)
        gtk_widget_add_css_class(m_renderButton, "render-done");
    else
        gtk_widget_remove_css_class(m_renderButton, "render-done");
    if (!busy)
        gtk_label_set_text(m_renderLabel, done ? "Open Render" : "Render…");
    gtk_widget_set_visible(GTK_WIDGET(m_renderBadge), queued > 0);
    gtk_label_set_text(m_renderBadge, std::to_string(queued).c_str());

    std::string tooltip =
        done ? "Open " + std::filesystem::path(m_lastRenderedPath).filename().string() : tooltipText("header.render");
    if (queued > 0)
        tooltip = std::to_string(queued) + " queued\n" + tooltip;
    gtk_widget_set_tooltip_text(m_renderButton, tooltip.c_str());
}

void AppWindow::showRenderMenu()
{
    // Rebuilt each time: profiles change in Settings.
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto addItem = [this, box](const std::string &label, const std::string &profileName) {
        GtkWidget *item = gtk_button_new_with_label(label.c_str());
        gtk_widget_add_css_class(item, "flat");
        gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(item)), GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(item), "ustudio-profile", g_strdup(profileName.c_str()), g_free);
        g_signal_connect(item, "clicked", G_CALLBACK(&AppWindow::renderMenuItemClickedTrampoline), this);
        gtk_box_append(GTK_BOX(box), item);
    };
    const bool busy = m_renderQueue->busy();
    for (const core::RenderProfile &profile : m_renderProfiles->all())
        addItem((busy ? "Queue “" : "Render “") + profile.name + "”", profile.name);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    addItem("Add to Render Queue", "");
    gtk_popover_set_child(m_renderMenu, box);
    gtk_popover_popup(m_renderMenu);
}

void AppWindow::onRenderStarted(const RenderJob &job)
{
    m_lastRenderedPath.clear();
    gtk_progress_bar_set_fraction(m_renderProgress, 0.0);
    gtk_label_set_text(m_renderLabel, "Render 0%");
    updateRenderButton();
    showStatus("Rendering “" + job.profile.name + "” to " + job.outputPath +
               " … (this can take a while — the window will stay responsive)");
}

void AppWindow::onRenderProgress(double fraction)
{
    fraction = std::clamp(fraction, 0.0, 1.0);
    const std::string percent = std::to_string(static_cast<int>(fraction * 100.0)) + "%";
    gtk_progress_bar_set_fraction(m_renderProgress, fraction);
    gtk_label_set_text(m_renderLabel, ("Render " + percent).c_str());
    showStatus("Rendering… " + percent);
}

void AppWindow::onRenderDone(const RenderJob &job, bool ok, bool cancelled, const std::string &error)
{
    if (ok) {
        m_lastRenderedPath = job.outputPath;
        showStatus("Rendered: " + job.outputPath);
    } else if (cancelled) {
        showStatus("Render cancelled; nothing was written.");
    } else {
        showStatus("Render failed: " + error);
    }
    updateRenderButton();
}

void AppWindow::offerPendingRenders()
{
    const std::vector<pending_renders::Pending> pending = pending_renders::list(pending_renders::directory());
    if (pending.empty())
        return;
    const std::string body = std::to_string(pending.size()) +
                             (pending.size() == 1 ? " render didn't" : " renders didn't") +
                             " finish last time. Restart them? They start from the beginning.";
    AdwDialog *dialog = adw_alert_dialog_new("Restart unfinished renders?", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "restart", "Restart");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "restart", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "restart");
    // Closed unanswered: offered again next launch.
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "later");
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            auto *self = static_cast<AppWindow *>(userData);
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response == "restart")
                self->restartPendingRenders();
            else if (response == "discard")
                pending_renders::clear(pending_renders::directory());
        },
        this);
}

void AppWindow::restartPendingRenders()
{
    // The snapshots are project files: read on the pool, queued here.
    m_pool->submit(
        [token = std::weak_ptr<void>(m_lifetime), this](std::stop_token) {
            const std::string dir = pending_renders::directory();
            auto jobs = std::make_shared<std::vector<RenderJob>>();
            size_t unreadable = 0;
            for (const pending_renders::Pending &pending : pending_renders::list(dir)) {
                std::expected<core::Model, std::string> model = core::loadProject(pending.projectPath);
                if (!model) {
                    Log::warn("[render] Can't restart " + pending.outputPath + ": " + model.error());
                    ++unreadable;
                    continue;
                }
                RenderJob job;
                job.snapshot = model->snapshot();
                job.profile = pending.profile;
                job.outputPath = pending.outputPath;
                jobs->push_back(std::move(job));
            }
            engine::MainThreadDispatcher::post(token, [this, jobs, unreadable, dir] {
                // Queued in memory now; quitting again saves them afresh.
                pending_renders::clear(dir);
                for (RenderJob &job : *jobs)
                    m_renderQueue->enqueue(std::move(job));
                if (unreadable > 0)
                    showStatus(std::to_string(unreadable) + " unfinished render(s) couldn't be read and were dropped.");
                updateRenderButton();
            });
        },
        core::concurrency::Priority::Interactive);
}

void AppWindow::onAddTrackClicked()
{
    // Inserted at row 0 (the top of the visual stack), matching v1's "a
    // new track always appears on top" -- see mltTrackOrder (doc 03):
    // video tracks are bottom-to-top by REVERSED model order, so a track
    // at model row 0 gets the highest MLT index, i.e. compositing wins.
    size_t trackNumber = m_model.sequence().tracks.size() + 1;
    auto cmd = std::make_unique<core::AddTrack>(core::Track::Kind::Video, 0, "V" + std::to_string(trackNumber));
    if (m_undoStack.execute(std::move(cmd))) {
        m_activeTrack = 0;
        refreshTimeline();
        showStatus("Added a track (now active).");
    }
}

void AppWindow::onUndo()
{
    if (m_undoStack.undo()) {
        // The selection stays: m_undoStack.changed pruned what's gone (with
        // the preview's handles, losing it on every undo was jarring).
        refreshTimeline();
        showStatus("Undid: " + m_undoStack.redoLabel());
    }
}

void AppWindow::onRedo()
{
    if (m_undoStack.redo()) {
        // The selection stays: m_undoStack.changed pruned what's gone (with
        // the preview's handles, losing it on every undo was jarring).
        refreshTimeline();
        showStatus("Redid: " + m_undoStack.undoLabel());
    }
}

void AppWindow::onPlayToggled()
{
    m_engine->togglePlay();
    refreshPlayButtonIcon();
}

void AppWindow::onSeekChanged()
{
    if (m_suppressSeekSignal)
        return;
    int frame = static_cast<int>(gtk_range_get_value(GTK_RANGE(m_seekScale)));
    m_engine->seek(frame);
}

void AppWindow::onShuttleForward()
{
    double current = m_engine->speed();
    double next = (current <= 0.0) ? 1.0 : std::min(current * 2.0, m_settings->shuttleMaxSpeed());
    m_engine->play(next);
    refreshPlayButtonIcon();
}

void AppWindow::onShuttleReverse()
{
    double current = m_engine->speed();
    double next = (current >= 0.0) ? -1.0 : std::max(current * 2.0, -m_settings->shuttleMaxSpeed());
    m_engine->play(next);
    refreshPlayButtonIcon();
}

void AppWindow::onShuttleStop()
{
    m_engine->pause();
    refreshPlayButtonIcon();
}

void AppWindow::onStepForward()
{
    m_engine->stepFrame(1);
    refreshPlayButtonIcon();
}

void AppWindow::onStepBackward()
{
    m_engine->stepFrame(-1);
    refreshPlayButtonIcon();
}

void AppWindow::onStepForward10()
{
    m_engine->stepFrame(10);
    refreshPlayButtonIcon();
}

void AppWindow::onStepBackward10()
{
    m_engine->stepFrame(-10);
    refreshPlayButtonIcon();
}

// fps() is a profile property (always > 0 for a loaded project -- doc 09's
// loadProject() refuses a zero/negative frame rate outright), but this
// still falls back the same way the timecode label does (line ~2418) for
// the brief window before any project/tractor exists.
int AppWindow::oneMinuteInFrames() const
{
    double fps = sequenceFps();
    return fps > 0.0 ? static_cast<int>(fps * 60.0 + 0.5) : 25 * 60;
}

void AppWindow::onStepForwardMinute()
{
    m_engine->stepFrame(oneMinuteInFrames());
    refreshPlayButtonIcon();
}

void AppWindow::onStepBackwardMinute()
{
    m_engine->stepFrame(-oneMinuteInFrames());
    refreshPlayButtonIcon();
}

void AppWindow::onSeekHome()
{
    m_engine->toHome();
}

void AppWindow::onSeekEnd()
{
    m_engine->toEnd();
}

// Every clip boundary (start and end) on the active track, plus the
// timeline's own start (0) and end, sorted and deduplicated -- shared by
// onSeekPreviousCut/onSeekNextCut so their "which frames count as a cut"
// definition can't drift apart.
std::vector<int> AppWindow::cutBoundariesOnActiveTrack() const
{
    return cutBoundariesForTrack(m_activeTrack);
}

std::vector<int> AppWindow::cutBoundariesForTrack(int row) const
{
    const core::Track &track = m_model.track(trackIdForRow(row));
    // The LAST actually reachable frame, not the exclusive totalFrames()
    // itself: PlaybackController::seek() clamps to [0, totalFrames()-1],
    // so using the raw total here would make "next cut" silently re-seek
    // to the same already-clamped frame forever once at the end, instead
    // of ever reporting "no later cut".
    int lastFrame = std::max(sequenceFrames() - 1, 0);
    std::vector<int> boundaries{0, lastFrame};
    for (core::ClipId clipId : track.clips) {
        const core::Clip &clip = m_model.clip(clipId);
        boundaries.push_back(static_cast<int>(clip.position));
        boundaries.push_back(static_cast<int>(clip.end()));
    }
    // A clip ending exactly at the sequence's own length (the common
    // case for whichever clip plays last) pushes the raw, EXCLUSIVE
    // clip.end() -- equal to totalFrames(), one past lastFrame -- so
    // clamp every entry before deduping, or that unreachable value
    // would slip back in as a distinct boundary past lastFrame,
    // reintroducing the exact bug lastFrame above exists to avoid.
    for (int &boundary : boundaries)
        boundary = std::clamp(boundary, 0, lastFrame);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

std::vector<int> AppWindow::cutBoundariesAllTracks() const
{
    std::vector<int> boundaries;
    for (int row = 0; row < static_cast<int>(m_model.sequence().tracks.size()); ++row) {
        std::vector<int> track = cutBoundariesForTrack(row);
        boundaries.insert(boundaries.end(), track.begin(), track.end());
    }
    // Plain A/F stop at markers too (owner, 2026-09-25); Shift+A/F, the
    // active-track seek, stays on that track's cuts.
    const int lastFrame = std::max(sequenceFrames() - 1, 0);
    for (const core::Marker &marker : m_model.sequence().markers)
        boundaries.push_back(std::clamp(static_cast<int>(marker.at), 0, lastFrame));
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

void AppWindow::moveSelectedClipAcrossTracks(int direction)
{
    core::ClipId clipId = m_timelineController.selection().single();
    if (!clipId.isValid() || !m_model.hasClip(clipId))
        return;
    for (const core::Transition &t : m_model.sequence().transitions) {
        if (t.a == clipId || t.b == clipId) {
            showStatus("This clip is in a dissolve; remove the dissolve to move it to another track.");
            return;
        }
    }
    const std::vector<core::Track> &tracks = m_model.sequence().tracks;
    const core::Clip &clip = m_model.clip(clipId);
    const core::FrameIndex position = clip.position;
    int row = 0;
    while (row < static_cast<int>(tracks.size()) && tracks[static_cast<size_t>(row)].id != clip.track)
        ++row;
    const core::Track::Kind kind = tracks[static_cast<size_t>(row)].kind;
    if (tracks[static_cast<size_t>(row)].locked) {
        showStatus("This clip's track is locked.");
        return;
    }
    for (int r = row + direction; r >= 0 && r < static_cast<int>(tracks.size()); r += direction) {
        const core::Track &target = tracks[static_cast<size_t>(r)];
        if (target.kind != kind)
            continue; // a video clip stays on video tracks, audio on audio
        // MoveClip checks everything else (a clip in the way, a locked
        // track) and leaves the model untouched when it refuses: the first
        // track it accepts is the nearest free one.
        if (m_undoStack.execute(std::make_unique<core::MoveClip>(clipId, target.id, position))) {
            m_activeTrack = r;
            refreshTimeline();
            showStatus("Moved the clip to " + tracks[static_cast<size_t>(r)].name + ".");
            return;
        }
    }
    showStatus(direction < 0 ? "No free track above for this clip." : "No free track below for this clip.");
}

void AppWindow::onSeekPreviousCut(bool activeTrackOnly)
{
    if (m_model.sequence().tracks.empty())
        return;
    std::vector<int> boundaries = activeTrackOnly ? cutBoundariesOnActiveTrack() : cutBoundariesAllTracks();

    // The largest boundary strictly before the current frame: lower_bound
    // finds the first boundary >= current (which, if current sits exactly
    // on one, is that same boundary, not the one before it), so the
    // previous distinct cut is always one step back from there.
    auto it = std::lower_bound(boundaries.begin(), boundaries.end(), m_engine->currentFrame());
    if (it == boundaries.begin()) {
        showStatus(activeTrackOnly ? "No earlier cut on this track." : "No earlier cut or marker.");
        return;
    }
    m_engine->seek(*(it - 1));
}

void AppWindow::onSeekNextCut(bool activeTrackOnly)
{
    if (m_model.sequence().tracks.empty())
        return;
    std::vector<int> boundaries = activeTrackOnly ? cutBoundariesOnActiveTrack() : cutBoundariesAllTracks();

    auto it = std::upper_bound(boundaries.begin(), boundaries.end(), m_engine->currentFrame());
    if (it == boundaries.end()) {
        showStatus(activeTrackOnly ? "No later cut on this track." : "No later cut or marker.");
        return;
    }
    m_engine->seek(*it);
}

void AppWindow::onActiveTrackUp()
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;
    m_activeTrack = std::clamp(m_activeTrack - 1, 0, trackCount - 1);
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onActiveTrackDown()
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;
    m_activeTrack = std::clamp(m_activeTrack + 1, 0, trackCount - 1);
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onSetLoopIn()
{
    int frame = m_engine->currentFrame();
    auto range = m_engine->loopRange();
    int out = range ? range->second : std::max(sequenceFrames() - 1, 0);
    if (frame >= out) {
        showStatus("Loop in must be before loop out.");
        return;
    }
    m_engine->setLoopRange(std::make_pair(frame, out));
    refreshLoopStatusLabel();
}

void AppWindow::onSetLoopOut()
{
    int frame = m_engine->currentFrame();
    auto range = m_engine->loopRange();
    int in = range ? range->first : 0;
    if (frame <= in) {
        showStatus("Loop out must be after loop in.");
        return;
    }
    m_engine->setLoopRange(std::make_pair(in, frame));
    refreshLoopStatusLabel();
}

void AppWindow::onClearLoopClicked()
{
    m_engine->setLoopRange(std::nullopt);
    refreshLoopStatusLabel();
}

void AppWindow::onVolumeChanged()
{
    m_engine->setVolume(gtk_range_get_value(GTK_RANGE(m_volumeScale)));
}

void AppWindow::updatePreviewScaleLabel()
{
    if (!m_previewScaleDropdown)
        return;
    // Only the selected Auto says what it resolved to: the engine's factor
    // is the manual one otherwise.
    const guint selected = gtk_drop_down_get_selected(m_previewScaleDropdown);
    const double factor = m_engine->previewFactor();
    const char *label = "Auto";
    if (selected == 0)
        label = factor >= 1.0 ? "Auto (Full)" : factor >= 0.5 ? "Auto (Half)" : "Auto (Quarter)";
    GtkStringList *list = GTK_STRING_LIST(gtk_drop_down_get_model(m_previewScaleDropdown));
    if (std::strcmp(gtk_string_list_get_string(list, 0), label) == 0)
        return;
    // Replacing the selected item deselects it: put the selection back
    // without it reaching the engine as a change.
    m_relabellingPreviewScale = true;
    const char *items[] = {label, nullptr};
    gtk_string_list_splice(list, 0, 1, items);
    gtk_drop_down_set_selected(m_previewScaleDropdown, selected);
    m_relabellingPreviewScale = false;
}

void AppWindow::onPreviewScaleChanged()
{
    if (m_relabellingPreviewScale)
        return;
    // EngineSync owns the preview scale: it builds the playback tractor on
    // a scaled profile (doc 05), rebuilding -- and so restarting playback
    // via `rebuilt` -- only when the resolved factor actually changes.
    switch (gtk_drop_down_get_selected(m_previewScaleDropdown)) {
    case 1:
        m_engine->setPreviewScale(engine::PreviewScale::Full);
        break;
    case 2:
        m_engine->setPreviewScale(engine::PreviewScale::Half);
        break;
    case 3:
        m_engine->setPreviewScale(engine::PreviewScale::Quarter);
        break;
    default:
        m_engine->setPreviewScale(engine::PreviewScale::Auto);
        break;
    }
    // Right away when the factor doesn't change (no rebuild follows);
    // `rebuilt` relabels it when it does.
    updatePreviewScaleLabel();
}

void AppWindow::onSplitClicked()
{
    int frame = m_engine->currentFrame();
    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_activeTrack && frame > clip.startFrame && frame < clip.startFrame + clip.frames) {
            if (m_undoStack.execute(std::make_unique<core::SplitClip>(clip.id, frame))) {
                refreshTimeline();
            } else {
                showStatus("Couldn't split there.");
            }
            return;
        }
    }
    showStatus("Nothing to split on track " + std::to_string(m_activeTrack) + " at the current playhead.");
}

void AppWindow::onTimelineClicked(int nPress, double x, double y, timeline::Modifiers mods)
{
    if (nPress < 2)
        return;
    if (overlayClaimsPress(x, y, nPress))
        return;
    timeline::TimelineOutcome outcome = m_timelineController.click(timelineContext(), nPress, x, y, mods);
    applyTimelineOutcome(outcome);
}

timeline::RowLayout AppWindow::rowLayout() const
{
    timeline::RowLayout layout{kTrackRowHeight, kTrackLabelHeight, {}};
    // IP5: a drop-in's lanes under the tracks and above the first; none
    // without one.
    if (!m_timelineOverlays.empty()) {
        for (const timeline::TimelineOverlayProvider *overlay : m_timelineOverlays)
            layout.topLane = std::max(layout.topLane, overlay->topLaneHeight(m_model));
        for (const core::Track &track : m_model.sequence().tracks) {
            double lane = 0.0;
            for (const timeline::TimelineOverlayProvider *overlay : m_timelineOverlays)
                lane += std::max(overlay->laneHeight(m_model, track), 0.0);
            layout.lanes.push_back(lane);
        }
    }
    return layout;
}

timeline::TimelineContext AppWindow::timelineContext() const
{
    return timeline::TimelineContext{.model = m_model,
                                     .viewport = m_viewport,
                                     .layout = rowLayout(),
                                     .handleWidth = kHandleWidth,
                                     .edgeGrabPx = kEdgeGrabWidth,
                                     .dragThresholdPx = kDragClickThreshold,
                                     .playhead = m_engine->currentFrame(),
                                     .sequenceLength = sequenceFrames(),
                                     .rippleMode = rippleMode(),
                                     .snapping = m_snapWhileDragging};
}

void AppWindow::applyTimelineOutcome(timeline::TimelineOutcome &outcome)
{
    if (outcome.activeRow)
        m_activeTrack = *outcome.activeRow;

    bool succeeded = false;
    for (timeline::TimelineOutcome::Attempt &attempt : outcome.attempts) {
        if (m_undoStack.execute(std::move(attempt.command))) {
            succeeded = true;
            if (!attempt.successStatus.empty())
                showStatus(attempt.successStatus);
            break;
        }
    }
    if (!outcome.attempts.empty() && !succeeded && !outcome.failureStatus.empty())
        showStatus(outcome.failureStatus);
    if (succeeded && outcome.activeRowOnSuccess)
        m_activeTrack = *outcome.activeRowOnSuccess;

    if (outcome.seek)
        m_engine->seek(static_cast<int>(*outcome.seek));

    if (outcome.rename == timeline::TimelineOutcome::Rename::Track) {
        beginTrackNameEdit(outcome.renameRow);
    } else if (outcome.rename == timeline::TimelineOutcome::Rename::Clip) {
        for (const ClipDisplay &clip : m_clips) {
            if (clip.id == outcome.renameClip) {
                beginClipNameEdit(clip);
                break;
            }
        }
    }
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onTimelineRightClicked(double x, double y)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;

    timeline::ContextTarget target = m_timelineController.contextTargetAt(timelineContext(), x, y);
    if (target.row < 0)
        return; // a drop-in's top lane: not a track
    int row = target.row;
    m_contextMenuTrack = row;
    m_contextMenuFrame = static_cast<int>(target.frame);
    m_contextMenuClipStartFrame = target.clip.isValid() ? static_cast<int>(m_model.clip(target.clip).position) : -1;
    m_contextMenuGapStartFrame = target.gapStart ? static_cast<int>(*target.gapStart) : -1;
    m_contextMenuTransitionId = target.transition;
    m_contextMenuAddTransitionA = target.addTransitionA;
    m_contextMenuAddTransitionB = target.addTransitionB;
    m_contextMenuClip = target.clip;

    gtk_widget_set_visible(m_removeTransitionButton, m_contextMenuTransitionId.isValid());
    const std::set<core::ClipId> &selectedClips = m_timelineController.selection().clips();
    gtk_widget_set_visible(m_syncClipsButton,
                           target.clip.isValid() && selectedClips.size() == 2 && selectedClips.contains(target.clip));
    gtk_widget_set_visible(m_addTransitionButton, m_contextMenuAddTransitionA.isValid());

    gtk_widget_set_visible(m_deleteClipButton, m_contextMenuClipStartFrame >= 0);

    // Doc 06: only offered when the clip actually has audio to pull out
    // and isn't already audio-only (splitting an audio-only clip would be
    // a no-op InsertClip of silence onto a second audio track).
    bool showSplitAudio = false;
    if (m_contextMenuClipStartFrame >= 0) {
        for (const auto &clip : m_clips) {
            if (clip.trackIndex == row && clip.startFrame == m_contextMenuClipStartFrame) {
                const core::Clip &modelClip = m_model.clip(clip.id);
                showSplitAudio = modelClip.videoEnabled && modelClip.audioEnabled &&
                                 m_model.hasAsset(modelClip.asset) && m_model.asset(modelClip.asset).info.hasAudio;
                break;
            }
        }
    }
    gtk_widget_set_visible(m_splitAudioButton, showSplitAudio);

    gtk_widget_set_visible(m_closeGapButton, m_contextMenuGapStartFrame >= 0);

    // Edit Name / Remove Name: only offered on a clip. Label and the
    // Remove button's visibility both depend on whether it already has a
    // custom name (doc request: "if it has a name it should have
    // edit/remove").
    bool clipHasName = false;
    if (m_contextMenuClipStartFrame >= 0) {
        for (const auto &clip : m_clips) {
            if (clip.trackIndex == row && clip.startFrame == m_contextMenuClipStartFrame) {
                clipHasName = !clip.name.empty();
                break;
            }
        }
    }
    gtk_widget_set_visible(m_editClipNameButton, m_contextMenuClipStartFrame >= 0);
    gtk_button_set_label(GTK_BUTTON(m_editClipNameButton), clipHasName ? "Edit Clip Name" : "Add Clip Name");
    gtk_widget_set_visible(m_removeClipNameButton, clipHasName);

    bool onEmptyTrackSpace = m_contextMenuClipStartFrame < 0 && m_contextMenuGapStartFrame < 0;
    gtk_widget_set_visible(m_removeTrackButton, onEmptyTrackSpace);
    gtk_widget_set_visible(m_toggleLockButton, onEmptyTrackSpace);
    gtk_widget_set_visible(m_toggleMuteButton, onEmptyTrackSpace);
    gtk_widget_set_visible(m_toggleHideButton, false);
    gtk_widget_set_visible(m_editTrackNameButton, onEmptyTrackSpace);
    gtk_widget_set_visible(gtk_widget_get_parent(GTK_WIDGET(m_trackVolumeScale)), onEmptyTrackSpace);
    if (onEmptyTrackSpace) {
        const core::Track &track = m_model.track(trackIdForRow(row));
        gtk_button_set_label(GTK_BUTTON(m_toggleLockButton), track.locked ? "Unlock Track" : "Lock Track");
        gtk_button_set_label(GTK_BUTTON(m_toggleMuteButton), track.muted ? "Unmute Track" : "Mute Track");
        // Hide turns a track's picture off; an audio track has none.
        gtk_widget_set_visible(m_toggleHideButton, track.kind == core::Track::Kind::Video);
        gtk_button_set_label(GTK_BUTTON(m_toggleHideButton), track.hidden ? "Show Track" : "Hide Track");
        m_suppressTrackVolumeSignal = true;
        gtk_range_set_value(GTK_RANGE(m_trackVolumeScale), track.volume);
        m_suppressTrackVolumeSignal = false;
    }

    GdkRectangle rect{static_cast<int>(x), static_cast<int>(rowLayout().rowTop(row)), 1,
                      static_cast<int>(rowLayout().spanOf(row))};
    gtk_popover_set_pointing_to(m_trackContextMenu, &rect);
    gtk_popover_popup(m_trackContextMenu);
}

void AppWindow::onDeleteClipClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    core::ClipId clipId;
    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            clipId = clip.id;
            break;
        }
    }

    if (clipId.isValid() && m_undoStack.execute(std::make_unique<core::RemoveClip>(clipId))) {
        m_timelineController.selection().clear();
        refreshTimeline();
        showStatus("Deleted clip — gap left behind. Right-click the gap to close it.");
    } else {
        showStatus("Couldn't delete that clip.");
    }
}

void AppWindow::onDeleteSelectedClip()
{
    const std::set<core::ClipId> &selected = m_timelineController.selection().clips();
    if (selected.empty()) {
        showStatus("No clip selected to delete.");
        return;
    }

    // Every selected clip, as one undo step.
    std::vector<std::unique_ptr<core::Command>> removals;
    for (core::ClipId clipId : selected)
        removals.push_back(std::make_unique<core::RemoveClip>(clipId));
    size_t count = removals.size();
    if (m_undoStack.execute(std::make_unique<core::CompositeCommand>(count == 1 ? "Delete clip" : "Delete clips",
                                                                     std::move(removals)))) {
        m_timelineController.selection().clear();
        refreshTimeline();
        showStatus(count == 1 ? "Deleted clip — gap left behind. Right-click the gap to close it."
                              : "Deleted " + std::to_string(count) + " clips — gaps left behind.");
    } else {
        showStatus("Couldn't delete that clip.");
    }
}

void AppWindow::onSplitAudioClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    core::ClipId clipId;
    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            clipId = clip.id;
            break;
        }
    }

    if (clipId.isValid() && m_undoStack.execute(std::make_unique<core::SplitAudio>(clipId))) {
        refreshTimeline();
        showStatus("Split audio to its own track.");
    } else {
        showStatus("Couldn't split that clip's audio.");
    }
}

void AppWindow::onCloseGapClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuGapStartFrame < 0) {
        m_contextMenuTrack = -1;
        return;
    }

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    const core::Track &track = m_model.track(trackId);
    core::FrameIndex gapStart = m_contextMenuGapStartFrame;

    // The gap's length: distance from gapStart to the next clip's start
    // (there must be one, or onTimelineRightClicked wouldn't have offered
    // "Close Gap" for this position at all).
    core::FrameIndex gapEnd = gapStart;
    bool foundNext = false;
    for (core::ClipId clipId : track.clips) {
        core::FrameIndex position = m_model.clip(clipId).position;
        if (position > gapStart) {
            gapEnd = position;
            foundNext = true;
            break;
        }
    }

    if (!foundNext) {
        showStatus("Couldn't close that gap.");
        m_contextMenuTrack = -1;
        return;
    }

    // ShiftClips moves everything after the gap as one group, so dissolves
    // between those clips survive (chaining MoveClip stripped every one).
    std::vector<std::unique_ptr<core::Command>> shift;
    shift.push_back(std::make_unique<core::ShiftClips>(trackId, gapEnd, -(gapEnd - gapStart)));
    bool ok = m_undoStack.execute(std::make_unique<core::CompositeCommand>("Close gap", std::move(shift)));
    if (ok) {
        m_timelineController.selection().clear();
        refreshTimeline();
        showStatus("Closed gap.");
    } else {
        showStatus("Couldn't close that gap.");
    }
    m_contextMenuTrack = -1;
}

void AppWindow::onEditClipNameClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            beginClipNameEdit(clip);
            return;
        }
    }
}

void AppWindow::onRemoveClipNameClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            if (m_undoStack.execute(std::make_unique<core::RenameClip>(clip.id, std::string{}))) {
                refreshTimeline();
                showStatus("Removed clip name.");
            } else {
                showStatus("Couldn't remove that clip's name.");
            }
            return;
        }
    }
}

void AppWindow::onSyncClipsClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    const std::set<core::ClipId> &selected = m_timelineController.selection().clips();
    const core::ClipId anchor = m_contextMenuClip;
    if (selected.size() != 2 || !selected.contains(anchor) || !m_model.hasClip(anchor))
        return;
    const core::ClipId other = *selected.begin() == anchor ? *std::next(selected.begin()) : *selected.begin();
    const core::Clip &a = m_model.clip(anchor);
    const core::Clip &b = m_model.clip(other);
    auto hasSound = [&](const core::Clip &clip) {
        return clip.audioEnabled && m_model.hasAsset(clip.asset) && m_model.asset(clip.asset).info.hasAudio;
    };
    if (!hasSound(a) || !hasSound(b)) {
        showStatus("Both clips need sound to sync by it.");
        return;
    }
    for (const core::Transition &t : m_model.sequence().transitions) {
        if (t.a == other || t.b == other) {
            showStatus("The clip to move is in a dissolve; remove the dissolve to sync it.");
            return;
        }
    }
    if (m_model.track(b.track).locked) {
        showStatus("The clip to move is on a locked track.");
        return;
    }

    // Only what can line up within the search range is decoded: the stretch
    // of timeline where the clips overlap, widened by the range, capped.
    const double fps = sequenceFps();
    const core::FrameIndex reach = static_cast<core::FrameIndex>(std::ceil(kSyncSearchSeconds * fps));
    const core::FrameIndex windowStart = std::max(a.position, b.position) - reach;
    const core::FrameIndex windowEnd =
        std::min({a.end(), b.end(), windowStart + reach + static_cast<core::FrameIndex>(120.0 * fps)}) + reach;
    auto spanFor = [&](const core::Clip &clip) {
        const core::FrameIndex start = std::max(clip.position, windowStart);
        const core::FrameIndex end = std::min(clip.end(), windowEnd);
        return engine::AudioSpan{m_model.asset(clip.asset).path, clip.in + (start - clip.position),
                                 std::max<core::FrameIndex>(0, end - start), start};
    };
    const engine::AudioSpan spanA = spanFor(a);
    const engine::AudioSpan spanB = spanFor(b);
    if (spanA.frames <= 0 || spanB.frames <= 0) {
        showStatus("The clips are too far apart to sync (they must overlap, give or take " +
                   std::to_string(static_cast<int>(kSyncSearchSeconds)) + " seconds).");
        return;
    }

    showStatus("Syncing by audio…");
    const core::Rational rate = m_model.sequence().profile.fps;
    const core::UndoStack::State stateBefore = m_undoStack.state();
    const uint64_t generation = m_projectGeneration;
    m_pool->submit(
        [this, spanA, spanB, rate, anchor, other, stateBefore, generation,
         token = std::weak_ptr<void>(m_lifetime)](std::stop_token stop) {
            core::trace::Scope trace("sync: decode and match");
            std::optional<core::audio::Alignment> alignment;
            std::string error;
            auto envA = engine::decodeEnvelope(spanA, rate);
            auto envB = stop.stop_requested() ? std::nullopt : engine::decodeEnvelope(spanB, rate);
            if (!envA || !envB)
                error = "Couldn't read the sound of one of the clips (silent, or the file can't be opened).";
            else if (!(alignment = core::audio::align(*envA, *envB, kSyncSearchSeconds * 1000.0, 2'000.0)))
                error = "The clips don't overlap for long enough to compare their sound (2 seconds or more).";
            engine::MainThreadDispatcher::post(token, [this, anchor, other, stateBefore, generation, alignment, error] {
                applySyncResult(anchor, other, stateBefore, generation, alignment, error);
            });
        },
        core::concurrency::Priority::Interactive);
}

void AppWindow::applySyncResult(core::ClipId anchor, core::ClipId other, core::UndoStack::State stateBefore,
                                uint64_t generation, std::optional<core::audio::Alignment> alignment,
                                const std::string &error)
{
    // Measured against the timeline as it was: any edit since makes the
    // offset meaningless.
    if (generation != m_projectGeneration || m_undoStack.state() != stateBefore || !m_model.hasClip(anchor) ||
        !m_model.hasClip(other)) {
        showStatus("The timeline changed while syncing; sync again.");
        return;
    }
    if (!error.empty()) {
        showStatus(error);
        return;
    }
    if (!alignment->confident) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Couldn't find a confident audio match (best %.2f, next best %.2f); nothing moved.",
                      alignment->correlation, alignment->runnerUp);
        showStatus(buf);
        return;
    }
    const core::FrameIndex shift =
        static_cast<core::FrameIndex>(std::llround(alignment->shiftMs * sequenceFps() / 1000.0));
    if (shift == 0) {
        showStatus("Already in sync.");
        return;
    }
    const core::Clip &b = m_model.clip(other);
    if (!m_undoStack.execute(std::make_unique<core::MoveClip>(other, b.track, b.position + shift))) {
        showStatus(
            "Can't sync: the clip would land on another clip on its track (or before the start). Nothing moved.");
        return;
    }
    refreshTimeline();
    showStatus("Synced: moved the clip " + std::to_string(std::abs(shift)) +
               (std::abs(shift) == 1 ? " frame " : " frames ") + (shift > 0 ? "later." : "earlier."));
}

void AppWindow::onRemoveTransitionClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (!m_contextMenuTransitionId.isValid())
        return;

    if (m_undoStack.execute(std::make_unique<core::RemoveTransition>(m_contextMenuTransitionId))) {
        refreshTimeline();
        showStatus("Removed dissolve.");
    } else {
        showStatus("Couldn't remove that transition.");
    }
}

void AppWindow::onAddTransitionClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (!m_contextMenuAddTransitionA.isValid() || !m_contextMenuAddTransitionB.isValid())
        return;

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    const core::Clip &clipA = m_model.clip(m_contextMenuAddTransitionA);
    const core::Clip &clipB = m_model.clip(m_contextMenuAddTransitionB);

    // Default length: about half a second, split between both clips' own
    // handles -- unlike a drag-created transition (which attributes the
    // whole length to whichever edge was actually dragged), there's no
    // single side to prefer here, so try half from each, and hand
    // whatever one side can't use to the other (clamped again there).
    core::FrameIndex targetLength =
        std::max<core::FrameIndex>(1, static_cast<core::FrameIndex>(sequenceFps() * 0.5 + 0.5));
    core::FrameIndex handleA = 0;
    if (m_model.hasAsset(clipA.asset)) {
        const core::Asset &asset = m_model.asset(clipA.asset);
        handleA = asset.info.isBoundless()
                      ? targetLength
                      : std::max<core::FrameIndex>(0, asset.info.lengthInSequenceFrames - 1 - clipA.out);
    }
    core::FrameIndex handleB = clipB.in; // source starts at 0, so `in` itself is the available head room

    core::FrameIndex extendA = std::min(targetLength / 2, handleA);
    core::FrameIndex remaining = targetLength - extendA;
    core::FrameIndex extendB = std::min(remaining, handleB);
    core::FrameIndex shortfall = remaining - extendB;
    if (shortfall > 0)
        extendA = std::min(handleA, extendA + shortfall);

    if (extendA + extendB <= 0) {
        showStatus("Couldn't add a transition there — neither clip has spare source frames.");
        return;
    }

    if (m_undoStack.execute(std::make_unique<core::AddTransition>(trackId, clipA.id, clipB.id, extendA, extendB))) {
        refreshTimeline();
        showStatus("Created a " + std::to_string(extendA + extendB) + "-frame dissolve.");
    } else {
        showStatus("Couldn't add a transition there.");
    }
}

void AppWindow::onRemoveTrackClicked()
{
    gtk_popover_popdown(m_trackContextMenu);

    if (m_contextMenuTrack < 0)
        return;

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    if (m_undoStack.execute(std::make_unique<core::RemoveTrack>(trackId))) {
        int trackCount = static_cast<int>(m_model.sequence().tracks.size());
        m_activeTrack = std::clamp(m_activeTrack, 0, std::max(trackCount - 1, 0));
        m_timelineController.selection().clear();
        refreshTimeline();
        showStatus("Removed track " + std::to_string(m_contextMenuTrack) + ".");
    } else {
        showStatus("Couldn't remove that track.");
    }
    m_contextMenuTrack = -1;
}

void AppWindow::onToggleTrackFlag(TrackFlag flag)
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuTrack < 0)
        return;

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    const core::Track &track = m_model.track(trackId);
    bool muted = track.muted, hidden = track.hidden, locked = track.locked;
    const char *status = nullptr;
    switch (flag) {
    case TrackFlag::Lock:
        locked = !locked;
        status = locked ? "Track locked." : "Track unlocked.";
        break;
    case TrackFlag::Hide:
        hidden = !hidden;
        status = hidden ? "Track hidden." : "Track shown.";
        break;
    case TrackFlag::Mute:
        muted = !muted;
        status = muted ? "Track muted." : "Track unmuted.";
        break;
    }
    if (m_undoStack.execute(std::make_unique<core::SetTrackFlags>(trackId, muted, hidden, locked))) {
        refreshTimeline();
        showStatus(status);
    } else {
        showStatus("Couldn't change that track.");
    }
    m_contextMenuTrack = -1;
}

void AppWindow::onEditTrackNameClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuTrack < 0)
        return;
    beginTrackNameEdit(m_contextMenuTrack);
}

void AppWindow::onTrackVolumeChanged()
{
    if (m_suppressTrackVolumeSignal || m_contextMenuTrack < 0)
        return;
    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    double volume = gtk_range_get_value(GTK_RANGE(m_trackVolumeScale));
    // Not gated on hasTrack/success feedback: a slider drag fires many of
    // these, and SetTrackVolume::mergeWith coalesces them into one undo
    // step already -- a status message per tick would just be noise.
    m_undoStack.execute(std::make_unique<core::SetTrackVolume>(trackId, volume));
}

bool AppWindow::onTrackDragBegin(double x, double y, timeline::Modifiers mods)
{
    if (m_model.sequence().tracks.empty())
        return false;
    if ((m_overlayDrag = overlayClaimsPress(x, y, 1))) {
        m_overlayDragX = x;
        m_overlayDragY = y;
        return true;
    }
    timeline::TimelineOutcome outcome = m_timelineController.press(timelineContext(), x, y, mods);
    applyTimelineOutcome(outcome);
    return true;
}

void AppWindow::onTrackDragUpdate(double offsetX, double offsetY)
{
    if (m_overlayDrag) {
        m_overlayDragEditing = true;
        m_overlayDrag->dragged(m_model, m_viewport, rowLayout(), m_overlayDragX + offsetX, m_overlayDragY + offsetY,
                               false);
        m_overlayDragEditing = false;
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return;
    }
    if (m_timelineController.mode() == timeline::TimelineController::Mode::None)
        return;
    timeline::TimelineOutcome outcome = m_timelineController.motion(timelineContext(), offsetX, offsetY);
    applyTimelineOutcome(outcome);
}

// A provider's drag gets pointer positions as press + gesture offset; if the
// timeline ever autoscrolls at its edges, that scroll must be forwarded to
// the provider mid-drag (or kept off), or its offsets drift from the view.
void AppWindow::cancelOverlayDrag()
{
    if (timeline::TimelineOverlayProvider *overlay = std::exchange(m_overlayDrag, nullptr)) {
        overlay->dragCancelled();
        if (m_timeline)
            gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
    }
}

void AppWindow::onTrackDragEnd(double offsetX, double offsetY)
{
    if (timeline::TimelineOverlayProvider *overlay = std::exchange(m_overlayDrag, nullptr)) {
        m_overlayDragEditing = true;
        overlay->dragged(m_model, m_viewport, rowLayout(), m_overlayDragX + offsetX, m_overlayDragY + offsetY, true);
        m_overlayDragEditing = false;
        refreshTimeline();
        return;
    }
    timeline::TimelineOutcome outcome = m_timelineController.release(timelineContext(), offsetX, offsetY);
    applyTimelineOutcome(outcome);
    refreshTimeline();
}

namespace {

// A label layout for one snapshot, in the timeline's font (generic Pango
// families, since the brand fonts may not be installed).
PangoLayout *newLabelLayout(GtkWidget *widget, bool monospace)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, nullptr);
    PangoFontDescription *font = pango_font_description_from_string(monospace ? "Monospace 8" : "Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    return layout;
}

} // namespace

void AppWindow::snapshotTimelineView(GtkSnapshot *snapshot, int width, int height)
{
    core::trace::Scope trace("timeline snapshot");
    m_timelineThumbnails->newFrameGeneration(); // older requests the view no longer shows can be dropped
    PangoLayout *layout = newLabelLayout(m_timeline, false);
    timeline::TimelineScene scene{
        .model = m_model,
        .viewport = m_viewport,
        .controller = m_timelineController,
        .layout = rowLayout(),
        .handleWidth = kHandleWidth,
        .activeRow = m_activeTrack,
        .nameEditRow = m_inlineEditKind == InlineEditKind::Track ? m_inlineEditTrackRow : -1,
        // Cached peaks, or null (and a worker started) until they're ready.
        .waveformFor = [this](const core::Clip &clip) -> const std::vector<float> * {
            if (!m_model.hasAsset(clip.asset) || m_model.asset(clip.asset).path.empty())
                return nullptr;
            return m_waveforms->peaksFor(m_model.asset(clip.asset).path, static_cast<int>(clip.in),
                                         static_cast<int>(clip.out), m_model.sequence().profile.fps);
        },
        .thumbnailFor = [this](const core::Clip &clip, core::FrameIndex frame) -> GdkTexture * {
            if (!m_model.hasAsset(clip.asset) || m_model.asset(clip.asset).path.empty())
                return nullptr;
            const std::string &path = m_model.asset(clip.asset).path;
            core::Rational fps = m_model.sequence().profile.fps;
            const engine::ThumbnailCache::Data *data =
                m_timelineThumbnails->frameThumbnail(path, static_cast<int>(frame), fps.num, fps.den);
            if (!data)
                return nullptr;
            return m_thumbnailTextures.get(path + '\n' + std::to_string(frame) + '@' + std::to_string(fps.num) + '/' +
                                               std::to_string(fps.den),
                                           data->rgba, data->width, data->height);
        },
        .overlays = {m_timelineOverlays.begin(), m_timelineOverlays.end()},
        .waveformTextures = &m_waveformTextures,
        .scaleFactor = gtk_widget_get_scale_factor(m_timeline),
        .labelLayout = layout,
    };
    // Settings > Toggles: without a callback the renderer neither draws nor
    // requests them.
    if (!m_showWaveforms)
        scene.waveformFor = nullptr;
    if (!m_showTimelineThumbnails)
        scene.thumbnailFor = nullptr;
    timeline::snapshotTimeline(snapshot, scene, width, height);
    g_object_unref(layout);
    noteSelectionForShell();
    noteTransformSelection();
}

void AppWindow::snapshotPlayheadOverlay(GtkSnapshot *snapshot, int width, int height)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0 || sequenceFrames() <= 0)
        return;
    timeline::snapshotPlayhead(snapshot, m_viewport, m_engine->currentFrame(), kHandleWidth, width,
                               std::min(static_cast<double>(height), rowLayout().contentHeight(trackCount)));
}

void AppWindow::snapshotRulerView(GtkSnapshot *snapshot, int width, int height)
{
    if (sequenceFrames() <= 0)
        return;
    PangoLayout *layout = newLabelLayout(m_rulerArea, true);
    timeline::RulerScene scene{
        .model = m_model,
        .viewport = m_viewport,
        .handleWidth = kHandleWidth,
        .fps = sequenceFps(),
        .formatTimecode = [this](core::FrameIndex frame) { return formatTimecode(static_cast<int>(frame)); },
        .labelLayout = layout,
    };
    timeline::snapshotRuler(snapshot, scene, width, height);
    g_object_unref(layout);
}

void AppWindow::onFrameReady(std::vector<uint8_t> rgba, int width, int height, int frameNumber)
{
    core::trace::Scope trace("frame: present");
    if (!rgba.empty() && width > 0 && height > 0) {
        GBytes *bytes = g_bytes_new(rgba.data(), rgba.size());
        GdkTexture *texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
        g_bytes_unref(bytes);
        gtk_picture_set_paintable(m_preview, GDK_PAINTABLE(texture));
        g_object_unref(texture);
    }
    redrawPreviewOverlays(); // IP5: handles follow the frame (none without drop-ins)

    refreshTransport(frameNumber);
    m_shellPlayheadMoved.emit(); // IP5: nothing listens without drop-ins
}

void AppWindow::onWaveformReady()
{
    core::trace::Scope trace("onWaveformReady");
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onThumbnailReady()
{
    core::trace::Scope trace("onThumbnailReady");
    // Audit A4: a thumbnail finishing while the panel is hidden has
    // nothing on screen to update -- rebuilding it anyway means N full
    // rebuilds (destroying and recreating every row) for N assets
    // imported at once, none of them visible. onToggleMediaBrowserClicked
    // already runs its own refreshMediaBrowser() when the panel goes
    // from hidden to visible, which picks up everything that finished
    // in the meantime in a single rebuild.
    if (!gtk_widget_get_visible(m_mediaBrowserPanel))
        return;
    refreshMediaThumbnails();
}

void AppWindow::onToggleMediaBrowserClicked()
{
    bool visible = gtk_widget_get_visible(m_mediaBrowserPanel);
    gtk_widget_set_visible(m_mediaBrowserPanel, !visible);
    if (!visible)
        queueRefresh(); // was hidden: stale or never built; after layout (refreshMediaBrowser())
}

void AppWindow::recordRecentProject(const std::string &path)
{
    char *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (!uri)
        return;
    gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
    g_free(uri);
    // No direct refreshRecentProjectsMenu() call here -- found live that
    // gtk_recent_manager_get_items() right after add_item(), in the same
    // call stack, doesn't yet see the item just added (GtkRecentManager
    // updates its in-memory list asynchronously and emits "changed" once
    // it has). buildUi() connects that signal to refreshRecentProjectsMenu()
    // instead, which is the correct source of truth regardless of this
    // timing.
}

void AppWindow::refreshRecentProjectsMenu()
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 260, -1);

    GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());

    // Newest first, capped at Settings::recentProjectsMax() -- a menu, not
    // a full history browser. Sorts plain int64 timestamps extracted up
    // front rather than calling back into GtkRecentInfo per comparison.
    //
    // gtk_recent_info_get_modified() is transfer-none (Gtk-4.0.gir): the
    // GDateTime belongs to the GtkRecentInfo and must NOT be unreffed here.
    // An earlier version did unref it, freeing timestamps the recent
    // manager still owned. That crashed in g_time_zone_get_offset twice:
    // first inside a g_list_sort comparator on a later refresh (once
    // blamed on a "large real history", which was wrong), then, after
    // sanitizer report S2 made SIGTERM reach the normal shutdown path,
    // when gtk_application_shutdown serialised recently-used.xbel from
    // the freed dates (2026-09-23, coredumpctl backtrace).
    std::vector<std::pair<gint64, GtkRecentInfo *>> entries;
    for (GList *l = items; l != nullptr; l = l->next) {
        auto *info = static_cast<GtkRecentInfo *>(l->data);
        const char *uri = gtk_recent_info_get_uri(info);
        // GtkRecentManager is shared system-wide across every app on the
        // desktop -- filtering by extension is the only practical way to
        // show only this app's own history, since this app doesn't
        // register a custom MIME type to filter on instead.
        if (!uri || !g_str_has_suffix(uri, ".ustudio"))
            continue;
        GDateTime *modified = gtk_recent_info_get_modified(info); // transfer none -- see above
        gint64 timestamp = modified ? g_date_time_to_unix(modified) : 0;
        entries.emplace_back(timestamp, info);
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

    int shown = 0;
    const int maxShown = m_settings->recentProjectsMax();
    for (const auto &entry : entries) {
        if (shown >= maxShown)
            break;
        GtkRecentInfo *info = entry.second;
        char *path = g_filename_from_uri(gtk_recent_info_get_uri(info), nullptr, nullptr);
        if (!path)
            continue;

        GtkWidget *button = gtk_button_new_with_label(gtk_recent_info_get_display_name(info));
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
        g_object_set_data_full(G_OBJECT(button), "ustudio-recent-path", g_strdup(path), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(&AppWindow::recentProjectClickedTrampoline), this);
        gtk_box_append(GTK_BOX(box), button);
        g_free(path);
        ++shown;
    }
    // GtkRecentInfo is its own refcounted boxed type (gtk_recent_info_ref/
    // _unref), not a GObject -- g_object_unref() here segfaults inside
    // GObject's own type-check machinery on the first real call (found
    // live: a standalone repro via gdb reproduced it immediately).
    g_list_free_full(items, reinterpret_cast<GDestroyNotify>(gtk_recent_info_unref));

    if (shown == 0) {
        GtkWidget *label = gtk_label_new("No recent projects");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_widget_set_margin_top(label, 6);
        gtk_widget_set_margin_bottom(label, 6);
        gtk_widget_set_margin_start(label, 6);
        gtk_widget_set_margin_end(label, 6);
        gtk_box_append(GTK_BOX(box), label);
    }

    gtk_popover_set_child(m_recentProjectsPopover, box);
}

void AppWindow::openRecentProject(const std::string &path)
{
    gtk_popover_popdown(m_recentProjectsPopover);
    confirmDiscardIfDirty([this, path] { loadProjectFromPath(path); });
}

void AppWindow::setUpMediaList(GtkWidget *scroller)
{
    // M4 D: a GtkListView over the bin's asset ids (strings, in bin
    // order), so only rows on screen exist and a change rebinds only the
    // rows it touches. Rebuilding every row took 81-105 ms per refresh at
    // 500 assets, and showing the panel stalled 1.2 s laying out ~3,000
    // widgets (debug build, 2026-09-25).
    m_mediaIds = gtk_string_list_new(nullptr);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(+[](GtkSignalListItemFactory *, GObject *item, gpointer self) {
                         static_cast<AppWindow *>(self)->setUpMediaRow(GTK_LIST_ITEM(item));
                     }),
                     this);
    g_signal_connect(factory, "bind", G_CALLBACK(+[](GtkSignalListItemFactory *, GObject *item, gpointer self) {
                         static_cast<AppWindow *>(self)->bindMediaRow(GTK_LIST_ITEM(item));
                     }),
                     this);
    g_signal_connect(factory, "unbind", G_CALLBACK(+[](GtkSignalListItemFactory *, GObject *item, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         const core::AssetId id = window->mediaItemAsset(GTK_LIST_ITEM(item));
                         GtkWidget *row = gtk_list_item_get_child(GTK_LIST_ITEM(item));
                         auto bound = window->m_mediaBoundThumbs.find(id.value);
                         if (row && bound != window->m_mediaBoundThumbs.end() &&
                             bound->second == g_object_get_data(G_OBJECT(row), "thumb"))
                             window->m_mediaBoundThumbs.erase(bound);
                     }),
                     this);
    GtkNoSelection *selection = gtk_no_selection_new(G_LIST_MODEL(m_mediaIds));      // takes the list
    m_mediaBrowserList = gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory); // takes both
    gtk_widget_add_css_class(m_mediaBrowserList, "media-list");
    gtk_widget_set_margin_top(m_mediaBrowserList, 6);
    gtk_widget_set_margin_bottom(m_mediaBrowserList, 6);
    gtk_widget_set_margin_start(m_mediaBrowserList, 6);
    gtk_widget_set_margin_end(m_mediaBrowserList, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), m_mediaBrowserList);
}

void AppWindow::setUpMediaRow(GtkListItem *item)
{
    // The widgets of one row, reused for whichever asset it's bound to.
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto keep = [row](const char *name, GtkWidget *widget) { g_object_set_data(G_OBJECT(row), name, widget); };

    GtkWidget *thumbCell = gtk_picture_new();
    gtk_widget_set_size_request(thumbCell, 120, 68);
    gtk_picture_set_content_fit(GTK_PICTURE(thumbCell), GTK_CONTENT_FIT_CONTAIN);
    gtk_box_append(GTK_BOX(row), thumbCell);
    keep("thumb", thumbCell);

    // Name over its badges (M4 D).
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(info, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(info, 140, -1);
    GtkWidget *nameLabel = gtk_label_new(nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(nameLabel), 0.0);
    gtk_box_append(GTK_BOX(info), nameLabel);
    GtkWidget *badges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(info), badges);
    gtk_box_append(GTK_BOX(row), info);
    keep("name", nameLabel);
    keep("badges", badges);

    auto column = [&](const char *name, int width) {
        GtkWidget *label = gtk_label_new(nullptr);
        gtk_widget_add_css_class(label, "dim-label");
        gtk_widget_set_size_request(label, width, -1);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_box_append(GTK_BOX(row), label);
        keep(name, label);
    };
    column("length", 60);
    column("fps", 40);
    column("format", 50);

    GtkGesture *rowRightClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rowRightClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rowRightClick, "pressed", G_CALLBACK(&AppWindow::mediaBrowserRowRightClickTrampoline), this);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(rowRightClick));

    // Enhancement #8: double-click inserts at the playhead on the
    // active track -- default GDK_BUTTON_PRIMARY, so left-click only.
    GtkGesture *rowActivate = gtk_gesture_click_new();
    g_signal_connect(rowActivate, "pressed", G_CALLBACK(&AppWindow::mediaBrowserRowActivatedTrampoline), this);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(rowActivate));

    // Drag-to-timeline: the content (the AssetId's value, a G_TYPE_INT64
    // onTimelineDrop looks up in the bin) is set when the row is bound.
    GtkDragSource *dragSource = gtk_drag_source_new();
    gtk_drag_source_set_actions(dragSource, GDK_ACTION_COPY);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(dragSource));
    g_object_set_data(G_OBJECT(row), "drag-source", dragSource);

    gtk_list_item_set_child(item, row);
}

core::AssetId AppWindow::mediaItemAsset(GtkListItem *item) const
{
    auto *entry = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    return core::AssetId{entry ? std::strtoull(gtk_string_object_get_string(entry), nullptr, 10) : 0};
}

void AppWindow::bindMediaRow(GtkListItem *item)
{
    GtkWidget *row = gtk_list_item_get_child(item);
    const core::AssetId id = mediaItemAsset(item);
    if (!row || !m_model.hasAsset(id))
        return;
    const core::Asset &asset = m_model.asset(id);
    auto part = [row](const char *name) { return GTK_WIDGET(g_object_get_data(G_OBJECT(row), name)); };
    // Right-click and double-click read which asset the row is for.
    g_object_set_data(G_OBJECT(row), "ustudio-asset-id", reinterpret_cast<void *>(static_cast<uintptr_t>(id.value)));

    // thumbnailFor() queues a background job on a miss and returns null:
    // the row shows an empty cell until onThumbnailReady() rebinds it.
    GdkTexture *texture = nullptr;
    if (const engine::ThumbnailCache::Data *thumb = m_thumbnails->thumbnailFor(asset.path))
        texture = m_mediaTextures.get(asset.path, thumb->rgba, thumb->width, thumb->height);
    if (!texture)
        m_mediaAwaitingThumbnail.insert(id.value);
    gtk_picture_set_paintable(GTK_PICTURE(part("thumb")), texture ? GDK_PAINTABLE(texture) : nullptr);
    m_mediaBoundThumbs[id.value] = part("thumb");

    gtk_label_set_text(GTK_LABEL(part("name")), asset.displayName.c_str());
    gtk_widget_set_tooltip_text(part("name"), asset.path.c_str());
    gtk_label_set_text(GTK_LABEL(part("length")), formatMediaLength(asset.info.nativeDurationSeconds).c_str());
    gtk_label_set_text(GTK_LABEL(part("fps")), formatMediaFps(asset.info.fps).c_str());
    std::string formatText = asset.info.container;
    for (char &c : formatText)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    gtk_label_set_text(GTK_LABEL(part("format")), formatText.empty() ? "—" : formatText.c_str());

    // The row's badge labels are reused across binds (a list view keeps
    // ~200 rows and rebinds them as it scrolls); more are made only when
    // an asset has more badges than the row has had before.
    const std::vector<MediaBadge> badges = mediaBadgesOf(asset);
    GtkWidget *badgeBox = part("badges");
    GtkWidget *label = gtk_widget_get_first_child(badgeBox);
    for (const MediaBadge &badge : badges) {
        if (!label) {
            label = gtk_label_new(nullptr);
            gtk_box_append(GTK_BOX(badgeBox), label);
        }
        gtk_label_set_text(GTK_LABEL(label), badge.text.c_str());
        const char *classes[] = {"media-badge", badge.cssClass.c_str(), nullptr};
        gtk_widget_set_css_classes(label, classes);
        gtk_widget_set_visible(label, TRUE);
        label = gtk_widget_get_next_sibling(label);
    }
    for (; label; label = gtk_widget_get_next_sibling(label))
        gtk_widget_set_visible(label, FALSE);

    GdkContentProvider *content = gdk_content_provider_new_typed(G_TYPE_INT64, static_cast<gint64>(id.value));
    gtk_drag_source_set_content(GTK_DRAG_SOURCE(g_object_get_data(G_OBJECT(row), "drag-source")), content);
    g_object_unref(content);
}

std::vector<MediaBadge> AppWindow::mediaBadgesOf(const core::Asset &asset) const
{
    ProxyState proxy;
    if (m_proxyQueue)
        proxy.progress = m_proxyQueue->progressOf(asset.id);
    std::error_code ec;
    proxy.fileExists =
        !asset.proxyPath.empty() && std::filesystem::is_regular_file(core::pathFromUtf8(asset.proxyPath), ec);
    return mediaBadges(asset, m_model.sequence().profile, proxy);
}

void AppWindow::refreshMediaBrowser()
{
    core::trace::Scope trace("refreshMediaBrowser");
    // Not while hidden: a list view with no height binds ~200 rows for
    // nothing (52 ms at 500 assets). Showing the panel refreshes it, from
    // an idle, once it has its height and binds only the rows on screen.
    if (!gtk_widget_get_visible(m_mediaBrowserPanel))
        return;
    // Just shown and not laid out yet (an idle runs before the next frame's
    // layout): wait for its height, once, or it binds ~200 rows anyway.
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(m_mediaBrowserPanel));
    if (gtk_adjustment_get_page_size(vertical) <= 0) {
        if (!m_mediaWaitingForHeight) {
            m_mediaWaitingForHeight = true;
            g_signal_connect(vertical, "changed", G_CALLBACK(+[](GtkAdjustment *adjustment, gpointer self) {
                                 auto *window = static_cast<AppWindow *>(self);
                                 if (!window->m_mediaWaitingForHeight || gtk_adjustment_get_page_size(adjustment) <= 0)
                                     return;
                                 window->m_mediaWaitingForHeight = false;
                                 window->queueRefresh();
                             }),
                             this);
        }
        return;
    }
    // Each asset's row as data: an unchanged one isn't touched; a changed
    // one is rebound (its item replaced); a new order replaces them all,
    // which a list view makes cheap (only rows on screen are built).
    std::vector<std::pair<uint64_t, std::string>> rows;
    const auto &bin = m_model.project().bin;
    rows.reserve(bin.size());
    for (const core::Asset &asset : bin) {
        std::string signature = asset.displayName + '\n' + asset.path + '\n' + asset.info.container + '\n' +
                                formatMediaLength(asset.info.nativeDurationSeconds) + '\n' +
                                formatMediaFps(asset.info.fps);
        for (const MediaBadge &badge : mediaBadgesOf(asset))
            signature += '\n' + badge.text + ' ' + badge.cssClass;
        rows.emplace_back(asset.id.value, std::move(signature));
    }
    // The same assets in the same order, maybe with more after them (an
    // import appends): rebind the changed rows, append the new ones. A
    // removal or a reorder replaces the list.
    const bool samePrefix = rows.size() >= m_mediaRows.size() &&
                            std::equal(m_mediaRows.begin(), m_mediaRows.end(), rows.begin(),
                                       [](const auto &a, const auto &b) { return a.first == b.first; });
    if (samePrefix) {
        for (size_t i = 0; i < m_mediaRows.size(); ++i)
            if (rows[i].second != m_mediaRows[i].second)
                replaceMediaItem(static_cast<guint>(i), rows[i].first);
        if (rows.size() > m_mediaRows.size()) {
            std::vector<std::string> added;
            for (size_t i = m_mediaRows.size(); i < rows.size(); ++i)
                added.push_back(std::to_string(rows[i].first));
            std::vector<const char *> raw;
            for (const std::string &id : added)
                raw.push_back(id.c_str());
            raw.push_back(nullptr);
            gtk_string_list_splice(m_mediaIds, static_cast<guint>(m_mediaRows.size()), 0, raw.data());
        }
    } else {
        std::vector<std::string> ids;
        ids.reserve(rows.size());
        for (const auto &[id, signature] : rows)
            ids.push_back(std::to_string(id));
        std::vector<const char *> raw;
        raw.reserve(ids.size() + 1);
        for (const std::string &id : ids)
            raw.push_back(id.c_str());
        raw.push_back(nullptr);
        gtk_string_list_splice(m_mediaIds, 0, g_list_model_get_n_items(G_LIST_MODEL(m_mediaIds)), raw.data());
    }
    m_mediaRows = std::move(rows);
}

void AppWindow::replaceMediaItem(guint position, uint64_t id)
{
    // The same id again: the list view rebinds that row.
    const std::string text = std::to_string(id);
    const char *items[] = {text.c_str(), nullptr};
    gtk_string_list_splice(m_mediaIds, position, 1, items);
}

void AppWindow::refreshMediaThumbnails()
{
    // Thumbnails that finished since their rows were bound: onto the rows
    // still showing those assets, without rebinding them. A row bound
    // later picks its thumbnail up in bindMediaRow().
    std::set<uint64_t> waiting;
    waiting.swap(m_mediaAwaitingThumbnail);
    for (uint64_t id : waiting) {
        auto bound = m_mediaBoundThumbs.find(id);
        if (bound == m_mediaBoundThumbs.end() || !m_model.hasAsset(core::AssetId{id}))
            continue; // not on a row any more: nothing to update
        const std::string &path = m_model.asset(core::AssetId{id}).path;
        const engine::ThumbnailCache::Data *thumb = m_thumbnails->thumbnailFor(path);
        GdkTexture *texture = thumb ? m_mediaTextures.get(path, thumb->rgba, thumb->width, thumb->height) : nullptr;
        if (!texture) {
            m_mediaAwaitingThumbnail.insert(id);
            continue;
        }
        gtk_picture_set_paintable(GTK_PICTURE(bound->second), GDK_PAINTABLE(texture));
    }
}

void AppWindow::onMediaBrowserRowRightClicked(core::AssetId assetId, GtkWidget *row, double x, double y)
{
    m_contextMenuAssetId = assetId;
    updateProxyMenuItems();

    graphene_point_t local = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y));
    graphene_point_t inPanel{};
    if (!gtk_widget_compute_point(row, m_mediaBrowserPanel, &local, &inPanel))
        inPanel = local; // row is always a live descendant of m_mediaBrowserPanel; kept as a harmless fallback

    GdkRectangle rect{static_cast<int>(inPanel.x), static_cast<int>(inPanel.y), 1, 1};
    gtk_popover_set_pointing_to(m_mediaBrowserContextMenu, &rect);
    gtk_popover_popup(m_mediaBrowserContextMenu);
}

void AppWindow::onRemoveAssetClicked()
{
    gtk_popover_popdown(m_mediaBrowserContextMenu);
    if (!m_contextMenuAssetId.isValid() || !m_model.hasAsset(m_contextMenuAssetId))
        return;
    std::string name = m_model.asset(m_contextMenuAssetId).displayName;

    if (m_undoStack.execute(std::make_unique<core::RemoveAsset>(m_contextMenuAssetId))) {
        refreshTimeline();
        refreshMediaBrowser();
        showStatus("Removed " + name + " from the project.");
    } else {
        showStatus("Can't remove " + name + ": used by a clip on a locked track.");
    }
}

void AppWindow::onDeleteAssetFileClicked()
{
    gtk_popover_popdown(m_mediaBrowserContextMenu);
    if (!m_contextMenuAssetId.isValid() || !m_model.hasAsset(m_contextMenuAssetId))
        return;
    const core::Asset &asset = m_model.asset(m_contextMenuAssetId);

    int clipCount = 0;
    for (const auto &[clipId, clip] : m_model.sequence().clips)
        if (clip.asset == m_contextMenuAssetId)
            ++clipCount;

    std::string body = "Move \"" + asset.displayName + "\" to Trash?";
    if (clipCount > 0)
        body += " It's used by " + std::to_string(clipCount) + (clipCount == 1 ? " clip" : " clips") +
                " in this project -- those will be removed too.";

    AdwDialog *dialog = adw_alert_dialog_new("Move file to Trash?", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", "Move to Trash");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct DeleteContext
    {
        AppWindow *self;
        core::AssetId assetId;
        std::string path;
        std::string displayName;
    };
    auto *ctx = new DeleteContext{this, m_contextMenuAssetId, asset.path, asset.displayName};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<DeleteContext> owned(static_cast<DeleteContext *>(userData));
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (!response || std::string(response) != "delete")
                return;

            AppWindow *self = owned->self;
            if (!self->m_model.hasAsset(owned->assetId)) {
                self->showStatus("Can't delete " + owned->displayName + ": no longer in the project.");
                return;
            }

            if (!self->m_undoStack.execute(std::make_unique<core::RemoveAsset>(owned->assetId))) {
                self->showStatus("Can't delete " + owned->displayName + ": used by a clip on a locked track.");
                return;
            }
            self->refreshTimeline();
            self->refreshMediaBrowser();

            // The project no longer references it either way (RemoveAsset
            // above already succeeded); a failed trash just leaves the
            // now-orphaned file on disk. Audit A3: this used to be
            // std::filesystem::remove -- a permanent unlink with no
            // recovery path -- despite the dialog only asking about
            // removing it from the *project*. g_file_trash() (GIO,
            // already a dependency; CLAUDE.md's "never execute/shell-
            // interpolate a project-file-derived path" rule is why this
            // isn't system("gio trash ...")) moves it to the desktop's
            // Trash instead, so it's still recoverable the normal way a
            // GNOME user expects.
            std::error_code ec;
            if (!std::filesystem::exists(owned->path, ec)) {
                self->showStatus("Removed " + owned->displayName + " (file was already gone).");
                return;
            }
            GFile *file = g_file_new_for_path(owned->path.c_str());
            GError *error = nullptr;
            if (!g_file_trash(file, nullptr, &error)) {
                Log::error("[app] could not move " + owned->path + " to Trash: " + error->message);
                self->showStatus("Removed " + owned->displayName +
                                 " from the project, but could not move the file to Trash: " + error->message);
                g_error_free(error);
            } else {
                self->showStatus("Moved " + owned->displayName + " to Trash.");
            }
            g_object_unref(file);
        },
        ctx);
}

gboolean AppWindow::onTimelineDrop(const GValue *value, double x, double y)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0 || !G_VALUE_HOLDS_INT64(value))
        return FALSE;

    core::AssetId assetId{static_cast<uint64_t>(g_value_get_int64(value))};
    if (!m_model.hasAsset(assetId))
        return FALSE;
    const core::Asset &asset = m_model.asset(assetId);

    const timeline::RowLayout layout = rowLayout();
    if (layout.inTopLane(y))
        return FALSE; // a drop-in's top lane: not a track
    int row = layout.clampedRowAt(y, trackCount);
    core::TrackId trackId = trackIdForRow(row);
    if (m_model.track(trackId).locked) {
        showStatus("Can't drop onto a locked track.");
        return FALSE;
    }

    if (x < kHandleWidth)
        return FALSE;

    core::FrameIndex dropFrame = m_viewport.frameForX(x);

    core::FrameIndex length =
        effectiveInsertLength(asset.info.isBoundless(), asset.info.lengthInSequenceFrames, dropFrame);

    if (!m_model.isRangeFree(trackId, dropFrame, dropFrame + length)) {
        showStatus("Can't drop " + asset.displayName + " there: it would overlap another clip.");
        return FALSE;
    }

    if (m_undoStack.execute(std::make_unique<core::InsertClip>(trackId, assetId, dropFrame, 0, length - 1))) {
        refreshTimeline();
        showStatus("Added " + asset.displayName + " to track " + std::to_string(row) + ".");
        return TRUE;
    }
    return FALSE;
}

gboolean AppWindow::onTimelineFileDrop(GdkFileList *files, double x, double y)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0 || !files)
        return FALSE;

    const timeline::RowLayout layout = rowLayout();
    if (layout.inTopLane(y))
        return FALSE; // a drop-in's top lane: not a track
    int row = layout.clampedRowAt(y, trackCount);
    core::TrackId trackId = trackIdForRow(row);
    if (m_model.track(trackId).locked) {
        showStatus("Can't drop onto a locked track.");
        return FALSE;
    }

    if (x < kHandleWidth)
        return FALSE;

    core::FrameIndex position = m_viewport.frameForX(x);

    std::vector<std::string> paths;
    GSList *list = gdk_file_list_get_files(files); // transfer container
    for (GSList *l = list; l != nullptr; l = l->next) {
        std::string path = localPathFor(static_cast<GFile *>(l->data));
        if (!path.empty())
            paths.push_back(std::move(path));
    }
    g_slist_free(list);
    if (paths.empty())
        return FALSE;
    // Several files dropped at once land one after another from the drop
    // point, the same "append" rule a multi-select Import uses.
    startImport(std::move(paths), trackId, position);
    return TRUE;
}

gboolean AppWindow::onMediaBrowserFileDrop(GdkFileList *files)
{
    if (!files)
        return FALSE;
    std::vector<std::string> paths;
    GSList *list = gdk_file_list_get_files(files); // transfer container
    for (GSList *l = list; l != nullptr; l = l->next) {
        std::string path = localPathFor(static_cast<GFile *>(l->data));
        if (!path.empty())
            paths.push_back(std::move(path));
    }
    g_slist_free(list);
    if (paths.empty())
        return FALSE;
    startImport(std::move(paths), std::nullopt, std::nullopt);
    return TRUE;
}

bool AppWindow::insertAssetAtPosition(core::AssetId assetId, core::TrackId trackId, core::FrameIndex position)
{
    if (!m_model.hasAsset(assetId))
        return false;
    const core::Asset &asset = m_model.asset(assetId);
    if (m_model.track(trackId).locked) {
        showStatus("Can't insert onto a locked track.");
        return false;
    }

    core::FrameIndex length =
        effectiveInsertLength(asset.info.isBoundless(), asset.info.lengthInSequenceFrames, position);
    if (!m_model.isRangeFree(trackId, position, position + length)) {
        showStatus("Can't insert " + asset.displayName + " there: it would overlap another clip.");
        return false;
    }

    if (m_undoStack.execute(std::make_unique<core::InsertClip>(trackId, assetId, position, 0, length - 1))) {
        refreshTimeline();
        showStatus("Added " + asset.displayName + " at the playhead.");
        return true;
    }
    return false;
}

void AppWindow::onMediaBrowserRowActivated(core::AssetId assetId)
{
    // Enhancement #8: the active track, at the playhead -- the same
    // target a plain click on a timeline row already sets as "where
    // things land" (onActiveTrackUp/Down's own comment).
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;
    core::TrackId trackId = trackIdForRow(m_activeTrack);
    insertAssetAtPosition(assetId, trackId, m_engine->currentFrame());
}

core::FrameIndex AppWindow::effectiveInsertLength(bool isBoundless, core::FrameIndex knownLength,
                                                  core::FrameIndex insertPos) const
{
    if (!isBoundless)
        return knownLength;
    core::FrameIndex timelineLength = m_model.sequence().length();
    if (timelineLength > insertPos)
        return timelineLength - insertPos;
    const core::Rational &fps = m_model.sequence().profile.fps;
    double fpsValue = fps.den > 0 ? static_cast<double>(fps.num) / fps.den : 30.0;
    return static_cast<core::FrameIndex>(fpsValue * 10.0);
}

void AppWindow::beginTrackNameEdit(int row)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (row < 0 || row >= trackCount)
        return;

    m_inlineEditKind = InlineEditKind::Track;
    m_inlineEditTrackRow = row;

    const core::Track &track = m_model.track(trackIdForRow(row));
    GdkRectangle anchor{static_cast<int>(kHandleWidth), static_cast<int>(rowLayout().rowTop(row)), 160,
                        static_cast<int>(kTrackLabelHeight)};
    showInlineNameEditor(anchor, track.name);
}

void AppWindow::beginClipNameEdit(const ClipDisplay &clip)
{
    m_inlineEditKind = InlineEditKind::Clip;
    m_inlineEditClipId = clip.id;

    // The visible part of the clip, so the editor stays on screen when the
    // clip is scrolled half out of view.
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    double left = std::max(xForFrame(clip.startFrame), kHandleWidth);
    double right = std::min(xForFrame(clip.startFrame + clip.frames), static_cast<double>(widgetWidth));
    int anchorX = static_cast<int>(left);
    int anchorW = std::max(static_cast<int>(right - left), 40);
    double rowY = rowLayout().rowTop(clip.trackIndex) + kTrackLabelHeight;
    GdkRectangle anchor{anchorX, static_cast<int>(rowY), anchorW,
                        static_cast<int>(kTrackRowHeight - kTrackLabelHeight)};
    showInlineNameEditor(anchor, clip.name);
}

void AppWindow::showInlineNameEditor(GdkRectangle anchor, const std::string &currentName)
{
    m_inlineEditCancelled = false;
    setTransportActionsEnabled(false);
    gtk_editable_set_text(GTK_EDITABLE(m_inlineNameEditEntry), currentName.c_str());
    gtk_popover_set_pointing_to(m_inlineNameEditPopover, &anchor);
    gtk_popover_popup(m_inlineNameEditPopover);
    gtk_widget_grab_focus(GTK_WIDGET(m_inlineNameEditEntry));
}

void AppWindow::onInlineNameEditClosed()
{
    setTransportActionsEnabled(true);

    InlineEditKind kind = m_inlineEditKind;
    m_inlineEditKind = InlineEditKind::None;

    if (kind == InlineEditKind::None || m_inlineEditCancelled) {
        m_inlineEditCancelled = false;
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return;
    }

    std::string newName = gtk_editable_get_text(GTK_EDITABLE(m_inlineNameEditEntry));

    if (kind == InlineEditKind::Track) {
        core::TrackId trackId = trackIdForRow(m_inlineEditTrackRow);
        if (trackId.isValid() && newName != m_model.track(trackId).name)
            m_undoStack.execute(std::make_unique<core::RenameTrack>(trackId, newName));
    } else {
        if (m_model.hasClip(m_inlineEditClipId) && newName != m_model.clip(m_inlineEditClipId).name)
            m_undoStack.execute(std::make_unique<core::RenameClip>(m_inlineEditClipId, newName));
    }

    refreshTimeline();
}

gboolean AppWindow::onInlineNameEditKeyPressed(guint keyval)
{
    if (keyval == GDK_KEY_Escape) {
        m_inlineEditCancelled = true;
        gtk_popover_popdown(m_inlineNameEditPopover);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

std::string AppWindow::clipTooltipText(core::ClipId id) const
{
    for (const ClipDisplay &clip : m_clips) {
        if (clip.id != id)
            continue;
        std::string name = clip.name.empty() ? std::string("(unnamed)") : clip.name;
        return name + "\n" + formatTimecode(clip.startFrame) + " – " + formatTimecode(clip.startFrame + clip.frames) +
               "\nLength: " + formatTimecode(clip.frames) + " (" + std::to_string(clip.frames) +
               " frames)\nSource: " + (clip.resource.empty() ? std::string("(none)") : clip.resource);
    }
    return {};
}

core::TrackId AppWindow::trackIdForRow(int row) const
{
    const auto &tracks = m_model.sequence().tracks;
    if (row < 0 || row >= static_cast<int>(tracks.size())) {
        Log::error("[app] trackIdForRow: row " + std::to_string(row) + " out of range (" +
                   std::to_string(tracks.size()) + " tracks)");
        return core::TrackId{};
    }
    return tracks[static_cast<size_t>(row)].id;
}

void AppWindow::updateViewportGeometry()
{
    double fps = sequenceFps() > 0.0 ? sequenceFps() : 25.0;
    m_viewport.setOriginX(kHandleWidth);
    m_viewport.setVisibleWidth(gtk_widget_get_width(GTK_WIDGET(m_timeline)) - kHandleWidth);
    // An empty or very short project fits 30 seconds, not a few frames
    // stretched across the whole width.
    m_viewport.setSequenceLength(sequenceFrames(), static_cast<core::FrameIndex>(fps * 30.0));
    syncTimelineScrollbar();
}

void AppWindow::syncTimelineScrollbar()
{
    m_suppressHScrollSignal = true;
    double page = m_viewport.visibleWidth();
    gtk_adjustment_configure(m_timelineHAdjustment, m_viewport.scrollX(), 0.0, m_viewport.extent(), page * 0.1,
                             page * 0.9, page);
    m_suppressHScrollSignal = false;
}

void AppWindow::onTimelineViewportChanged()
{
    syncTimelineScrollbar();
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
    gtk_widget_queue_draw(GTK_WIDGET(m_rulerArea));
    gtk_widget_queue_draw(GTK_WIDGET(m_playheadOverlay));
}

void AppWindow::zoomTimeline(double factor)
{
    // Keyboard zoom anchors on the playhead when it's on screen, else on
    // the middle of the view.
    double anchorX = xForFrame(m_engine->currentFrame());
    int width = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    if (anchorX < kHandleWidth || anchorX > width)
        anchorX = kHandleWidth + m_viewport.visibleWidth() / 2.0;
    m_viewport.zoomAround(anchorX, factor);
    onTimelineViewportChanged();
}

void AppWindow::onZoomIn()
{
    zoomTimeline(1.5);
}

void AppWindow::onZoomOut()
{
    zoomTimeline(1.0 / 1.5);
}

void AppWindow::onZoomFit()
{
    m_viewport.fit();
    onTimelineViewportChanged();
}

gboolean AppWindow::onTimelineScroll(GtkEventControllerScroll *controller, double dx, double dy)
{
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    // A wheel detent is 1.0; a touchpad reports surface pixels.
    bool wheel = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL;

    if (mods & GDK_CONTROL_MASK) {
        double anchorX = m_timelinePointerX;
        double steps = wheel ? dy : dy / 40.0;
        m_viewport.zoomAround(std::max(anchorX, kHandleWidth), std::pow(1.25, -steps));
        onTimelineViewportChanged();
        return TRUE;
    }

    double sideways = (mods & GDK_SHIFT_MASK) ? (dx != 0.0 ? dx : dy) : dx;
    if (sideways == 0.0)
        return FALSE; // a plain vertical wheel: the scroller's
    m_viewport.setScrollX(m_viewport.scrollX() + sideways * (wheel ? 60.0 : 1.0));
    onTimelineViewportChanged();
    return TRUE;
}

void AppWindow::onTimelineHScrollChanged()
{
    if (m_suppressHScrollSignal)
        return;
    m_viewport.setScrollX(gtk_adjustment_get_value(m_timelineHAdjustment));
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
    gtk_widget_queue_draw(GTK_WIDGET(m_rulerArea));
    gtk_widget_queue_draw(GTK_WIDGET(m_playheadOverlay));
}

void AppWindow::refreshTimeline()
{
    core::trace::Scope trace("refreshTimeline");
    m_clips.clear();
    const core::Sequence &seq = m_model.sequence();
    for (size_t row = 0; row < seq.tracks.size(); ++row) {
        const core::Track &track = seq.tracks[row];
        for (core::ClipId clipId : track.clips) {
            const core::Clip &clip = m_model.clip(clipId);
            ClipDisplay display;
            display.id = clipId;
            display.name = clip.name;
            display.resource = m_model.hasAsset(clip.asset) ? m_model.asset(clip.asset).path : std::string{};
            display.trackIndex = static_cast<int>(row);
            display.startFrame = static_cast<int>(clip.position);
            display.frames = static_cast<int>(clip.length());
            display.in = static_cast<int>(clip.in);
            display.out = static_cast<int>(clip.out);
            m_clips.push_back(std::move(display));
        }
    }

    int trackCount = static_cast<int>(seq.tracks.size());
    int height = static_cast<int>(trackCount > 0 ? rowLayout().contentHeight(trackCount) : kTrackRowHeight);
    gtk_widget_set_size_request(GTK_WIDGET(m_timeline), -1, height);
    updateViewportGeometry();

    int total = sequenceFrames();
    m_suppressSeekSignal = true;
    gtk_range_set_range(GTK_RANGE(m_seekScale), 0, std::max(total - 1, 0));
    m_suppressSeekSignal = false;

    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
    // Enhancement #12: the ruler's ticks depend on the same total-frames
    // mapping the timeline itself does, so it needs redrawing on every
    // edit that could change sequence length too -- refreshTimeline() is
    // the one place already called after all of them.
    gtk_widget_queue_draw(GTK_WIDGET(m_rulerArea));
}

void AppWindow::refreshTransport(int frameNumber)
{
    m_suppressSeekSignal = true;
    gtk_range_set_value(GTK_RANGE(m_seekScale), frameNumber);
    m_suppressSeekSignal = false;

    gtk_label_set_text(m_timecodeLabel, formatTimecode(frameNumber).c_str());
    // Follow the playhead a page at a time when it leaves the view
    // (playback, Home/End, cut jumps).
    // (playback, Home/End, cut jumps). With Follow playhead off, playback
    // leaves the view where it is; seeks while stopped still follow.
    if ((m_followPlayhead || !m_engine->isPlaying()) && m_viewport.ensureVisible(frameNumber))
        onTimelineViewportChanged();
    // Keeps the timeline's playhead line in sync with playback, not just
    // with edits -- this runs once per displayed frame (onFrameReady's
    // own comment), both while playing and after a seek (seek() purges
    // and requests a fresh frame, which comes back through the same
    // callback), so nothing else needs to separately queue this redraw.
    // Audit A5: queues only the playhead's own overlay, not the full
    // m_timeline -- see onPlayheadOverlayDraw()'s declaration comment.
    gtk_widget_queue_draw(GTK_WIDGET(m_playheadOverlay));
}

void AppWindow::refreshPlayButtonIcon()
{
    const char *icon = m_engine->isPlaying() ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
    gtk_button_set_icon_name(m_playButton, icon);
}

void AppWindow::refreshLoopStatusLabel()
{
    auto range = m_engine->loopRange();
    if (!range) {
        gtk_label_set_text(m_loopStatusLabel, "");
        return;
    }
    std::string text = "Loop " + formatTimecode(range->first) + " – " + formatTimecode(range->second);
    gtk_label_set_text(m_loopStatusLabel, text.c_str());
}

bool AppWindow::pathIsProjectAsset(const std::string &path) const
{
    std::error_code ec;
    std::filesystem::path candidate = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        candidate = path; // weakly_canonical only fails on a genuinely unusable path; compare as given then
    for (const core::Asset &asset : m_model.project().bin) {
        std::error_code assetEc;
        std::filesystem::path assetPath = std::filesystem::weakly_canonical(asset.path, assetEc);
        if (assetEc)
            assetPath = asset.path;
        if (candidate == assetPath)
            return true;
    }
    return false;
}

void AppWindow::updateWindowTitle()
{
    // Enhancement #4: the project name, not just "U-Stu Video Editor"
    // for every window regardless of which project is open -- every call
    // site that changes m_currentProjectPath (Save, Open, Reload, New
    // Project, recovery) already calls something that emits
    // m_undoStack's "changed" signal right after, which this is
    // connected to, so this always sees the current path.
    std::string docName =
        m_currentProjectPath.empty() ? "Untitled Project" : std::filesystem::path(m_currentProjectPath).stem().string();
    const std::string dirtyMark = m_undoStack.isClean() ? "" : " •";
    // The taskbar gets the app's name too; the header bar has no room for it
    // beside its buttons at the default width.
    gtk_window_set_title(GTK_WINDOW(m_window), (docName + " — U-Stu" + dirtyMark).c_str());
    adw_window_title_set_title(m_windowTitle, (docName + dirtyMark).c_str());
    const core::Profile &format = m_model.sequence().profile;
    adw_window_title_set_subtitle(m_windowTitle, (std::to_string(format.width) + "×" + std::to_string(format.height) +
                                                  " · " + core::formatFps(format.fps) + " fps")
                                                     .c_str());
    // What "Reopen last project on startup" opens next launch. Every change
    // of m_currentProjectPath comes through here (see above).
    if (m_settings->lastProjectPath() != m_currentProjectPath)
        m_settings->setLastProjectPath(m_currentProjectPath);
    gtk_widget_set_sensitive(GTK_WIDGET(m_undoButton), m_undoStack.canUndo());
    gtk_widget_set_sensitive(GTK_WIDGET(m_redoButton), m_undoStack.canRedo());
}

void AppWindow::showStatus(const std::string &text)
{
    ++m_statusCount;
    gtk_label_set_text(m_statusLabel, text.c_str());
    gtk_widget_set_tooltip_text(GTK_WIDGET(m_statusLabel), text.c_str());
    // Every user-visible outcome (import result, save/open/render success
    // or failure, split/delete/close-gap/lock/volume messages, refusals)
    // goes through this one function -- logging it here, once, instead of
    // at each of the dozens of call sites is the only way to get a
    // reliable trail of "what did the user just do" leading up to a crash
    // without relying on someone remembering to log at every future call
    // site too.
    Log::debug("[app] status: " + text);
}

std::string AppWindow::formatTimecode(int frame) const
{
    double fps = sequenceFps();
    int fpsInt = fps > 0.0 ? static_cast<int>(fps + 0.5) : 25;
    if (fpsInt <= 0)
        fpsInt = 25;

    int totalSeconds = frame / fpsInt;
    int frames = frame % fpsInt;
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    return buf;
}

void AppWindow::performAutosave()
{
    std::string dir = autosave::directory();
    if (dir.empty())
        return;

    std::string base = autosave::baseNameFor(m_currentProjectPath, m_autosaveSessionId);
    std::string autosavePath = dir + "/" + base + ".ustudio";
    std::string metaPath = dir + "/" + base + ".meta";

    if (!m_saveQueue)
        return;
    autosave::Meta meta;
    meta.originalPath = m_currentProjectPath;
    meta.timestampUnix = static_cast<int64_t>(std::time(nullptr));
    meta.ownerPid = platform::currentProcessId(); // audit A5: lets a later launch skip a still-live owner
    // audit A3: cross-checked against ownerPid in ownerAlive() so a
    // later, unrelated process reusing this same pid isn't mistaken for
    // this instance still being alive.
    meta.ownerStartTime = autosave::processStartTime(meta.ownerPid);

    std::shared_ptr<const core::Project> snapshot = m_model.snapshot();
    uint64_t generation = m_projectGeneration;
    m_saveQueue->autosave(
        [snapshot, autosavePath, metaPath, meta] {
            core::Model model(*snapshot);
            std::string error = core::saveProject(model, autosavePath);
            if (error.empty() && !autosave::writeMeta(metaPath, meta))
                error = "could not write " + metaPath;
            return error;
        },
        [this, autosavePath, generation, originalPath = m_currentProjectPath](const std::string &error) {
            // Silent either way: an autosave failure shouldn't interrupt
            // the user.
            if (!error.empty()) {
                Log::warn("[app] Autosave failed: " + error);
            } else {
                if (generation == m_projectGeneration)
                    m_unsavedSinceMonotonicUsec = 0;
                m_ownAutosaves->written(originalPath);
                Log::debug("[app] Autosaved to " + autosavePath);
            }
            onSaveQueueSettled();
        });
}

void AppWindow::removeStaleAutosaves(const std::vector<std::string> &bases)
{
    const std::string dir = autosave::directory();
    if (dir.empty())
        return;
    for (const std::string &base : bases) {
        autosave::removeAutosavePair(dir, base);
        Log::debug("[app] Removed stale autosave " + base);
    }
}

void AppWindow::onAutosaveHeartbeat()
{
    // Clean (just saved, loaded, or undone back to the save point): nothing
    // to protect, and the next edit starts a fresh pending window.
    if (m_undoStack.isClean()) {
        m_unsavedSinceMonotonicUsec = 0;
        return;
    }

    // N minutes from Settings (default 2): idle for N, or the oldest
    // unsaved edit about to be N old -- see autosave::autosaveDue().
    const gint64 autosaveDelayUsec = static_cast<gint64>(m_settings->autosaveDelayMinutes()) * 60 * G_USEC_PER_SEC;
    if (autosave::autosaveDue(g_get_monotonic_time(), m_lastEditMonotonicUsec, m_unsavedSinceMonotonicUsec,
                              autosaveDelayUsec, kAutosaveHeartbeatSeconds * G_USEC_PER_SEC))
        performAutosave();
}

void AppWindow::onWindowActiveChanged()
{
    // doc 09: autosave "on focus loss" too, not just the 2-minute timer.
    if (!gtk_window_is_active(GTK_WINDOW(m_window)) && !m_undoStack.isClean())
        performAutosave();
}

void AppWindow::confirmDiscardIfDirty(std::function<void()> onConfirmed)
{
    if (m_undoStack.isClean()) {
        onConfirmed();
        return;
    }

    AdwDialog *dialog =
        adw_alert_dialog_new("Discard unsaved changes?", "This project has unsaved changes that will be lost.");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct DiscardConfirmContext
    {
        std::function<void()> onConfirmed;
    };
    auto *ctx = new DiscardConfirmContext{std::move(onConfirmed)};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<DiscardConfirmContext> owned(static_cast<DiscardConfirmContext *>(userData));
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response && std::string(response) == "discard")
                owned->onConfirmed();
        },
        ctx);
}

gboolean AppWindow::onCloseRequest()
{
    // Quitting stops a running render (prepareForShutdown), so ask first.
    if (m_renderQueue && m_renderQueue->busy() && !m_stopRenderConfirmed) {
        const std::string body = "A render is in progress (" + std::to_string(m_renderQueue->queued()) +
                                 " queued). Quit anyway? Unfinished renders are offered again next time.";
        AdwDialog *dialog = adw_alert_dialog_new("Quit while rendering?", body.c_str());
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Keep Working");
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "stop", "Quit");
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "stop", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
        adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
        adw_alert_dialog_choose(
            ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
            [](GObject *source, GAsyncResult *result, gpointer userData) {
                auto *self = static_cast<AppWindow *>(userData);
                const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
                if (response && std::string(response) == "stop") {
                    self->m_stopRenderConfirmed = true;
                    gtk_window_close(GTK_WINDOW(self->m_window)); // on to the unsaved-changes check
                }
            },
            this);
        return GDK_EVENT_STOP;
    }

    // A save still writing (Ctrl+S, then straight to close): wait for it
    // rather than ask about changes it's about to make clean. Quitting any
    // other way waits in prepareForShutdown().
    if (m_saveQueue && m_saveQueue->busy()) {
        m_closeWhenSaved = true;
        showStatus("Finishing the save before closing…");
        return GDK_EVENT_STOP;
    }

    if (m_undoStack.isClean())
        return GDK_EVENT_PROPAGATE;

    AdwDialog *dialog = adw_alert_dialog_new(
        "Save changes before closing?", "This project has unsaved changes that will be lost if you don't save them.");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "save", "Save");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            auto *self = static_cast<AppWindow *>(userData);
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (!response)
                return;
            std::string chosen = response;
            if (chosen == "discard") {
                // Bypasses close-request entirely -- calling
                // gtk_window_close() again here would just re-enter this
                // same prompt.
                gtk_window_destroy(GTK_WINDOW(self->m_window));
            } else if (chosen == "save") {
                // Enhancement #2: saves straight back to the project's own
                // path when it has one, same as Ctrl+S, rather than always
                // forcing a Save As dialog just to close.
                self->saveInPlaceOrPrompt(true);
            }
            // "cancel": nothing to do -- the close is already vetoed by
            // onCloseRequest()'s GDK_EVENT_STOP.
        },
        this);

    return GDK_EVENT_STOP;
}

bool AppWindow::offerRecoveryIfAny()
{
    // Excludes every candidate already offered (and answered) THIS
    // launch -- recovering doesn't delete the file (see
    // m_pendingAutosaveCleanupPath's own comment), so without this the
    // very next call below would just find the same one again, forever,
    // and any OTHER independently-orphaned autosave would never surface
    // at all (2026-09-23: exactly how a real, richer autosave went
    // unmentioned and unrecovered alongside newer, emptier ones).
    auto found = autosave::findRecoverable(m_offeredAutosaveMetaPaths);
    if (!found)
        return false;
    bool isFirstOfferThisLaunch = m_offeredAutosaveMetaPaths.empty();
    m_offeredAutosaveMetaPaths.insert(found->metaPath);

    auto timestamp = static_cast<std::time_t>(found->meta.timestampUnix);
    char timeBuf[64] = {};
    std::tm tmBuf{};
    localtime_r(&timestamp, &tmBuf);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tmBuf);

    std::string article = isFirstOfferThisLaunch ? "An" : "Another";
    std::string body =
        found->meta.originalPath.empty()
            ? article + " unsaved, untitled project from " + timeBuf + " was found."
            : article + " unsaved version of “" + found->meta.originalPath + "” from " + timeBuf + " was found.";
    if (!isFirstOfferThisLaunch)
        body += " Recovering it will replace what you just recovered.";

    AdwDialog *dialog = adw_alert_dialog_new("Recover unsaved work?", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "recover", "Recover");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "recover", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "recover");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "discard");

    struct RecoveryContext
    {
        AppWindow *self;
        autosave::Recoverable found;
    };
    auto *ctx = new RecoveryContext{this, *found};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<RecoveryContext> owned(static_cast<RecoveryContext *>(userData));
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);

            if (response && std::string(response) == "recover") {
                AppWindow *self = owned->self;
                autosave::Recoverable recoverable = owned->found;
                self->showStatus("Recovering unsaved work…");
                self->loadProjectAsync(
                    recoverable.autosavePath,
                    [self, recoverable](core::Model model) {
                        self->replaceProject(std::move(model));
                        self->m_currentProjectPath = recoverable.meta.originalPath;
                        // markDirty(), not setCleanPoint() (audit A2):
                        // recovered content is unsaved relative to
                        // m_currentProjectPath (or has no target at all),
                        // but clear() alone makes the empty stack clean.
                        // markDirty() keeps it dirty until a real Save.
                        self->m_undoStack.markDirty(); // emits changed -- updateWindowTitle() follows
                        self->refreshTimeline();
                        self->refreshMediaBrowser();
                        self->showStatus("Recovered unsaved work.");
                        // NOT deleted here: this session's own autosaves go
                        // to a filename keyed on its own (fresh) session id,
                        // never this recovered file's, so nothing else will
                        // ever clean it up. Kept as the only durable copy of
                        // the recovered work until a manual Save succeeds
                        // (performSaveToPath()), so a second crash before
                        // that Save doesn't lose it again.
                        self->m_pendingAutosaveCleanupPath = recoverable.autosavePath;
                        self->m_pendingAutosaveCleanupMetaPath = recoverable.metaPath;
                    },
                    [self](const core::ProjectLoadError &error) {
                        // Load failed -- leave the autosave files untouched
                        // entirely rather than destroying what may be the
                        // only copy of that work; a later launch gets
                        // another chance to recover them.
                        self->showStatus("Couldn't recover: " + error.message);
                    });
            } else {
                // An affirmative "no" from the owner (doc 09: "discarded
                // ones are deleted") -- safe to remove immediately, unlike
                // the recover-success case above.
                owned->self->showStatus("Discarded the recovered autosave.");
                std::remove(owned->found.autosavePath.c_str());
                std::remove(owned->found.metaPath.c_str());
            }

            // Check again: m_offeredAutosaveMetaPaths now excludes the one
            // just handled, so this surfaces any OTHER independently-
            // orphaned autosave instead of leaving it to be silently
            // outranked by whichever one this call happened to find
            // first. A no-op the overwhelming majority of the time (one
            // recoverable file is the normal case).
            owned->self->offerRecoveryIfAny();
        },
        ctx);
    return true;
}

// ---- GTK/GObject trampolines: static C-linkage-compatible callbacks that
// forward straight into the owning AppWindow instance. ----

gboolean AppWindow::queuedRefreshTrampoline(gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_refreshSourceId = 0;
    self->refreshTimeline();
    self->refreshMediaBrowser();
    return G_SOURCE_REMOVE;
}

void AppWindow::importClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onImportClicked();
}

void AppWindow::fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onFileOpened(sourceObject, result);
}

void AppWindow::saveClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->saveInPlaceOrPrompt(false);
}

void AppWindow::saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSaveFinished(sourceObject, result);
}

void AppWindow::openProjectClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onOpenProjectClicked();
}

void AppWindow::recentProjectClickedTrampoline(GtkButton *button, gpointer userData)
{
    const char *path = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "ustudio-recent-path"));
    if (path)
        static_cast<AppWindow *>(userData)->openRecentProject(path);
}

void AppWindow::recentManagerChangedTrampoline(GtkRecentManager *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->refreshRecentProjectsMenu();
}

void AppWindow::openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onOpenProjectFinished(sourceObject, result);
}

void AppWindow::reloadProjectClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onReloadProjectClicked();
}

void AppWindow::newProjectClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onNewProjectClicked();
}

void AppWindow::toggleMediaBrowserClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onToggleMediaBrowserClicked();
}

void AppWindow::mediaBrowserRowRightClickTrampoline(GtkGestureClick *gesture, int, double x, double y,
                                                    gpointer userData)
{
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    auto assetIdValue =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_object_get_data(G_OBJECT(row), "ustudio-asset-id")));
    static_cast<AppWindow *>(userData)->onMediaBrowserRowRightClicked(core::AssetId{assetIdValue}, row, x, y);
}

void AppWindow::removeAssetClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveAssetClicked();
}

void AppWindow::deleteAssetFileClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onDeleteAssetFileClicked();
}

gboolean AppWindow::timelineDropTrampoline(GtkDropTarget *, const GValue *value, double x, double y, gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onTimelineDrop(value, x, y);
}

gboolean AppWindow::timelineFileDropTrampoline(GtkDropTarget *, const GValue *value, double x, double y,
                                               gpointer userData)
{
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    return static_cast<AppWindow *>(userData)->onTimelineFileDrop(static_cast<GdkFileList *>(g_value_get_boxed(value)),
                                                                  x, y);
}

gboolean AppWindow::mediaBrowserFileDropTrampoline(GtkDropTarget *, const GValue *value, double, double,
                                                   gpointer userData)
{
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    return static_cast<AppWindow *>(userData)->onMediaBrowserFileDrop(
        static_cast<GdkFileList *>(g_value_get_boxed(value)));
}

void AppWindow::mediaBrowserRowActivatedTrampoline(GtkGestureClick *gesture, int nPress, double, double,
                                                   gpointer userData)
{
    if (nPress != 2)
        return;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    auto assetIdValue =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_object_get_data(G_OBJECT(row), "ustudio-asset-id")));
    static_cast<AppWindow *>(userData)->onMediaBrowserRowActivated(core::AssetId{assetIdValue});
}

void AppWindow::renderClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRenderClicked();
}

void AppWindow::renderFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRenderFinished(sourceObject, result);
}

void AppWindow::helpClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->showHelpDialog();
}

void AppWindow::settingsClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->showSettingsDialog();
}

void AppWindow::settingsAutosaveDelayChangedTrampoline(GtkSpinButton *row, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->m_settings->setAutosaveDelayMinutes(
        static_cast<int>(gtk_spin_button_get_value(row)));
}

void AppWindow::settingsRecentProjectsMaxChangedTrampoline(GtkSpinButton *row, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->m_settings->setRecentProjectsMax(
        static_cast<int>(gtk_spin_button_get_value(row)));
}

void AppWindow::settingsWorkerThreadsChangedTrampoline(GtkSpinButton *row, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->m_settings->setWorkerThreads(static_cast<int>(gtk_spin_button_get_value(row)));
}

gboolean AppWindow::settingsWorkerThreadsOutputTrampoline(GtkSpinButton *row, gpointer)
{
    if (gtk_spin_button_get_value(row) != 0.0)
        return FALSE; // the number itself
    std::string text = "Automatic (" + std::to_string(core::concurrency::ThreadPool::defaultThreadCount()) + ")";
    gtk_editable_set_text(GTK_EDITABLE(row), text.c_str());
    return TRUE;
}

gint AppWindow::settingsWorkerThreadsInputTrampoline(GtkSpinButton *row, double *newValue, gpointer)
{
    // "Automatic (N)" as shown at 0 -- anything else parses as a number.
    const char *text = gtk_editable_get_text(GTK_EDITABLE(row));
    if (text && std::string(text).starts_with("Automatic")) {
        *newValue = 0.0;
        return TRUE;
    }
    return FALSE;
}

void AppWindow::settingsCacheJobsChangedTrampoline(GtkSpinButton *row, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->m_settings->setCacheJobs(static_cast<int>(gtk_spin_button_get_value(row)));
}

gboolean AppWindow::settingsCacheJobsOutputTrampoline(GtkSpinButton *row, gpointer)
{
    if (gtk_spin_button_get_value(row) != 0.0)
        return FALSE; // the number itself
    gtk_editable_set_text(GTK_EDITABLE(row), "Automatic");
    return TRUE;
}

void AppWindow::settingsToggleChangedTrampoline(AdwSwitchRow *row, GParamSpec *, gpointer userData)
{
    const auto *key = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "ustudio-setting"));
    if (key)
        static_cast<AppWindow *>(userData)->onSettingsToggleChanged(key, adw_switch_row_get_active(row));
}

void AppWindow::settingsChooseFolderClickedTrampoline(GtkButton *button, gpointer userData)
{
    auto *row = static_cast<AdwActionRow *>(g_object_get_data(G_OBJECT(button), "ustudio-row"));
    const auto *key = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "ustudio-setting"));
    static_cast<AppWindow *>(userData)->chooseDefaultFolder(key, row);
}

void AppWindow::settingsClearFolderClickedTrampoline(GtkButton *button, gpointer userData)
{
    auto *row = static_cast<AdwActionRow *>(g_object_get_data(G_OBJECT(button), "ustudio-row"));
    const auto *key = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "ustudio-setting"));
    static_cast<AppWindow *>(userData)->setDefaultFolder(key, "", row);
}

void AppWindow::settingsShuttleMaxSpeedChangedTrampoline(GtkSpinButton *row, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->m_settings->setShuttleMaxSpeed(gtk_spin_button_get_value(row));
}

void AppWindow::settingsPreviewScaleChangedTrampoline(AdwComboRow *row, GParamSpec *, gpointer userData)
{
    static const char *kScales[] = {"auto", "full", "half", "quarter"};
    guint selected = adw_combo_row_get_selected(row);
    if (selected >= G_N_ELEMENTS(kScales))
        return;
    static_cast<AppWindow *>(userData)->m_settings->setDefaultPreviewScale(kScales[selected]);
}

void AppWindow::addTrackClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAddTrackClicked();
}

void AppWindow::undoClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onUndo();
}

void AppWindow::redoClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRedo();
}

void AppWindow::playToggledTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onPlayToggled();
}

void AppWindow::seekChangedTrampoline(GtkRange *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekChanged();
}

void AppWindow::splitClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSplitClicked();
}

gboolean AppWindow::timelineScrollTrampoline(GtkEventControllerScroll *controller, double dx, double dy,
                                             gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onTimelineScroll(controller, dx, dy);
}

void AppWindow::tracedActionTrampoline(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    auto *traced = static_cast<TracedAction *>(data);
    core::trace::Scope trace([traced] { return std::string("action: ") + traced->name; });
    traced->activated(action, parameter, traced->self);
}

void AppWindow::unparentPopoverTrampoline(GtkWidget *, gpointer popover)
{
    gtk_widget_unparent(GTK_WIDGET(popover));
}

void AppWindow::timelineMotionTrampoline(GtkEventControllerMotion *, double x, double y, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_timelinePointerX = x;
    self->onTimelineHover(x, y);
}

void AppWindow::timelineLeaveTrampoline(GtkEventControllerMotion *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    // The leave the popover's own appearance causes (see onTimelineHover()).
    if (g_get_monotonic_time() - self->m_hoverShownAt < 300000)
        return;
    self->hideHoverPreview();
}

gboolean AppWindow::hoverPreviewTimerTrampoline(gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_hoverTimerId = 0;
    self->showHoverPreview();
    return G_SOURCE_REMOVE;
}

void AppWindow::timelineHScrollChangedTrampoline(GtkAdjustment *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTimelineHScrollChanged();
}

void AppWindow::timelineClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y, gpointer userData)
{
    static_cast<AppWindow *>(userData)->hideHoverPreview(); // a press starts a click or a drag
    static_cast<AppWindow *>(userData)->onTimelineClicked(nPress, x, y,
                                                          timelineModifiers(GTK_EVENT_CONTROLLER(gesture)));
}

void AppWindow::timelineRightClickTrampoline(GtkGestureClick *, int, double x, double y, gpointer userData)
{
    static_cast<AppWindow *>(userData)->hideHoverPreview();
    static_cast<AppWindow *>(userData)->onTimelineRightClicked(x, y);
}

void AppWindow::deleteClipClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onDeleteClipClicked();
}

void AppWindow::splitAudioClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSplitAudioClicked();
}

void AppWindow::closeGapClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onCloseGapClicked();
}

void AppWindow::removeTrackClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveTrackClicked();
}

void AppWindow::toggleLockClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onToggleTrackFlag(TrackFlag::Lock);
}

void AppWindow::toggleHideClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onToggleTrackFlag(TrackFlag::Hide);
}

void AppWindow::toggleMuteClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onToggleTrackFlag(TrackFlag::Mute);
}

void AppWindow::editTrackNameClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onEditTrackNameClicked();
}

void AppWindow::editClipNameClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onEditClipNameClicked();
}

void AppWindow::removeClipNameClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveClipNameClicked();
}

void AppWindow::removeTransitionClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveTransitionClicked();
}

void AppWindow::syncClipsClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSyncClipsClicked();
}

void AppWindow::addTransitionClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAddTransitionClicked();
}

void AppWindow::inlineNameEditActivateTrampoline(GtkEntry *, gpointer userData)
{
    // Pops the popover down; its "closed" signal (inlineNameEditClosedTrampoline)
    // does the actual commit, so Enter and click-away share one code path.
    gtk_popover_popdown(static_cast<AppWindow *>(userData)->m_inlineNameEditPopover);
}

void AppWindow::inlineNameEditClosedTrampoline(GtkPopover *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onInlineNameEditClosed();
}

gboolean AppWindow::inlineNameEditKeyTrampoline(GtkEventControllerKey *, guint keyval, guint, GdkModifierType,
                                                gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onInlineNameEditKeyPressed(keyval);
}

void AppWindow::trackVolumeChangedTrampoline(GtkRange *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTrackVolumeChanged();
}

void AppWindow::trackDragBeginTrampoline(GtkGestureDrag *gesture, double x, double y, gpointer userData)
{
    if (static_cast<AppWindow *>(userData)->onTrackDragBegin(x, y, timelineModifiers(GTK_EVENT_CONTROLLER(gesture))))
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

void AppWindow::trackDragUpdateTrampoline(GtkGestureDrag *, double offsetX, double offsetY, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTrackDragUpdate(offsetX, offsetY);
}

void AppWindow::trackDragEndTrampoline(GtkGestureDrag *, double offsetX, double offsetY, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTrackDragEnd(offsetX, offsetY);
}

void AppWindow::trackDragCancelTrampoline(GtkGesture *, GdkEventSequence *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->cancelOverlayDrag();
}

void AppWindow::undoActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onUndo();
}

void AppWindow::redoActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRedo();
}

void AppWindow::playPauseActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onPlayToggled();
}

void AppWindow::saveActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->saveInPlaceOrPrompt(false);
}

void AppWindow::renderButtonRightClickTrampoline(GtkGestureClick *, int, double, double, gpointer userData)
{
    static_cast<AppWindow *>(userData)->showRenderMenu();
}

void AppWindow::renderMenuItemClickedTrampoline(GtkButton *button, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    const auto *name = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "ustudio-profile"));
    gtk_popover_popdown(self->m_renderMenu);
    // "" is Add to Render Queue: the default profile.
    const core::RenderProfile profile = name && *name
                                            ? self->m_renderProfiles->find(name).value_or(self->defaultRenderProfile())
                                            : self->defaultRenderProfile();
    self->queueRender(profile, self->autoRenderPath(profile));
}

void AppWindow::projectFrameRateClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onProjectFrameRateClicked();
}

void AppWindow::projectFrameRateActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onProjectFrameRateClicked();
}

void AppWindow::saveButtonRightClickTrampoline(GtkGestureClick *, int, double, double, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSaveClicked();
}

void AppWindow::saveAsActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSaveClicked();
}

void AppWindow::openProjectActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onOpenProjectClicked();
}

void AppWindow::quitActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    gtk_window_close(GTK_WINDOW(static_cast<AppWindow *>(userData)->m_window));
}

void AppWindow::newProjectActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onNewProjectClicked();
}

void AppWindow::importActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onImportClicked();
}

void AppWindow::importFolderActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onImportFolderClicked();
}

void AppWindow::importImageSequenceActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onImportImageSequenceClicked();
}

void AppWindow::splitAtPlayheadActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSplitClicked();
}

void AppWindow::onRippleDeleteSelected()
{
    std::vector<core::ClipId> selected;
    for (core::ClipId id : m_timelineController.selection().clips()) {
        if (m_model.hasClip(id))
            selected.push_back(id);
    }
    if (selected.empty()) {
        showStatus("No clip selected to ripple delete.");
        return;
    }
    // Latest first, so each close-up never moves a clip still to go.
    std::sort(selected.begin(), selected.end(),
              [&](core::ClipId a, core::ClipId b) { return m_model.clip(a).position > m_model.clip(b).position; });
    std::vector<std::unique_ptr<core::Command>> steps;
    for (core::ClipId id : selected)
        steps.push_back(std::make_unique<core::RippleDelete>(id));
    if (m_undoStack.execute(std::make_unique<core::CompositeCommand>("Ripple delete", std::move(steps)))) {
        m_timelineController.selection().clear();
        refreshTimeline();
        showStatus(selected.size() == 1 ? "Deleted the clip and closed the gap."
                                        : "Deleted " + std::to_string(selected.size()) + " clips and closed the gaps.");
    } else {
        showStatus("Couldn't ripple delete — a track is locked, or a dissolve crosses the gap.");
    }
}

bool AppWindow::rippleMode() const
{
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(m_window), "ripple-mode");
    if (!action)
        return false;
    GVariant *state = g_action_get_state(action);
    bool on = state && g_variant_get_boolean(state);
    if (state)
        g_variant_unref(state);
    return on;
}

void AppWindow::selectAdjacentClip(bool forward)
{
    const auto &tracks = m_model.sequence().tracks;
    if (m_activeTrack < 0 || m_activeTrack >= static_cast<int>(tracks.size()))
        return;
    const std::vector<core::ClipId> &clips = tracks[static_cast<size_t>(m_activeTrack)].clips;
    if (clips.empty()) {
        showStatus("No clips on this track.");
        return;
    }
    // From the selected clip if it's on this track, else from the playhead.
    core::ClipId current = m_timelineController.selection().single();
    core::FrameIndex from = m_engine->currentFrame();
    bool fromClip = current.isValid() && m_model.hasClip(current) &&
                    m_model.clip(current).track == tracks[static_cast<size_t>(m_activeTrack)].id;
    if (fromClip)
        from = m_model.clip(current).position;
    core::ClipId pick;
    if (forward) {
        for (core::ClipId id : clips) {
            core::FrameIndex p = m_model.clip(id).position;
            if (fromClip ? p > from : p >= from) {
                pick = id;
                break;
            }
        }
    } else {
        for (auto it = clips.rbegin(); it != clips.rend(); ++it) {
            if (m_model.clip(*it).position < from) {
                pick = *it;
                break;
            }
        }
    }
    if (!pick.isValid()) {
        showStatus(forward ? "No later clip on this track." : "No earlier clip on this track.");
        return;
    }
    m_timelineController.selection().selectOnly(pick);
    m_engine->seek(static_cast<int>(m_model.clip(pick).position));
    gtk_widget_queue_draw(m_timeline);
}

void AppWindow::nudgeSelection(core::FrameIndex frames)
{
    const std::set<core::ClipId> &selected = m_timelineController.selection().clips();
    if (selected.empty()) {
        showStatus("Select a clip to nudge.");
        return;
    }
    std::vector<core::ClipId> clips(selected.begin(), selected.end());
    if (m_undoStack.execute(std::make_unique<core::MoveClips>(std::move(clips), frames, 0))) {
        refreshTimeline();
    } else {
        showStatus("Can't nudge there — something is in the way, or it would go before the start.");
    }
}

void AppWindow::onAddMarker()
{
    int frame = m_engine->currentFrame();
    for (const core::Marker &marker : m_model.sequence().markers) {
        if (marker.at == frame) {
            showStatus("There's already a marker here.");
            return;
        }
    }
    if (m_undoStack.execute(std::make_unique<core::AddMarker>(frame, std::string{}))) {
        showStatus("Marker added at " + formatTimecode(frame) + ".");
        gtk_widget_queue_draw(GTK_WIDGET(m_rulerArea));
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
    }
}

void AppWindow::onRemoveMarker()
{
    // The marker at the playhead, or failing that the nearest one within
    // the snap distance, so it can be removed without landing exactly on it.
    int frame = m_engine->currentFrame();
    const core::Marker *best = nullptr;
    double bestPx = kEdgeGrabWidth + 1;
    for (const core::Marker &marker : m_model.sequence().markers) {
        double px = std::abs(static_cast<double>(marker.at - frame)) * m_viewport.pxPerFrame();
        if (marker.at == frame || px < bestPx) {
            best = &marker;
            bestPx = marker.at == frame ? -1 : px;
        }
    }
    if (best == nullptr) {
        showStatus("No marker at the playhead.");
        return;
    }
    if (m_undoStack.execute(std::make_unique<core::RemoveMarker>(best->id))) {
        showStatus("Marker removed.");
        gtk_widget_queue_draw(GTK_WIDGET(m_rulerArea));
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
    }
}

void AppWindow::selectNextClipActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->selectAdjacentClip(true);
}

void AppWindow::selectPreviousClipActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->selectAdjacentClip(false);
}

void AppWindow::nudgeLeftActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->nudgeSelection(-1);
}

void AppWindow::nudgeRightActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->nudgeSelection(1);
}

void AppWindow::nudgeLeft10Activated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->nudgeSelection(-10);
}

void AppWindow::nudgeRight10Activated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->nudgeSelection(10);
}

void AppWindow::rippleModeChangedTrampoline(GObject *, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->showStatus(self->rippleMode() ? "Ripple mode on: moving a clip closes its gap and pushes later clips."
                                        : "Ripple mode off.");
}

void AppWindow::timelinePinchBeginTrampoline(GtkGesture *, GdkEventSequence *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->m_pinchLastScale = 1.0;
}

void AppWindow::timelinePinchTrampoline(GtkGestureZoom *gesture, double scale, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    double anchorX = self->m_timelinePointerX, anchorY = 0.0;
    gtk_gesture_get_bounding_box_center(GTK_GESTURE(gesture), &anchorX, &anchorY);
    if (self->m_pinchLastScale > 0.0 && scale > 0.0) {
        self->m_viewport.zoomAround(std::max(anchorX, kHandleWidth), scale / self->m_pinchLastScale);
        self->onTimelineViewportChanged();
    }
    self->m_pinchLastScale = scale;
}

void AppWindow::rippleDeleteSelectedActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRippleDeleteSelected();
}

void AppWindow::addMarkerActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAddMarker();
}

void AppWindow::removeMarkerActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveMarker();
}

void AppWindow::selectAllActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_timelineController.selectAll(self->m_model);
    gtk_widget_queue_draw(GTK_WIDGET(self->m_timeline));
}

void AppWindow::clearSelectionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_timelineController.cancel();
    self->cancelOverlayDrag();
    self->m_timelineController.selection().clear();
    gtk_widget_queue_draw(GTK_WIDGET(self->m_timeline));
}

void AppWindow::zoomInActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onZoomIn();
}

void AppWindow::zoomOutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onZoomOut();
}

void AppWindow::zoomFitActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onZoomFit();
}

void AppWindow::deleteSelectedClipActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onDeleteSelectedClip();
}

void AppWindow::shuttleForwardActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onShuttleForward();
}

void AppWindow::shuttleReverseActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onShuttleReverse();
}

void AppWindow::shuttleStopActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onShuttleStop();
}

void AppWindow::stepForwardActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepForward();
}

void AppWindow::stepBackwardActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepBackward();
}

void AppWindow::seekHomeActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekHome();
}

void AppWindow::seekEndActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekEnd();
}

void AppWindow::loopSetInActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSetLoopIn();
}

void AppWindow::loopSetOutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSetLoopOut();
}

void AppWindow::seekPreviousCutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekPreviousCut(false);
}

void AppWindow::seekNextCutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekNextCut(false);
}

void AppWindow::seekPreviousCutOnActiveTrackActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekPreviousCut(true);
}

void AppWindow::seekNextCutOnActiveTrackActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekNextCut(true);
}

void AppWindow::activeTrackTopActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    if (self->m_model.sequence().tracks.empty())
        return;
    // One clip selected: move it up instead; several: ask for one.
    size_t selected = self->m_timelineController.selection().clips().size();
    if (selected == 1) {
        self->moveSelectedClipAcrossTracks(-1);
        return;
    }
    if (selected > 1) {
        self->showStatus("Select just one clip to move it to another track.");
        return;
    }
    self->m_activeTrack = 0;
    gtk_widget_queue_draw(GTK_WIDGET(self->m_timeline));
}

void AppWindow::activeTrackBottomActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    int trackCount = static_cast<int>(self->m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;
    size_t selected = self->m_timelineController.selection().clips().size();
    if (selected == 1) {
        self->moveSelectedClipAcrossTracks(+1);
        return;
    }
    if (selected > 1) {
        self->showStatus("Select just one clip to move it to another track.");
        return;
    }
    self->m_activeTrack = trackCount - 1;
    gtk_widget_queue_draw(GTK_WIDGET(self->m_timeline));
}

void AppWindow::activeTrackUpActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onActiveTrackUp();
}

void AppWindow::activeTrackDownActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onActiveTrackDown();
}

void AppWindow::stepForward10Activated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepForward10();
}

void AppWindow::stepBackward10Activated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepBackward10();
}

void AppWindow::stepForwardMinuteActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepForwardMinute();
}

void AppWindow::stepBackwardMinuteActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepBackwardMinute();
}

void AppWindow::clearLoopClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onClearLoopClicked();
}

void AppWindow::volumeChangedTrampoline(GtkRange *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onVolumeChanged();
}

void AppWindow::previewScaleChangedTrampoline(GtkDropDown *, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onPreviewScaleChanged();
}

gboolean AppWindow::autosaveHeartbeatTrampoline(gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAutosaveHeartbeat();
    return G_SOURCE_CONTINUE;
}

void AppWindow::windowActiveChangedTrampoline(GObject *, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onWindowActiveChanged();
}

void AppWindow::focusWidgetChangedTrampoline(GObject *, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onFocusWidgetChanged();
}

void AppWindow::textFocusReleaseTrampoline(GtkGestureClick *, int, double, double, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    if (self->m_textHasFocus)
        gtk_window_set_focus(GTK_WINDOW(self->m_window), nullptr);
}

gboolean AppWindow::closeRequestTrampoline(GtkWindow *, gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onCloseRequest();
}

} // namespace ustudio::app
