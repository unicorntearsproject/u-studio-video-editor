// The designer's window closing (a demo blocker, 2026-09-29): closing U Stu
// Titles with a layer selected crashed in GtkListBox's dispose, which
// emitted row-selected into the layers panel after the window's C++ object
// was gone. Each part now disconnects its handlers in its destructor. Run
// under ASan (`just asan titles-window`), where a call into a freed part is
// a report even when it doesn't crash.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/titles_window.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "platform/process.h"

#include <adwaita.h>

#include <cstdlib>
#include <fstream>
#include <vector>
#include <filesystem>
#include <string>

using namespace ustudio;
using namespace ustudio::titles;
namespace fs = std::filesystem;

namespace {

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-titles-window-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir); // every time: a test may have removed it
    return dir;
}

bool haveGtk()
{
    // Nothing the tests do reaches the user's settings or templates.
    for (const char *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"})
        g_setenv(name, core::utf8String(scratch() / name).c_str(), TRUE);
    if (gtk_init_check()) {
        adw_init();
        return true;
    }
    MESSAGE("no display: skipped");
    return false;
}

GtkApplication *application()
{
    static GtkApplication *app = [] {
        GtkApplication *made = GTK_APPLICATION(
            adw_application_new("com.ustudio.Titles.Test", static_cast<GApplicationFlags>(G_APPLICATION_NON_UNIQUE)));
        g_application_register(G_APPLICATION(made), nullptr, nullptr);
        return made;
    }();
    return app;
}

// The first GtkListBox under `widget`: the layers list.
GtkWidget *findListBox(GtkWidget *widget)
{
    if (GTK_IS_LIST_BOX(widget))
        return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (GtkWidget *found = findListBox(child))
            return found;
    return nullptr;
}

void settle()
{
    for (int i = 0; i < 50 && g_main_context_iteration(nullptr, FALSE); ++i) {
    }
}

std::string twoLayerTitle()
{
    const std::string path = core::utf8String(scratch() / "two layers.ustitle");
    auto doc = parseTitle(R"(<ustitle version="1" width="640" height="360" fps="30/1">
      <timing intro="10" hold="30" outro="10"/>
      <layer id="back" kind="shape" x="20" y="200" w="400" h="80"><fill color="#1b1230"/></layer>
      <layer id="name" kind="text" x="40" y="210" w="360"><text>Jay Doe</text><font size="48"/></layer>
    </ustitle>)");
    REQUIRE(doc.has_value());
    REQUIRE(saveTitle(doc->document, path).empty());
    return path;
}

} // namespace

TEST_CASE("closing the designer with a layer selected doesn't call into the closed window")
{
    if (!haveGtk())
        return;
    const std::string path = twoLayerTitle();
    for (int round = 0; round < 3; ++round) {
        CAPTURE(round);
        // Deletes itself when its window is destroyed.
        auto *window = new app::TitlesWindow(application(), path, {});
        GtkWindow *gtkWindow = window->window();
        gtk_window_present(gtkWindow);
        settle();
        GtkWidget *list = findListBox(GTK_WIDGET(gtkWindow));
        REQUIRE(list != nullptr);
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), round % 2);
        REQUIRE(row != nullptr);
        gtk_list_box_select_row(GTK_LIST_BOX(list), row); // as a click does
        settle();
        // The panel rebuilt its rows for the selection: a row is selected.
        REQUIRE(gtk_list_box_get_selected_row(GTK_LIST_BOX(findListBox(GTK_WIDGET(gtkWindow)))) != nullptr);
        // Close it, as Close does: the dispose that follows must not reach
        // the window's parts.
        g_object_add_weak_pointer(G_OBJECT(gtkWindow), reinterpret_cast<gpointer *>(&gtkWindow));
        gtk_window_destroy(gtkWindow);
        settle();
        CHECK(gtkWindow == nullptr);
    }
    fs::remove_all(scratch());
}

namespace {

// The transport's Play button: the button showing the play icon.
GtkWidget *findPlayButton(GtkWidget *widget)
{
    if (GTK_IS_BUTTON(widget)) {
        const char *icon = gtk_button_get_icon_name(GTK_BUTTON(widget));
        if (icon && std::string(icon) == "media-playback-start-symbolic")
            return widget;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (GtkWidget *found = findPlayButton(child))
            return found;
    return nullptr;
}

} // namespace

TEST_CASE("closing the designer while it plays doesn't touch the freed transport (2026-10-08)")
{
    // ~TitlesWindow stopped playback through stopPlaying(), which set the
    // play button's icon after the window's dispose had freed it: a SIGSEGV
    // whenever a play tick was still running at close. meson's
    // MALLOC_PERTURB_ makes it a crash every time; ASan misses it (the read
    // is in libgobject, which isn't instrumented).
    if (!haveGtk())
        return;
    const std::string path = twoLayerTitle();
    auto *window = new app::TitlesWindow(application(), path, {});
    GtkWindow *gtkWindow = window->window();
    gtk_window_present(gtkWindow);
    settle();
    GtkWidget *play = findPlayButton(GTK_WIDGET(gtkWindow));
    REQUIRE(play != nullptr);
    g_signal_emit_by_name(play, "clicked");
    settle();
    CHECK(std::string(gtk_button_get_icon_name(GTK_BUTTON(play))) == "media-playback-pause-symbolic");
    g_object_add_weak_pointer(G_OBJECT(gtkWindow), reinterpret_cast<gpointer *>(&gtkWindow));
    gtk_window_destroy(gtkWindow);
    settle();
    CHECK(gtkWindow == nullptr);
    fs::remove_all(scratch());
}

namespace {

void collect(GtkWidget *widget, std::vector<GtkWidget *> &out)
{
    out.push_back(widget);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        collect(child, out);
}

} // namespace

TEST_CASE("the inspector's number rows are reachable by screen readers: spin buttons with labels (Demos, 2026-09-29)")
{
    // libadwaita 1.9.2 leaves AdwSpinRow out of the AT-SPI tree, so the
    // inspector's number rows are action rows with a labelled spin button.
    if (!haveGtk())
        return;
    {
        std::ofstream(scratch() / "square.json")
            << R"({"v":"5.7.0","fr":30,"ip":0,"op":30,"w":512,"h":512,"assets":[],"layers":[]})";
    }
    const std::string path = core::utf8String(scratch() / "animated.ustitle");
    auto doc = parseTitle(R"(<ustitle version="2" width="1920" height="1080" fps="30/1">
      <timing intro="10" hold="30" outro="10"/>
      <layer id="anim" kind="lottie" src="square.json" x="704" y="284" w="512" h="512"/>
    </ustitle>)");
    REQUIRE(doc.has_value());
    REQUIRE(saveTitle(doc->document, path).empty());
    auto *window = new app::TitlesWindow(application(), path, {});
    GtkWindow *gtkWindow = window->window();
    gtk_window_present(gtkWindow);
    settle();
    GtkWidget *list = findListBox(GTK_WIDGET(gtkWindow));
    REQUIRE(list != nullptr);
    gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    settle();
    std::vector<GtkWidget *> widgets;
    collect(GTK_WIDGET(gtkWindow), widgets);
    int spins = 0;
    bool speed = false;
    for (GtkWidget *widget : widgets) {
        CHECK_FALSE(ADW_IS_SPIN_ROW(widget));
        if (!GTK_IS_SPIN_BUTTON(widget))
            continue;
        ++spins;
        CHECK(gtk_test_accessible_has_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL));
        if (char *differs =
                gtk_test_accessible_check_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, "Speed (%)"))
            g_free(differs);
        else
            speed = true;
    }
    CHECK(spins > 5); // Speed, position, size, rotation, ...
    CHECK(speed);
    gtk_window_destroy(gtkWindow);
    settle();
    fs::remove_all(scratch());
}
