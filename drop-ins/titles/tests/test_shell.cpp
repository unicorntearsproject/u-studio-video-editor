// The titles drop-in in the editor's shell (IP5): importing a .ustitle,
// and the file watch that reloads a title within a second of a save
// (doc 16, T1 acceptance). A fake ShellHost stands in for the window.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "editor/title_launch.h"
#include "core/clip_fields.h"
#include "editor/title_bake.h"
#include "editor/title_shell.h"
#include "engine/factory_policy.h"
#include "engine/title_extension.h"
#include "platform/process.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace ustudio;
namespace fs = std::filesystem;

namespace {

class FakeShell : public app::ShellHost
{
  public:
    explicit FakeShell(core::Profile profile) : m_model(core::Model::createEmpty(profile))
    {
        video = m_model.addTrack(core::Track::Kind::Video, 0, "V1");
    }
    const core::Model &model() const override
    {
        return m_model;
    }
    bool execute(std::unique_ptr<core::Command> command) override
    {
        if (!m_undo.execute(std::move(command)))
            return false;
        m_projectChanged.emit();
        return true;
    }
    core::Signal<> &projectChanged() override
    {
        return m_projectChanged;
    }
    core::FrameIndex currentFrame() const override
    {
        return 0;
    }
    void seek(core::FrameIndex) override {}
    void showInspectorPage(const char *) override {}
    core::Signal<> &playheadMoved() override
    {
        return m_playheadMoved;
    }
    void showStatus(const std::string &text) override
    {
        status = text;
    }
    app::ShellSelection currentSelection() const override
    {
        return selection;
    }
    core::Signal<> &selectionChanged() override
    {
        return m_selectionChanged;
    }
    void addInspectorPage(const app::InspectorPage &page) override
    {
        pages.push_back(page.widget);
    }
    void addHints(const std::vector<app::HintSpec> &) override {}
    void setTooltip(GtkWidget *, const char *) override {}
    void addPreviewOverlay(GtkWidget *) override {}
    app::PreviewMapping previewMapping() const override
    {
        return {};
    }
    void redrawPreviewOverlays() override {}
    void addTimelineOverlay(app::timeline::TimelineOverlayProvider *provider) override
    {
        overlays.push_back(provider);
    }
    void addActions(const std::vector<app::ActionSpec> &specs, gpointer target) override
    {
        for (const app::ActionSpec &spec : specs)
            actions.push_back({spec, target});
    }
    void redrawTimeline() override {}
    void addImportHandler(app::ImportHandler handler) override
    {
        importHandlers.push_back(std::move(handler));
    }
    void assetChangedOnDisk(core::AssetId asset) override
    {
        changedOnDisk.push_back(asset);
    }
    void addHeaderButton(GtkWidget *button) override
    {
        headerButtons.push_back(button);
    }
    std::vector<GtkWidget *> headerButtons;
    std::string projectFolder() const override
    {
        return folder;
    }
    std::string folder;

    core::TrackId video;
    std::string status;
    std::vector<app::timeline::TimelineOverlayProvider *> overlays;
    std::vector<std::pair<app::ActionSpec, gpointer>> actions;
    app::ShellSelection selection;
    std::vector<app::ImportHandler> importHandlers;
    std::vector<core::AssetId> changedOnDisk;
    std::vector<GtkWidget *> pages;
    void select(std::vector<core::ClipId> clips)
    {
        selection.clips = std::move(clips);
        m_selectionChanged.emit();
    }
    // As the window does: an undo is a project change.
    void undo()
    {
        m_undo.undo();
        m_projectChanged.emit();
    }

  private:
    core::Model m_model;
    core::UndoStack m_undo{m_model};
    core::Signal<> m_projectChanged, m_selectionChanged, m_playheadMoved;
};

// extendShell() builds the Title page's widgets, which need a display.
bool haveGtk()
{
    if (gtk_init_check())
        return true;
    MESSAGE("no display: skipped");
    return false;
}

// MLT with the titles module, once, for the tests that bake or export.
void setUpMlt()
{
    static const bool done = [] {
        engine::FactoryPaths paths;
        paths.mltModuleDirs.push_back(TITLES_MLT_BUILD_DIR);
        static engine::FactoryPolicy policy(paths);
        engine::registerEngineExtension([] { return titles::makeTitleExtension(); });
        return true;
    }();
    (void)done;
}

// Every GtkEntry under `widget`, in order.
void entriesIn(GtkWidget *widget, std::vector<GtkWidget *> &out)
{
    if (GTK_IS_ENTRY(widget))
        out.push_back(widget);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        entriesIn(child, out);
}

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-titles-shell-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

constexpr const char *kTitle = R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15"/>
  <layer kind="shape" x="0" y="0" w="10" h="10"><fill color="#ffffff"/></layer>
  <frobnicate/>
</ustitle>)";

std::string saveTitle(const std::string &name, const char *colour = "#ffffff")
{
    // Written as is (the reader's round trip would drop <frobnicate>), and
    // renamed into place as the titles app's saveTitle() does.
    std::string xml = kTitle;
    xml.replace(xml.find("#ffffff"), 7, colour);
    const fs::path path = scratch() / name;
    const fs::path part = scratch() / (name + ".part");
    std::FILE *file = std::fopen(part.string().c_str(), "wb");
    REQUIRE(file);
    std::fputs(xml.c_str(), file);
    std::fclose(file);
    fs::rename(part, path);
    return core::utf8String(path);
}

// Runs the main loop until `done` or `limit` passes; how long it took.
template <typename Done> std::chrono::milliseconds spinUntil(Done done, std::chrono::milliseconds limit)
{
    const auto start = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - start < limit)
        if (!g_main_context_iteration(nullptr, FALSE))
            g_usleep(2000);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

} // namespace

TEST_CASE("importing a title: an asset, and a clip of its designed length at the sequence's rate")
{
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const std::string path = saveTitle("Lower Third.ustitle");
    auto next = titles::importTitle(shell, path, shell.video, std::nullopt);
    REQUIRE(next.has_value());
    const core::Model &model = shell.model();
    REQUIRE(model.project().bin.size() == 1);
    const core::Asset &asset = model.project().bin.front();
    CHECK(asset.displayName == "Lower Third.ustitle");
    CHECK(asset.info.hasVideo);
    CHECK_FALSE(asset.info.hasAudio);
    CHECK(asset.info.isBoundless());
    REQUIRE(model.track(shell.video).clips.size() == 1);
    const core::Clip &clip = model.clip(model.track(shell.video).clips.front());
    CHECK(clip.length() == 78); // 93 frames at 30 fps = 3.1 s = 77.5 at 25, rounded
    CHECK(**next == 78);
    // What T1 doesn't know about is reported, not fatal.
    CHECK(shell.status == "Lower Third.ustitle: <frobnicate> isn't supported yet");
    // One undo step removes both.
    shell.undo();
    CHECK(model.project().bin.empty());
    // Not a title: a clean error.
    const std::string bad = core::utf8String(scratch() / "bad.ustitle");
    {
        std::FILE *f = std::fopen(bad.c_str(), "w");
        std::fputs("hello", f);
        std::fclose(f);
    }
    CHECK_FALSE(titles::importTitle(shell, bad, shell.video, std::nullopt).has_value());
}

TEST_CASE("a saved title is reported changed within a second, once per save")
{
    if (!haveGtk())
        return;
    core::Profile profile;
    FakeShell shell(profile);
    titles::extendShell(shell);
    REQUIRE(shell.importHandlers.size() == 2); // titles, and subtitles (T5)
    CHECK(shell.importHandlers[0].extensions == std::vector<std::string>{"ustitle"});
    const std::string path = saveTitle("watched.ustitle");
    REQUIRE(shell.importHandlers[0].import(path, shell.video, std::nullopt).has_value());
    const core::AssetId asset = shell.model().project().bin.front().id;
    spinUntil([] { return false; }, std::chrono::milliseconds(300)); // let the monitor settle

    // An atomic save (a temporary file renamed over it), as the titles app does.
    saveTitle("watched.ustitle", "#19e3ff");
    const auto took = spinUntil([&] { return !shell.changedOnDisk.empty(); }, std::chrono::milliseconds(2000));
    REQUIRE(shell.changedOnDisk.size() == 1);
    CHECK(shell.changedOnDisk[0] == asset);
    CHECK(took < std::chrono::milliseconds(1000));
    spinUntil([] { return false; }, std::chrono::milliseconds(400));
    CHECK(shell.changedOnDisk.size() == 1); // the save's several events settle into one

    // Undoing the import stops the watch.
    shell.undo();
    shell.changedOnDisk.clear();
    saveTitle("watched.ustitle", "#ff3cc7");
    spinUntil([] { return false; }, std::chrono::milliseconds(500));
    CHECK(shell.changedOnDisk.empty());
}

TEST_CASE("Edit Title: the action and a double-click open a title clip, and nothing else")
{
    if (!haveGtk())
        return;
    std::vector<std::string> launched;
    titles::setTitlesLauncherForTesting([&](const std::string &path, GdkTexture *, bool) {
        launched.push_back(path);
        return std::string();
    });
    core::Profile profile;
    FakeShell shell(profile);
    titles::extendShell(shell);
    REQUIRE(shell.overlays.size() == 1);
    // New Title, Edit Title, Bake Title, Open U-Stu Titles, Export Captions…
    REQUIRE(shell.actions.size() == 5);
    CHECK(std::string(shell.actions[3].first.name) == "titles-open");
    CHECK(std::string(shell.actions[4].first.name) == "titles-export-captions");
    // Export Captions… without captions: says so, and opens no dialog.
    shell.actions[4].first.activated(nullptr, nullptr, shell.actions[4].second);
    CHECK(shell.status == "No captions to export: import a .srt or .vtt file first.");
    REQUIRE(shell.headerButtons.size() == 1); // the header's "T"
    CHECK(std::string(gtk_actionable_get_action_name(GTK_ACTIONABLE(shell.headerButtons[0]))) == "win.titles-open");
    // Its icon is in the drop-in's own resources, on the icon theme's path.
    CHECK(
        gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(gdk_display_get_default()), "ustudio-titles-symbolic"));
    CHECK(std::string(shell.actions[0].first.name) == "titles-new");
    CHECK(std::string(shell.actions[2].first.name) == "titles-bake");
    CHECK(std::string(shell.actions[1].first.name) == "titles-edit");
    const std::string path = saveTitle("edit-me.ustitle");
    REQUIRE(titles::importTitle(shell, path, shell.video, 100).has_value()); // frames 100..192
    const core::ClipId clip = shell.model().track(shell.video).clips.front();

    // The action: nothing selected, then the title clip.
    shell.actions[1].first.activated(nullptr, nullptr, shell.actions[1].second);
    CHECK(launched.empty());
    CHECK(shell.status == "Select a title clip to edit it.");
    shell.selection.clips = {clip};
    shell.actions[1].first.activated(nullptr, nullptr, shell.actions[1].second);
    REQUIRE(launched.size() == 1);
    CHECK(launched[0] == path);

    // The double-click: on the clip (under the name strip), not a single
    // click, not beside it.
    const app::timeline::Viewport viewport; // one pixel a frame from x = 0
    app::timeline::RowLayout layout;
    const double x = viewport.xForFrame(150.0), y = layout.rowHeight - 5.0;
    CHECK_FALSE(shell.overlays[0]->pressed(shell.model(), viewport, layout, x, y, 1));
    CHECK(shell.overlays[0]->pressed(shell.model(), viewport, layout, x, y, 2));
    CHECK(launched.size() == 2);
    CHECK_FALSE(shell.overlays[0]->pressed(shell.model(), viewport, layout, viewport.xForFrame(50.0), y, 2));
    CHECK_FALSE(shell.overlays[0]->pressed(shell.model(), viewport, layout, x, 2.0, 2)); // the name strip
    CHECK(launched.size() == 2);

    // The header's "T": the selected title clip's title, else a new one
    // (no path) with the gallery; the timeline never changes.
    const size_t clips = shell.model().track(shell.video).clips.size();
    shell.actions[3].first.activated(nullptr, nullptr, shell.actions[3].second);
    REQUIRE(launched.size() == 3);
    CHECK(launched[2] == path);
    shell.selection.clips = {};
    shell.actions[3].first.activated(nullptr, nullptr, shell.actions[3].second);
    REQUIRE(launched.size() == 4);
    CHECK(launched[3].empty());
    CHECK(shell.model().track(shell.video).clips.size() == clips);
    titles::setTitlesLauncherForTesting(nullptr);
}

TEST_CASE("the designer is found: next to the program, else this build's")
{
    CHECK(titles::titlesAppPath() == TITLES_APP_BUILD_PATH);
}

TEST_CASE("the Title page edits the selected clip's fields, one undo step per entry visit")
{
    if (!haveGtk())
        return;
    core::Profile profile;
    FakeShell shell(profile);
    titles::extendShell(shell);
    REQUIRE(shell.pages.size() == 1);
    GtkWidget *page = shell.pages[0];
    const fs::path file = scratch() / "guest.ustitle";
    {
        std::FILE *f = std::fopen(file.string().c_str(), "w");
        std::fputs(R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="0" hold="60" outro="0"/>
  <field name="name" default="Jay Doe"/><field name="role" default="Host"/>
  <layer kind="text" x="10" y="10" w="500"><text>{{name}}, {{role}}</text></layer>
</ustitle>)",
                   f);
        std::fclose(f);
    }
    REQUIRE(titles::importTitle(shell, core::utf8String(file), shell.video, 0).has_value());
    const core::ClipId clip = shell.model().track(shell.video).clips.front();

    std::vector<GtkWidget *> entries;
    entriesIn(page, entries);
    CHECK(entries.empty()); // nothing selected
    shell.select({clip});
    entriesIn(page, entries);
    REQUIRE(entries.size() == 2);
    CHECK(std::string(gtk_editable_get_text(GTK_EDITABLE(entries[0]))) == "Jay Doe");

    // Typing is one step; the default isn't stored.
    gtk_editable_set_text(GTK_EDITABLE(entries[0]), "A");
    gtk_editable_set_text(GTK_EDITABLE(entries[0]), "Ada");
    auto values = titles::clipFieldValues(shell.model().clip(clip));
    CHECK(values == std::map<std::string, std::string>{{"name", "Ada"}});
    shell.undo();
    CHECK(titles::clipFieldValues(shell.model().clip(clip)).empty());
    CHECK(std::string(gtk_editable_get_text(GTK_EDITABLE(entries[0]))) == "Jay Doe"); // the page follows undo

    // Nothing selected: no entries.
    shell.select({});
    CHECK_FALSE(gtk_widget_get_visible(gtk_widget_get_parent(gtk_widget_get_parent(entries[0]))));
}

TEST_CASE("Bake Title: the clip plays a ProRes file, keeps its place and transform, and undo brings the title back")
{
    if (!haveGtk())
        return;
    setUpMlt();
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const fs::path dir = scratch() / "bake";
    fs::create_directories(dir);
    const std::string path = saveTitle("bake/Guest.ustitle");
    REQUIRE(titles::importTitle(shell, path, shell.video, 30).has_value());
    const core::ClipId clip = shell.model().track(shell.video).clips.front();
    REQUIRE(shell.execute(
        std::make_unique<titles::SetClipFields>(clip, std::map<std::string, std::string>{{"name", "Ada"}})));
    core::Transform flipped;
    flipped.flipH = true;
    REQUIRE(shell.execute(std::make_unique<core::SetClipTransform>(clip, flipped)));
    const core::Clip before = shell.model().clip(clip);

    CHECK(core::utf8String(core::pathFromUtf8(titles::bakePath(path)).filename()) == "Guest (baked).mov");
    titles::bakeTitleClip(shell, clip);
    CHECK(shell.status.starts_with("Baking"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (shell.status.starts_with("Baking") && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, TRUE);
    INFO(shell.status);
    REQUIRE(shell.status.starts_with("Baked"));

    const core::Clip &baked = shell.model().clip(clip);
    const core::Asset &asset = shell.model().asset(baked.asset);
    CHECK(asset.path.ends_with("Guest (baked).mov"));
    CHECK(asset.info.lengthInSequenceFrames >= baked.out + 1);
    CHECK(baked.position == before.position);
    CHECK(baked.in == before.in);
    CHECK(baked.out == before.out);
    CHECK(baked.transform.get().flipH);
    CHECK(baked.sourceParams.empty());
    CHECK(shell.model().check().empty());
    // The name is taken now.
    CHECK(core::utf8String(core::pathFromUtf8(titles::bakePath(path)).filename()) == "Guest (baked 2).mov");

    shell.undo();
    CHECK(shell.model().clip(clip).asset == before.asset);
    CHECK(titles::clipFieldValues(shell.model().clip(clip)).at("name") == "Ada");
}

TEST_CASE("Export Title from the editor: the clip's title on its own, at the clip's length; the project unchanged")
{
    if (!haveGtk())
        return;
    setUpMlt();
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const std::string path = saveTitle("export-clip.ustitle");
    REQUIRE(titles::importTitle(shell, path, shell.video, 0).has_value());
    const core::ClipId clip = shell.model().track(shell.video).clips.front();
    const core::Project before = shell.model().project();
    const fs::path out = scratch() / "export-clip-frames";
    fs::remove_all(out);
    titles::exportTitleClip(shell, clip, "png", core::utf8String(out));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (shell.status.starts_with("Exporting") && std::chrono::steady_clock::now() < deadline)
        g_main_context_iteration(nullptr, TRUE);
    INFO(shell.status);
    REQUIRE(shell.status.starts_with("Exported"));
    size_t count = 0;
    for (const auto &entry : fs::directory_iterator(out))
        count += entry.path().extension() == ".png";
    CHECK(count == static_cast<size_t>(shell.model().clip(clip).length()));
    CHECK(shell.model().project() == before);
}

TEST_CASE("New Title: a blank title in the project's Titles folder, at the playhead, opened with the gallery")
{
    if (!haveGtk())
        return;
    std::vector<std::pair<std::string, bool>> launched;
    titles::setTitlesLauncherForTesting([&](const std::string &path, GdkTexture *, bool gallery) {
        launched.push_back({path, gallery});
        return std::string();
    });
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const fs::path project = scratch() / "new-title-project";
    fs::remove_all(project);
    fs::create_directories(project);
    shell.folder = core::utf8String(project);
    titles::extendShell(shell);
    REQUIRE(std::string(shell.actions[0].first.name) == "titles-new");
    shell.actions[0].first.activated(nullptr, nullptr, shell.actions[0].second);

    const fs::path first = project / "Titles" / "Title 1.ustitle";
    REQUIRE(fs::exists(first));
    REQUIRE(launched.size() == 1);
    CHECK(launched[0] == std::pair<std::string, bool>{core::utf8String(first), true});
    const auto &clips = shell.model().track(shell.video).clips;
    REQUIRE(clips.size() == 1);
    const core::Clip &clip = shell.model().clip(clips.front());
    CHECK(clip.position == 0);   // the playhead
    CHECK(clip.length() == 125); // five seconds at the sequence's 25 fps
    auto read = titles::readTitle(core::utf8String(first));
    REQUIRE(read.has_value());
    CHECK(read->document.fpsNum == 25);
    CHECK(read->document.layers.empty());

    // The next one never overwrites it.
    CHECK(titles::newTitlePath(core::utf8String(project)) == core::utf8String(project / "Titles" / "Title 2.ustitle"));
    // No project folder: the Videos folder's "U-Stu Titles", or one in the
    // data dir; never a folder straight in $HOME.
    const fs::path fallback = core::pathFromUtf8(titles::newTitlePath("")).parent_path();
    CHECK(fallback.filename() == "U Stu Titles"); // a folder name: unchanged by the U-Stu rename
    CHECK(fallback.parent_path() != core::pathFromUtf8(g_get_home_dir()));
    titles::setTitlesLauncherForTesting(nullptr);
}

TEST_CASE("New Title in an unsaved project: the link survives saving the project there and moving the folder, or "
          "saving it elsewhere")
{
    if (!haveGtk())
        return;
    titles::setTitlesLauncherForTesting([](const std::string &, GdkTexture *, bool) { return std::string(); });
    core::Profile profile;
    FakeShell shell(profile);
    const fs::path home = scratch() / "unsaved-home";
    fs::remove_all(home);
    fs::remove_all(scratch() / "unsaved-home-moved");
    fs::remove_all(scratch() / "elsewhere");
    fs::create_directories(home);
    shell.folder = core::utf8String(home); // Settings › Locations' default, the project not saved yet
    auto made = titles::newTitle(shell);
    REQUIRE_MESSAGE(made.has_value(), (made ? "" : made.error()));
    const std::string titlePath = shell.model().asset(shell.model().clip(*made).asset).path;

    // Saved elsewhere: the title's path stays absolute and still opens.
    fs::create_directories(scratch() / "elsewhere");
    const std::string other = core::utf8String(scratch() / "elsewhere" / "show.ustudio");
    REQUIRE(core::saveProject(shell.model(), other).empty());
    auto again = core::loadProject(other);
    REQUIRE(again.has_value());
    CHECK(again->asset(again->clip(*made).asset).path == titlePath);

    // Saved in the folder it was made for, then the folder moved: the title
    // travels with it.
    const std::string there = core::utf8String(home / "show.ustudio");
    REQUIRE(core::saveProject(shell.model(), there).empty());
    fs::rename(home, scratch() / "unsaved-home-moved");
    auto moved = core::loadProject(core::utf8String(scratch() / "unsaved-home-moved" / "show.ustudio"));
    REQUIRE(moved.has_value());
    const std::string relinked = moved->asset(moved->clip(*made).asset).path;
    CHECK(relinked == core::utf8String(scratch() / "unsaved-home-moved" / "Titles" / "Title 1.ustitle"));
    CHECK(fs::exists(core::pathFromUtf8(relinked)));
    titles::setTitlesLauncherForTesting(nullptr);
}

TEST_CASE("importing subtitles: a caption title beside the project, a clip per cue on tracks on top, one undo step")
{
    if (!haveGtk())
        return;
    core::Profile profile;
    profile.fps = {25, 1};
    FakeShell shell(profile);
    const fs::path project = scratch() / "captions-project";
    fs::remove_all(project);
    fs::create_directories(project);
    shell.folder = core::utf8String(project);
    titles::extendShell(shell);
    // .srt and .vtt go through the titles drop-in.
    const auto handler =
        std::find_if(shell.importHandlers.begin(), shell.importHandlers.end(), [](const app::ImportHandler &h) {
            return std::find(h.extensions.begin(), h.extensions.end(), "srt") != h.extensions.end();
        });
    REQUIRE(handler != shell.importHandlers.end());
    CHECK(std::find(handler->extensions.begin(), handler->extensions.end(), "vtt") != handler->extensions.end());

    const fs::path srt = scratch() / "show.srt";
    {
        std::ofstream out(srt, std::ios::binary);
        out << "1\n00:00:01,000 --> 00:00:03,000\nHello\n\n"
               "2\n00:00:02,000 --> 00:00:04,000\n<i>Overlapping</i>\n\n"
               "3\n00:00:0X,000 --> 00:00:05,000\nbroken\n\n"
               "4\n00:00:05,000 --> 00:00:06,000\n<v Jay>Bye</v>\n";
    }
    const size_t tracksBefore = shell.model().sequence().tracks.size();
    auto result = handler->import(core::utf8String(srt), shell.video, std::nullopt);
    REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error()));
    CHECK(shell.status.find("Imported 3 captions") != std::string::npos);
    CHECK(shell.status.find("Skipped 1 unreadable cue (the first at line 10") != std::string::npos);

    const fs::path title = project / "Titles" / "show captions.ustitle";
    REQUIRE(fs::exists(title));
    const auto &tracks = shell.model().sequence().tracks;
    REQUIRE(tracks.size() == tracksBefore + 2);
    CHECK(tracks[0].name == "Captions");
    CHECK(tracks[1].name == "Captions 2");
    REQUIRE(tracks[0].clips.size() == 2);
    REQUIRE(tracks[1].clips.size() == 1);
    const core::Clip &hello = shell.model().clip(tracks[0].clips[0]);
    CHECK(hello.position == 25);
    CHECK(hello.length() == 50);
    CHECK(shell.model().asset(hello.asset).path == core::utf8String(title));
    CHECK(titles::clipFieldValues(hello).at("caption") == "Hello");
    CHECK(titles::clipFieldValues(shell.model().clip(tracks[1].clips[0])).at("caption") == "<i>Overlapping</i>");
    CHECK(titles::clipFieldValues(shell.model().clip(tracks[0].clips[1])).at("speaker") == "Jay");
    CHECK(shell.model().check().empty());

    shell.undo(); // one step
    CHECK(shell.model().sequence().tracks.size() == tracksBefore);
    CHECK(shell.model().project().bin.empty());

    // A second import of the same file makes its own caption title.
    REQUIRE(handler->import(core::utf8String(srt), shell.video, std::nullopt).has_value());
    CHECK(fs::exists(project / "Titles" / "show captions 2.ustitle"));

    // A file that isn't subtitles: refused, nothing added.
    const fs::path junk = scratch() / "junk.vtt";
    std::ofstream(junk) << "WEBVTT\n\nnothing to see\n";
    const size_t tracksNow = shell.model().sequence().tracks.size();
    auto refused = handler->import(core::utf8String(junk), shell.video, std::nullopt);
    REQUIRE_FALSE(refused.has_value());
    CHECK(shell.model().sequence().tracks.size() == tracksNow);
}
