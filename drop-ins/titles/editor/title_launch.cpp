#include "title_launch.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "platform/process.h"

#include <gio/gio.h>

#include <atomic>
#include <filesystem>

namespace ustudio::titles {

namespace {
bool isFile(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

// The editor's frame as a PNG in the user cache directory, one file per
// launch (two titles opened at once each keep theirs).
std::string saveBackdrop(GdkTexture *texture)
{
    static std::atomic<unsigned> counter{0};
    char *dir = g_build_filename(g_get_user_cache_dir(), "ustudio", nullptr);
    g_mkdir_with_parents(dir, 0700);
    const std::string name = "titles-backdrop-" + std::to_string(platform::currentProcessId()) + "-" +
                             std::to_string(counter.fetch_add(1)) + ".png";
    char *path = g_build_filename(dir, name.c_str(), nullptr);
    const std::string out = gdk_texture_save_to_png(texture, path) ? path : "";
    g_free(path);
    g_free(dir);
    return out;
}
} // namespace

std::string titlesAppPath()
{
    const std::string name = std::string("u-studio-titles") + platform::executableSuffix();
    const std::filesystem::path self = platform::executablePath();
    if (!self.empty() && isFile(self.parent_path() / name))
        return core::utf8String(self.parent_path() / name);
    // Installed with this drop-in, when it's packaged apart from the editor
    // (a Flatpak extension has its own prefix, doc 17).
    const std::filesystem::path installed = core::pathFromUtf8(TITLES_APP_INSTALL_DIR) / name;
    if (isFile(installed))
        return core::utf8String(installed);
    if (isFile(core::pathFromUtf8(TITLES_APP_BUILD_PATH)))
        return TITLES_APP_BUILD_PATH;
    char *found = g_find_program_in_path(name.c_str());
    const std::string out = found ? found : "";
    g_free(found);
    return out;
}

namespace {
TitlesLauncher &testLauncher()
{
    static TitlesLauncher launcher;
    return launcher;
}
} // namespace

void setTitlesLauncherForTesting(TitlesLauncher launcher)
{
    testLauncher() = std::move(launcher);
}

bool titlesLauncherIsForTesting()
{
    return static_cast<bool>(testLauncher());
}

std::string launchTitlesApp(const std::string &title, GdkTexture *backdrop, bool gallery)
{
    if (testLauncher())
        return testLauncher()(title, backdrop, gallery);
    const std::string app = titlesAppPath();
    if (app.empty())
        return "U-Stu Titles isn't installed";
    std::vector<std::string> args = {app};
    if (!title.empty())
        args.push_back(title);
    if (gallery)
        args.push_back("--gallery");
    if (backdrop) {
        const std::string png = saveBackdrop(backdrop);
        if (!png.empty()) {
            args.push_back("--backdrop");
            args.push_back(png);
        }
    }
    std::vector<const char *> argv;
    for (const std::string &arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    GError *error = nullptr;
    // Unreffed straight away: GLib reaps the child when it exits.
    GSubprocess *process = g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_NONE, &error);
    if (!process) {
        const std::string message = error ? error->message : "it didn't start";
        g_clear_error(&error);
        return message;
    }
    g_object_unref(process);
    core::Log::info("[titles] opened " + (title.empty() ? std::string("a new title") : title) + " in U-Stu Titles");
    return {};
}

} // namespace ustudio::titles
