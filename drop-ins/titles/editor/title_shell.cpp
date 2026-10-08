#include "title_shell.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"
#include "core/captions.h"
#include "core/template_library.h"
#include "core/title_xml.h"
#include "engine/backdrop.h"
#include "jobs.h"
#include "package/archive.h"
#include "title_bake.h"
#include "title_launch.h"
#include "title_page.h"
#include "titles-editor-resources.h"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace ustudio::titles {

namespace {

// How long after the last change event a title is reloaded: an atomic save
// is several events (write, rename), and editors save more than once.
constexpr guint kSettleMs = 150;

// One GFileMonitor per title asset in the project, kept in step with the
// bin on every project change. Main thread only; lives for the process,
// like the window.
class TitleWatcher
{
  public:
    // The window it serves (the editor has one per process; a new host
    // replaces the old, and its watches go).
    void bind(app::ShellHost &host)
    {
        for (auto &[id, watch] : m_watches)
            stop(watch);
        m_watches.clear();
        m_pending.clear();
        if (m_timer)
            g_source_remove(m_timer);
        m_timer = 0;
        m_host = &host;
    }

    void sync()
    {
        if (!m_host)
            return;
        std::map<uint64_t, std::string> wanted;
        for (const core::Asset &asset : m_host->model().project().bin)
            if (isTitleFile(asset.path))
                wanted.emplace(asset.id.value, asset.path);
        for (auto it = m_watches.begin(); it != m_watches.end();) {
            auto want = wanted.find(it->first);
            if (want == wanted.end() || want->second != it->second.path) {
                stop(it->second);
                it = m_watches.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto &[id, path] : wanted) {
            if (m_watches.contains(id))
                continue;
            GFile *file = g_file_new_for_path(path.c_str());
            GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, nullptr, nullptr);
            g_object_unref(file);
            if (!monitor)
                continue;
            Watch &watch = m_watches[id];
            watch.path = path;
            watch.monitor = monitor;
            watch.handler = g_signal_connect(monitor, "changed", G_CALLBACK(onChanged), this);
            // Remembered by the monitor, so the callback knows which asset.
            g_object_set_data(G_OBJECT(monitor), "ustudio-asset", GSIZE_TO_POINTER(id));
        }
    }

  private:
    struct Watch
    {
        std::string path;
        GFileMonitor *monitor = nullptr;
        gulong handler = 0;
    };

    static void stop(Watch &watch)
    {
        g_signal_handler_disconnect(watch.monitor, watch.handler);
        g_file_monitor_cancel(watch.monitor);
        g_object_unref(watch.monitor);
    }

    void changed(uint64_t asset)
    {
        m_pending.insert(asset);
        if (m_timer)
            g_source_remove(m_timer);
        m_timer = g_timeout_add(kSettleMs, &onSettled, this);
    }

    void settled()
    {
        m_timer = 0;
        const std::set<uint64_t> pending = std::move(m_pending);
        m_pending.clear();
        for (uint64_t id : pending)
            if (m_host)
                m_host->assetChangedOnDisk(core::AssetId{id});
    }

    app::ShellHost *m_host = nullptr;
    std::map<uint64_t, Watch> m_watches;
    std::set<uint64_t> m_pending;
    guint m_timer = 0;

    // --- GLib trampolines --------------------------------------------------
    static void onChanged(GFileMonitor *monitor, GFile *, GFile *, GFileMonitorEvent event, gpointer self)
    {
        if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED || event == G_FILE_MONITOR_EVENT_PRE_UNMOUNT ||
            event == G_FILE_MONITOR_EVENT_UNMOUNTED)
            return;
        const auto asset =
            static_cast<uint64_t>(GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(monitor), "ustudio-asset")));
        static_cast<TitleWatcher *>(self)->changed(asset);
    }
    static gboolean onSettled(gpointer self)
    {
        static_cast<TitleWatcher *>(self)->settled();
        return G_SOURCE_REMOVE;
    }
};

void launch(app::ShellHost &host, const std::string &path, const std::string &name, GdkTexture *backdrop, bool gallery)
{
    const std::string error = launchTitlesApp(path, backdrop, gallery);
    host.showStatus(error.empty() ? (gallery ? "Pick a template for " + name + " in U-Stu Titles, then save it."
                                             : "Editing " + name + " in U-Stu Titles: save it there to update it here.")
                                  : "Couldn't open U-Stu Titles: " + error);
}

// Opens a title clip in U-Stu Titles, over the editor's frame at the
// playhead (within the clip) without the title itself; with its template
// gallery when `gallery` (New Title).
void editTitleClip(app::ShellHost &host, core::ClipId id, bool gallery = false)
{
    const core::Model &model = host.model();
    if (!model.hasClip(id))
        return;
    const core::Clip &clip = model.clip(id);
    if (!model.hasAsset(clip.asset) || !isTitleFile(model.asset(clip.asset).path))
        return;
    const core::Asset &asset = model.asset(clip.asset);
    const std::string path = asset.path, name = asset.displayName;
    if (titlesLauncherIsForTesting()) {
        launch(host, path, name, nullptr, gallery);
        return;
    }
    const core::FrameIndex frame = std::clamp(host.currentFrame(), clip.position, clip.end() - 1);
    std::shared_ptr<const core::Project> project = model.snapshot();
    host.showStatus("Opening " + name + " in U-Stu Titles…");
    app::ShellHost *hostPtr = &host; // the window lives for the process
    startJob([project, id, frame, path, name, hostPtr, gallery] {
        auto backdrop = std::make_shared<Backdrop>(renderBackdrop(project, id, frame));
        postToMain([backdrop, path, name, hostPtr, gallery] {
            GdkTexture *texture = nullptr;
            if (!backdrop->rgba.empty()) {
                GBytes *bytes = g_bytes_new(backdrop->rgba.data(), backdrop->rgba.size());
                texture = gdk_memory_texture_new(backdrop->width, backdrop->height, GDK_MEMORY_R8G8B8A8, bytes,
                                                 static_cast<gsize>(backdrop->width) * 4);
                g_bytes_unref(bytes);
            }
            launch(*hostPtr, path, name, texture, gallery);
            if (texture)
                g_object_unref(texture);
        });
    });
}

// Edit Title (the action): the first selected title clip.
void onEditTitle(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    const core::Model &model = host.model();
    for (core::ClipId id : host.currentSelection().clips) {
        if (!model.hasClip(id))
            continue;
        const core::Clip &clip = model.clip(id);
        if (model.hasAsset(clip.asset) && isTitleFile(model.asset(clip.asset).path)) {
            editTitleClip(host, id);
            return;
        }
    }
    host.showStatus("Select a title clip to edit it.");
}

// New Title (the action, Shift+T).
void onNewTitle(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    auto made = newTitle(host);
    if (!made) {
        host.showStatus("Couldn't make a new title: " + made.error());
        return;
    }
    editTitleClip(host, *made, true);
}

// The header's "T" (Open U-Stu Titles): a selected title clip's title,
// else the designer's gallery on a new, untitled title. The timeline
// doesn't change; New Title (Shift+T) is the way to add one.
void onOpenTitles(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    const core::Model &model = host.model();
    for (core::ClipId id : host.currentSelection().clips) {
        if (!model.hasClip(id))
            continue;
        const core::Clip &clip = model.clip(id);
        if (model.hasAsset(clip.asset) && isTitleFile(model.asset(clip.asset).path)) {
            editTitleClip(host, id);
            return;
        }
    }
    const std::string error = launchTitlesApp({}, nullptr, true);
    host.showStatus(error.empty() ? "U-Stu Titles is open with its templates."
                                  : "Couldn't open U-Stu Titles: " + error);
}

// app.install-template-pack(s): a pack handed over by u-studio-share (ADR-020)
// installs into My Templates. Any program of the user's session may call
// it: the pack is validated in full before anything is written, like one
// opened by hand.
void onInstallTemplatePack(GSimpleAction *, GVariant *parameter, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    const std::string path = g_variant_get_string(parameter, nullptr);
    auto installed = pack::openPackage(path, pack::templatesLibrary(), pack::Replace::IfNewer);
    host.showStatus(installed
                        ? "Installed the template pack “" + installed->manifest.title + "”: New Title's gallery has it."
                        : "Couldn't install the template pack: " + installed.error());
}

// Bake Title (the action): the first selected title clip.
void onBakeTitle(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    const core::Model &model = host.model();
    for (core::ClipId id : host.currentSelection().clips) {
        if (!model.hasClip(id))
            continue;
        const core::Clip &clip = model.clip(id);
        if (model.hasAsset(clip.asset) && isTitleFile(model.asset(clip.asset).path)) {
            bakeTitleClip(host, id);
            return;
        }
    }
    host.showStatus("Select a title clip to bake it.");
}

void onExportCaptionsChosen(GObject *source, GAsyncResult *result, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
    if (!file)
        return; // cancelled
    char *chosen = g_file_get_path(file);
    g_object_unref(file);
    if (!chosen) {
        host.showStatus("Couldn't export the captions: pick a local file.");
        return;
    }
    std::string path = chosen;
    g_free(chosen);
    // No extension: SRT, and the name says so.
    if (core::pathFromUtf8(path).extension().empty())
        path += ".srt";
    const auto cues = captions::captionCues(host.model());
    if (cues.empty()) {
        host.showStatus("No captions to export.");
        return;
    }
    const std::string error = captions::saveSubtitles(cues, host.model().sequence().profile.fps, path);
    const std::string name = core::utf8String(core::pathFromUtf8(path).filename());
    host.showStatus(error.empty() ? "Exported " + std::to_string(cues.size()) +
                                        (cues.size() == 1 ? " caption" : " captions") + " to " + name + "."
                                  : "Couldn't export the captions: " + error);
}

// Export Captions… (T5.1): the project's captions to a .srt or .vtt, the
// format by the name chosen. Read from the model when the file is picked, so
// an edit made while the dialog is open is in the file.
void onExportCaptions(GSimpleAction *, GVariant *, gpointer target)
{
    auto &host = *static_cast<app::ShellHost *>(target);
    if (captions::captionCues(host.model()).empty()) {
        host.showStatus("No captions to export: import a .srt or .vtt file first.");
        return;
    }
    GtkFileDialog *chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, "Export Captions");
    gtk_file_dialog_set_initial_name(chooser, "Captions.srt");
    if (const std::string folder = host.projectFolder(); !folder.empty()) {
        GFile *initial = g_file_new_for_path(folder.c_str());
        gtk_file_dialog_set_initial_folder(chooser, initial);
        g_object_unref(initial);
    }
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *subtitles = gtk_file_filter_new();
    gtk_file_filter_set_name(subtitles, "Subtitles (.srt, .vtt)");
    gtk_file_filter_add_suffix(subtitles, "srt");
    gtk_file_filter_add_suffix(subtitles, "vtt");
    g_list_store_append(filters, subtitles);
    g_object_unref(subtitles);
    gtk_file_dialog_set_filters(chooser, G_LIST_MODEL(filters));
    g_object_unref(filters);
    GtkApplication *application = GTK_APPLICATION(g_application_get_default());
    gtk_file_dialog_save(chooser, application ? gtk_application_get_active_window(application) : nullptr, nullptr,
                         &onExportCaptionsChosen, &host);
    g_object_unref(chooser);
}

// A double-click on a title clip opens it (anywhere but the track's name
// strip, which still renames the track). Paints nothing.
class TitleClipClicks : public app::timeline::TimelineOverlayProvider
{
  public:
    void bind(app::ShellHost &host)
    {
        m_host = &host;
    }

    void paintOverlay(GtkSnapshot *, const core::Model &, const app::timeline::Viewport &,
                      const app::timeline::RowLayout &, double, double) const override
    {}

    bool pressed(const core::Model &model, const app::timeline::Viewport &viewport,
                 const app::timeline::RowLayout &layout, double x, double y, int nPress) override
    {
        if (nPress < 2 || layout.inNameStrip(y))
            return false;
        const int row = layout.rowAt(y);
        const auto &tracks = model.sequence().tracks;
        if (row < 0 || static_cast<size_t>(row) >= tracks.size())
            return false;
        for (core::ClipId id : model.track(tracks[static_cast<size_t>(row)].id).clips) {
            const core::Clip &clip = model.clip(id);
            // Against the clip's edges on screen (xForFrame is the timeline's
            // own mapping).
            if (x < viewport.xForFrame(static_cast<double>(clip.position)) ||
                x >= viewport.xForFrame(static_cast<double>(clip.end())))
                continue;
            if (!model.hasAsset(clip.asset) || !isTitleFile(model.asset(clip.asset).path))
                return false;
            if (m_host)
                editTitleClip(*m_host, id);
            return true;
        }
        return false;
    }

  private:
    app::ShellHost *m_host = nullptr;
};

} // namespace

core::Asset titleAsset(const std::string &path, const TitleDocument &doc)
{
    core::Asset asset;
    asset.path = path;
    asset.displayName = core::utf8String(core::pathFromUtf8(path).filename());
    asset.fileFingerprint = core::fileFingerprint(path);
    asset.info.hasVideo = true;
    asset.info.width = doc.width;
    asset.info.height = doc.height;
    asset.info.fps = {doc.fpsNum, doc.fpsDen};
    asset.info.sar = {1, 1};
    // Boundless, like a still (doc 16): the elastic hold makes any length
    // valid. The still flag is what keeps it boundless once clips are cut
    // from it (InsertClip grows a boundless asset's length otherwise), and
    // keeps titles out of proxies and profile matching.
    asset.info.isStillImage = true;
    asset.info.container = "ustitle";
    return asset;
}

std::string builtInTemplatesDir()
{
    std::error_code ec;
    if (std::filesystem::is_directory(core::pathFromUtf8(TITLES_TEMPLATES_INSTALL_DIR), ec))
        return TITLES_TEMPLATES_INSTALL_DIR;
    return TITLES_TEMPLATES_SOURCE_DIR;
}

std::expected<std::optional<core::FrameIndex>, std::string>
importCaptions(app::ShellHost &host, const std::string &path, const std::string &templateId)
{
    namespace fs = std::filesystem;
    const fs::path file = core::pathFromUtf8(path);
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec)
        return std::unexpected("can't read " + path + " (" + ec.message() + ")");
    if (size > captions::kMaxBytes)
        return std::unexpected("the file is over " + std::to_string(captions::kMaxBytes >> 20) + " MB");
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = captions::parse(bytes);
    if (!parsed)
        return std::unexpected(parsed.error());

    // The caption title: a copy of the caption template, beside the project;
    // and one from the top template when a cue sits at the top (T5.2).
    const auto templates = listTemplates(builtInTemplatesDir(), true);
    const fs::path folder = core::pathFromUtf8(newTitlePath(host.projectFolder())).parent_path();
    const std::string stem = core::utf8String(file.stem());
    const auto makeTitle =
        [&](const std::string &id,
            const std::string &suffix) -> std::expected<std::pair<fs::path, core::Asset>, std::string> {
        auto chosen =
            std::find_if(templates.begin(), templates.end(), [&](const TemplateInfo &t) { return t.id == id; });
        if (chosen == templates.end())
            return std::unexpected("the caption template “" + id + "” isn't installed");
        fs::path titlePath = folder / core::pathFromUtf8(stem + " captions" + suffix + ".ustitle");
        for (int n = 2; fs::exists(titlePath, ec); ++n)
            titlePath = folder / core::pathFromUtf8(stem + " captions" + suffix + " " + std::to_string(n) + ".ustitle");
        fs::create_directories(folder, ec);
        auto made = newTitleFromTemplate(*chosen, core::utf8String(titlePath));
        if (!made)
            return std::unexpected(made.error());
        auto doc = readTitle(core::utf8String(titlePath));
        if (!doc)
            return std::unexpected(doc.error());
        return std::pair{titlePath, titleAsset(core::utf8String(titlePath), doc->document)};
    };
    auto title = makeTitle(templateId, "");
    if (!title)
        return std::unexpected(title.error());
    const fs::path titlePath = title->first;
    std::optional<core::Asset> topAsset;
    if (std::any_of(parsed->cues.begin(), parsed->cues.end(), [](const captions::Cue &c) { return c.top; })) {
        auto top = makeTitle("caption-top", " (top)");
        if (!top)
            return std::unexpected(top.error());
        topAsset = top->second;
    }

    const auto placed = captions::place(parsed->cues, host.model().sequence().profile.fps);
    const std::string name = core::utf8String(file.filename());
    if (!host.execute(std::make_unique<captions::ImportCaptions>(title->second, placed, name, topAsset)))
        return std::unexpected("couldn't add the captions to the timeline");
    std::string status = "Imported " + std::to_string(parsed->cues.size()) + " captions from " + name +
                         "; restyle them all by editing " + core::utf8String(titlePath.filename()) + ".";
    if (parsed->skipped)
        status += " Skipped " + std::to_string(parsed->skipped) + " unreadable " +
                  (parsed->skipped == 1 ? "cue" : "cues") + " (the first at line " +
                  std::to_string(parsed->firstSkippedLine) + ": " + parsed->firstSkippedWhy + ").";
    for (const std::string &warning : parsed->warnings)
        status += " The file was " + warning + ".";
    host.showStatus(status);
    return std::nullopt;
}

std::expected<std::optional<core::FrameIndex>, std::string> importTitle(app::ShellHost &host, const std::string &path,
                                                                        std::optional<core::TrackId> track,
                                                                        std::optional<core::FrameIndex> position)
{
    auto read = readTitle(path);
    if (!read)
        return std::unexpected(read.error());
    const TitleDocument &doc = read->document;
    const core::Model &model = host.model();
    const core::Rational fps = model.sequence().profile.fps;
    const core::Asset asset = titleAsset(path, doc);

    // The designed length, in sequence frames.
    const double seconds = static_cast<double>(doc.timing.length()) * doc.fpsDen / doc.fpsNum;
    const auto length =
        std::max<core::FrameIndex>(1, static_cast<core::FrameIndex>(std::lround(seconds * fps.num / fps.den)));

    std::vector<std::unique_ptr<core::Command>> steps;
    const core::AssetId assetId{model.project().nextId}; // AddAsset takes the next id, as the media import does
    steps.push_back(std::make_unique<core::AddAsset>(asset));
    core::FrameIndex at = 0;
    if (track) {
        at = position.value_or(0);
        if (!position)
            for (core::ClipId id : model.track(*track).clips)
                at = std::max(at, model.clip(id).end());
        steps.push_back(std::make_unique<core::InsertClip>(*track, assetId, at, 0, length - 1));
    }
    if (!host.execute(std::make_unique<core::CompositeCommand>("Import " + asset.displayName, std::move(steps))))
        return std::unexpected("Couldn't import " + asset.displayName + " there");
    for (const std::string &warning : read->warnings)
        host.showStatus(asset.displayName + ": " + warning);
    return track ? std::optional<core::FrameIndex>(at + length) : std::nullopt;
}

std::string newTitlePath(const std::string &projectFolder)
{
    namespace fs = std::filesystem;
    fs::path folder;
    if (!projectFolder.empty()) {
        folder = core::pathFromUtf8(projectFolder) / "Titles";
    } else {
        // No project folder: the Videos folder, when there is one (GLib
        // gives none, or the home folder itself, when it isn't set up),
        // else a folder in the user's data dir. Never files loose in $HOME.
        std::error_code ec;
        const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        const fs::path home = core::pathFromUtf8(g_get_home_dir());
        if (videos && core::pathFromUtf8(videos) != home && fs::is_directory(core::pathFromUtf8(videos), ec))
            folder = core::pathFromUtf8(videos) / "U Stu Titles";
        else
            folder = core::pathFromUtf8(g_get_user_data_dir()) / "ustudio" / "U Stu Titles";
        // A folder name on disk: it keeps the name it had before the display
        // name became "U-Stu" (2026-10-08), so titles saved there stay found.
    }
    std::error_code ec;
    for (int n = 1;; ++n) {
        const fs::path candidate = folder / core::pathFromUtf8("Title " + std::to_string(n) + ".ustitle");
        if (!fs::exists(candidate, ec))
            return core::utf8String(candidate);
    }
}

std::expected<core::ClipId, std::string> newTitle(app::ShellHost &host)
{
    const core::Model &model = host.model();
    // The active track when it's a video one, else the first video track.
    std::optional<core::TrackId> track = host.currentSelection().track;
    if (track && (!model.hasTrack(*track) || model.track(*track).kind != core::Track::Kind::Video))
        track.reset();
    if (!track)
        for (const core::Track &candidate : model.sequence().tracks)
            if (candidate.kind == core::Track::Kind::Video) {
                track = candidate.id;
                break;
            }
    if (!track)
        return std::unexpected("the project has no video track");

    // A blank title the size and rate of the sequence, five seconds long:
    // the template picked in the designer fills it.
    const core::Profile &profile = model.sequence().profile;
    TitleDocument doc;
    doc.width = profile.width;
    doc.height = profile.height;
    doc.fpsNum = static_cast<int>(profile.fps.num);
    doc.fpsDen = static_cast<int>(profile.fps.den);
    const auto fiveSeconds = std::max<int64_t>(
        3, std::llround(5.0 * static_cast<double>(profile.fps.num) / static_cast<double>(profile.fps.den)));
    doc.timing = {0, fiveSeconds, 0};
    const std::string path = newTitlePath(host.projectFolder());
    std::error_code ec;
    std::filesystem::create_directories(core::pathFromUtf8(path).parent_path(), ec);
    if (ec)
        return std::unexpected("can't make " + core::utf8String(core::pathFromUtf8(path).parent_path()) + " (" +
                               ec.message() + ")");
    if (const std::string error = saveTitle(doc, path); !error.empty())
        return std::unexpected(error);
    const core::FrameIndex at = host.currentFrame();
    auto imported = importTitle(host, path, track, at);
    if (!imported)
        return std::unexpected(imported.error());
    for (core::ClipId id : host.model().track(*track).clips)
        if (host.model().clip(id).position == at && host.model().hasAsset(host.model().clip(id).asset) &&
            host.model().asset(host.model().clip(id).asset).path == path)
            return id;
    return std::unexpected("the new title isn't on the track");
}

void extendShell(app::ShellHost &host)
{
    host.addImportHandler({{"ustitle"}, "Titles", [&host](const std::string &path, auto track, auto position) {
                               return importTitle(host, path, track, position);
                           }});
    // Subtitle files (T5): captions as title clips, on tracks of their own.
    host.addImportHandler({{"srt", "vtt"}, "Subtitles", [&host](const std::string &path, auto, auto) {
                               return importCaptions(host, path);
                           }});
    host.addActions({{"titles-new", "New Title", "Titles", {"<Shift>t"}, &onNewTitle},
                     {"titles-edit", "Edit Title", "Titles", {"<Control><Shift>t"}, &onEditTitle},
                     {"titles-bake", "Bake Title", "Titles", {}, &onBakeTitle},
                     {"titles-open", "Open U-Stu Titles", "Titles", {}, &onOpenTitles},
                     {"titles-export-captions", "Export Captions…", "Titles", {}, &onExportCaptions}},
                    &host);
    host.addHints(
        {{"titles.open", "Header bar", "Open U-Stu Titles, the title designer",
          "With a title clip selected, it opens that title; otherwise the template gallery", "titles-open", nullptr},
         {"titles.new", "Titles", "New title",
          "A new title at the playhead on the active track, opened in U-Stu Titles to pick a template", "titles-new",
          nullptr},
         {"titles.edit", "Titles", "Edit title", "Open the selected title clip in U-Stu Titles", "titles-edit",
          "Double-click a title clip"},
         {"titles.bake", "Titles", "Bake title",
          "Render the clip to a video file with transparency, for tools without U-Stu's titles; undo "
          "brings the live title back",
          "titles-bake", nullptr},
         {"titles.export-captions", "Titles", "Export captions",
          "Write the project's captions to a subtitle file: .srt, or .vtt when the name ends in .vtt",
          "titles-export-captions", nullptr}});
    // For the process, as the host requires; bound to this window.
    static TitleClipClicks clicks;
    clicks.bind(host);
    host.addTimelineOverlay(&clicks);
    static TitleWatcher watcher;
    watcher.bind(host);
    watcher.sync();
    host.projectChanged().connect([] { watcher.sync(); });
    addTitlePage(host);

    // What u-studio-share calls to hand over a downloaded pack.
    if (GApplication *application = g_application_get_default();
        application && !g_action_map_lookup_action(G_ACTION_MAP(application), "install-template-pack")) {
        GSimpleAction *install = g_simple_action_new("install-template-pack", G_VARIANT_TYPE_STRING);
        g_signal_connect(install, "activate", G_CALLBACK(onInstallTemplatePack), &host);
        g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(install));
        g_object_unref(install);
    }

    // The header's "T", between Render… and Settings: only with this
    // drop-in, so without it there's no button and no gap. Its icon is in
    // the drop-in's own resources (editor/titles-editor.gresource.xml).
    // Referencing the generated getter links the resource in (a static
    // library's unreferenced objects are dropped), and with it the
    // constructor that registers it.
    g_resource_unref(g_resource_ref(ustudio_titles_editor_get_resource()));
    if (GdkDisplay *display = gdk_display_get_default())
        gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(display), "/com/ustudio/Titles/editor/icons");
    GtkWidget *button = gtk_button_new_from_icon_name("ustudio-titles-symbolic");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.titles-open");
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Open U-Stu Titles", -1);
    host.setTooltip(button, "titles.open");
    host.addHeaderButton(button);
}

} // namespace ustudio::titles
