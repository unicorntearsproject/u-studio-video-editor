// The Help dialog: Controls, Keyboard Shortcuts, Release notes and About.
//
// M4 G: each category is a collapsible section (AdwExpanderRow, class
// "help-section", styled in style.css), collapsed until opened. Help comes
// back as it was left: the open sections, the tab and each tab's scroll
// position, for the session (AppWindow members) and across restarts
// (Settings; nothing is lost without a schema, it just isn't kept). Release
// notes are the AppStream metainfo's <releases>, compiled into the
// GResource, one section per release, newest first.

#include "app_window.h"

#include "action_registry.h"
#include "diagnostics.h"
#include "project_links.h"
#include "ui_hints.h"
#include "core/log.h"
#include "core/media/utf8_path.h"
#include "core/trace.h"
#include "engine/factory_policy.h"
#include "platform/process.h"
#include "core/xml/release_notes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ustudio::app {

namespace {

// Each tab's vertical scroll adjustment, held while Help is open.
using HelpScrollers = std::vector<std::pair<std::string, GtkAdjustment *>>;

constexpr const char *kMetainfoResource = "/com/ustudio/VideoEditor/com.ustudio.VideoEditor.metainfo.xml";

std::string plural(size_t count, const char *one, const char *many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// "25 September 2026" from "2026-09-25"; the text as written otherwise.
std::string longDate(const std::string &iso)
{
    int year = 0, month = 0, day = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
        return iso;
    GDateTime *date = g_date_time_new_local(year, month, day, 0, 0, 0);
    if (!date)
        return iso;
    gchar *text = g_date_time_format(date, "%-d %B %Y");
    std::string out = text ? text : iso;
    g_free(text);
    g_date_time_unref(date);
    return out;
}

// A row's title or subtitle is Pango markup: a shortcut's label can be "<"
// itself ("Shift+,, <" failed to parse and showed nothing).
GtkWidget *textRow(const char *title, const std::string &subtitle)
{
    GtkWidget *row = adw_action_row_new();
    char *escapedTitle = g_markup_escape_text(title, -1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escapedTitle);
    g_free(escapedTitle);
    if (!subtitle.empty()) {
        char *escaped = g_markup_escape_text(subtitle.c_str(), -1);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), escaped);
        g_free(escaped);
    }
    return row;
}

GtkScrolledWindow *scrollerOf(GtkWidget *widget)
{
    if (GTK_IS_SCROLLED_WINDOW(widget))
        return GTK_SCROLLED_WINDOW(widget);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (GtkScrolledWindow *found = scrollerOf(child))
            return found;
    return nullptr;
}

} // namespace

void AppWindow::loadHelpState()
{
    if (m_helpStateLoaded)
        return;
    m_helpStateLoaded = true;
    for (const std::string &section : m_settings->helpOpenSections())
        m_helpOpenSections.insert(section);
    m_helpTab = m_settings->helpTab();
    for (const std::string &entry : m_settings->helpScroll())
        if (size_t eq = entry.find('='); eq != std::string::npos)
            m_helpScroll[entry.substr(0, eq)] = std::strtod(entry.c_str() + eq + 1, nullptr);
}

AdwExpanderRow *AppWindow::helpSection(AdwPreferencesGroup *group, const char *tab, const std::string &title,
                                       const std::string &subtitle)
{
    AdwExpanderRow *row = ADW_EXPANDER_ROW(adw_expander_row_new());
    gtk_widget_add_css_class(GTK_WIDGET(row), "help-section");
    char *escaped = g_markup_escape_text(title.c_str(), -1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), escaped);
    g_free(escaped);
    adw_expander_row_set_subtitle(row, subtitle.c_str());
    const std::string key = std::string(tab) + ":" + title;
    adw_expander_row_set_expanded(row, m_helpOpenSections.contains(key));
    g_object_set_data_full(G_OBJECT(row), "help-key", g_strdup(key.c_str()), g_free);
    g_signal_connect(row, "notify::expanded", G_CALLBACK(+[](AdwExpanderRow *expander, GParamSpec *, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         const std::string sectionKey =
                             static_cast<const char *>(g_object_get_data(G_OBJECT(expander), "help-key"));
                         if (adw_expander_row_get_expanded(expander))
                             window->m_helpOpenSections.insert(sectionKey);
                         else
                             window->m_helpOpenSections.erase(sectionKey);
                         window->m_settings->setHelpOpenSections(
                             {window->m_helpOpenSections.begin(), window->m_helpOpenSections.end()});
                     }),
                     this);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return row;
}

GtkWidget *AppWindow::buildShortcutsPage()
{
    GtkWidget *page = adw_preferences_page_new();
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), group);

    // One section per ActionSpec::category, in the table's own order (it is
    // already grouped: Playback, Editing, ...), then the drop-ins' under
    // their own categories.
    const std::vector<ActionSpec> specs = allActionSpecs();
    std::vector<std::string> order;
    for (const ActionSpec &spec : specs)
        if (std::find(order.begin(), order.end(), spec.category) == order.end())
            order.emplace_back(spec.category);
    for (const std::string &category : order) {
        std::vector<const ActionSpec *> inCategory;
        for (const ActionSpec &spec : specs)
            if (category == spec.category)
                inCategory.push_back(&spec);
        AdwExpanderRow *section =
            helpSection(group, "shortcuts", category, plural(inCategory.size(), "action", "actions"));
        for (const ActionSpec *spec : inCategory)
            adw_expander_row_add_row(section, textRow(spec->label, shortcutLabel(spec->name)));
    }
    return page;
}

GtkWidget *AppWindow::buildControlsPage()
{
    GtkWidget *page = adw_preferences_page_new();
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), group);

    const std::vector<HintSpec> &hints = hintSpecs();
    std::vector<std::string> order;
    for (const HintSpec &hint : hints)
        if (std::find(order.begin(), order.end(), hint.category) == order.end())
            order.emplace_back(hint.category);
    for (const std::string &category : order) {
        std::vector<const HintSpec *> inCategory;
        for (const HintSpec &hint : hints)
            if (category == hint.category)
                inCategory.push_back(&hint);
        AdwExpanderRow *section =
            helpSection(group, "controls", category, plural(inCategory.size(), "control", "controls"));
        for (const HintSpec *hint : inCategory) {
            // "Click the clip, then Delete": the gesture, then the key.
            std::string how = hint->gesture != nullptr ? hint->gesture : "";
            if (std::string shortcut = shortcutLabel(hint->action); !shortcut.empty())
                how += how.empty() ? shortcut : " " + shortcut;
            std::string subtitle = how;
            if (hint->detail != nullptr)
                subtitle += subtitle.empty() ? hint->detail : std::string("\n") + hint->detail;
            adw_expander_row_add_row(section, textRow(hint->title, subtitle));
        }
    }
    return page;
}

GtkWidget *AppWindow::buildReleaseNotesPage()
{
    std::vector<core::ReleaseNote> notes;
    if (GBytes *bytes = g_resources_lookup_data(kMetainfoResource, G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr)) {
        gsize size = 0;
        const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
        notes = core::parseReleaseNotes(std::string_view(data, size));
        g_bytes_unref(bytes);
    }
    if (notes.empty()) {
        GtkWidget *empty = adw_status_page_new();
        adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No release notes");
        adw_status_page_set_description(ADW_STATUS_PAGE(empty), "This build doesn't carry any.");
        return empty;
    }

    GtkWidget *page = adw_preferences_page_new();
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_description(group, "Every released build, newest first.");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), group);
    for (const core::ReleaseNote &note : notes) {
        std::string subtitle = note.type == "development" ? "Beta" : "";
        if (!note.date.empty())
            subtitle += (subtitle.empty() ? "" : " · ") + longDate(note.date);
        AdwExpanderRow *section = helpSection(group, "releases", note.version, subtitle);
        GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(body, "release-notes-body");
        gtk_widget_set_margin_top(body, 10);
        gtk_widget_set_margin_bottom(body, 12);
        gtk_widget_set_margin_start(body, 14);
        gtk_widget_set_margin_end(body, 14);
        if (note.blocks.empty()) {
            GtkWidget *none = gtk_label_new("No notes for this release.");
            gtk_widget_add_css_class(none, "dim-label");
            gtk_label_set_xalign(GTK_LABEL(none), 0);
            gtk_box_append(GTK_BOX(body), none);
        }
        for (const core::ReleaseNote::Block &block : note.blocks) {
            const std::string text =
                block.kind == core::ReleaseNote::Block::Kind::ListItem ? "•  " + block.text : block.text;
            GtkWidget *label = gtk_label_new(text.c_str());
            gtk_label_set_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            if (block.kind == core::ReleaseNote::Block::Kind::ListItem)
                gtk_widget_set_margin_start(label, 8);
            gtk_box_append(GTK_BOX(body), label);
        }
        adw_expander_row_add_row(section, body);
    }
    return page;
}

GtkWidget *AppWindow::buildAboutPage()
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);

    // The owner's wordmark (data/logos, in the GResource), scaled to fit;
    // the name as text if it can't be loaded (no SVG loader, say).
    GtkWidget *logo = gtk_picture_new_for_resource("/com/ustudio/VideoEditor/logos/u-stu-video-editor-full.svg");
    if (gtk_picture_get_paintable(GTK_PICTURE(logo))) {
        gtk_picture_set_content_fit(GTK_PICTURE(logo), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_size_request(logo, 420, 126);
        gtk_picture_set_alternative_text(GTK_PICTURE(logo), "U-Stu Video Editor");
        gtk_box_append(GTK_BOX(box), logo);
    } else {
        g_object_ref_sink(logo);
        g_object_unref(logo);
        GtkWidget *title = gtk_label_new("U-Stu Video Editor");
        gtk_widget_add_css_class(title, "title-1");
        gtk_box_append(GTK_BOX(box), title);
    }

    GtkWidget *version = gtk_label_new("Version " USTUDIO_VERSION);
    gtk_widget_add_css_class(version, "dim-label");
    gtk_box_append(GTK_BOX(box), version);

    auto paragraph = [&](const std::string &markup, int top, bool dim) {
        GtkWidget *label = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
        gtk_widget_set_margin_top(label, top);
        if (dim)
            gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(box), label);
    };
    auto link = [](const char *url, const char *text) {
        return std::string("<a href=\"") + url + "\">" + text + "</a>";
    };
    paragraph("A GNOME-native multi-track video editor built on GTK4, libadwaita and MLT, made for "
              "livestream and promo editing. It is fast, and it never uses the network.",
              14, false);
    paragraph(std::string("Part of ") + link(links::kProjectUrl, links::kProjectName) + ".", 8, false);
    paragraph(link(links::kSourceUrl, "Source code") + " · " + link(links::kLicenseUrl, "MIT licence"), 8, false);
    paragraph("The Flatpak also bundles FFmpeg and x264, which are GPL-licensed.", 8, true);

    // For bug reports (0.49.0-beta.2).
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(buttons, 18);
    GtkWidget *logs = gtk_button_new_with_label("Open Log Folder");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(logs), "win.open-log-folder");
    setTooltip(logs, "help.open-log-folder");
    gtk_box_append(GTK_BOX(buttons), logs);
    GtkWidget *copy = gtk_button_new_with_label("Copy Diagnostics");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(copy), "win.copy-diagnostics");
    setTooltip(copy, "help.copy-diagnostics");
    gtk_box_append(GTK_BOX(buttons), copy);
    gtk_box_append(GTK_BOX(box), buttons);

    return box;
}

GtkAdjustment *AppWindow::restoreHelpScroll(GtkWidget *page, const char *tab)
{
    GtkScrolledWindow *scroller = scrollerOf(page);
    if (!scroller)
        return nullptr;
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(scroller);
    auto it = m_helpScroll.find(tab);
    if (it == m_helpScroll.end() || it->second <= 0)
        return adjustment;
    // The page gets its height over a few layout passes (the dialog's
    // opening animation, the open sections): scroll as far towards the
    // remembered position as each allows, for a moment, then stop so it
    // never fights the user.
    auto *want = new double(it->second);
    g_object_set_data_full(
        G_OBJECT(adjustment), "help-restore", want, +[](gpointer data) { delete static_cast<double *>(data); });
    g_signal_connect(
        adjustment, "changed", G_CALLBACK(+[](GtkAdjustment *adj, gpointer) {
            auto *wanted = static_cast<double *>(g_object_get_data(G_OBJECT(adj), "help-restore"));
            const double room = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
            if (!wanted || room <= 0)
                return;
            // "changed" comes from inside the viewport's
            // allocation, which drops the re-layout a value
            // change asks for (the view stayed at the top):
            // set it once that's done.
            g_idle_add_once(
                +[](gpointer data) {
                    auto *later = static_cast<GtkAdjustment *>(data);
                    if (auto *remembered = static_cast<double *>(g_object_get_data(G_OBJECT(later), "help-restore")))
                        gtk_adjustment_set_value(later, std::min(*remembered, gtk_adjustment_get_upper(later) -
                                                                                  gtk_adjustment_get_page_size(later)));
                    g_object_unref(later);
                },
                g_object_ref(adj));
        }),
        nullptr);
    g_timeout_add_once(
        1500,
        +[](gpointer data) {
            auto *adj = static_cast<GtkAdjustment *>(data);
            g_object_set_data(G_OBJECT(adj), "help-restore", nullptr);
            g_object_unref(adj);
        },
        g_object_ref(adjustment));
    return adjustment;
}

void AppWindow::openLogFolder()
{
    std::error_code ec;
    const std::filesystem::path folder = core::Log::directory();
    std::filesystem::create_directories(folder, ec);
    // This run's log, shown selected in the file manager: "open containing
    // folder" asks the file manager itself (FileManager1, or the OpenURI
    // portal's OpenDirectory inside Flatpak). Launching the folder as a
    // URI instead opened nothing the first time (owner, 2026-09-25): the
    // file manager, started by D-Bus activation for it, came up as a
    // background service without a window; the second request showed one.
    const std::filesystem::path current = core::Log::currentFile();
    GFile *file = g_file_new_for_path(core::utf8String(current.empty() ? folder : current).c_str());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    g_object_unref(file);
    auto done = +[](GObject *source, GAsyncResult *result, gpointer self) {
        GError *error = nullptr;
        auto *launched = GTK_FILE_LAUNCHER(source);
        const bool ok = static_cast<bool>(g_object_get_data(source, "folder-itself"))
                            ? gtk_file_launcher_launch_finish(launched, result, &error)
                            : gtk_file_launcher_open_containing_folder_finish(launched, result, &error);
        if (!ok) {
            static_cast<AppWindow *>(self)->showStatus(std::string("Couldn't open the log folder: ") +
                                                       (error ? error->message : "unknown error"));
            g_clear_error(&error);
        }
        g_object_unref(source);
    };
    if (current.empty()) { // no log file this run: the folder itself
        g_object_set_data(G_OBJECT(launcher), "folder-itself", GINT_TO_POINTER(1));
        gtk_file_launcher_launch(launcher, GTK_WINDOW(m_window), nullptr, done, this);
    } else {
        gtk_file_launcher_open_containing_folder(launcher, GTK_WINDOW(m_window), nullptr, done, this);
    }
}

void AppWindow::copyDiagnostics()
{
    DiagnosticsFacts facts;
    facts.appVersion = USTUDIO_VERSION;
    facts.mltVersion = engine::FactoryPolicy::mltVersion();
    facts.gtkVersion = std::to_string(gtk_get_major_version()) + "." + std::to_string(gtk_get_minor_version()) + "." +
                       std::to_string(gtk_get_micro_version());
    facts.adwaitaVersion = std::to_string(adw_get_major_version()) + "." + std::to_string(adw_get_minor_version()) +
                           "." + std::to_string(adw_get_micro_version());
    facts.flatpak = platform::runningInFlatpak();
    facts.gpu = m_gpu ? m_gpu->statusText() : "Off";
    facts.logFolder = core::utf8String(core::Log::directory());
    facts.recentLines = core::Log::recentLines(50);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(m_window)), formatDiagnostics(facts).c_str());
    showStatus("Diagnostics copied: paste them into your bug report.");
}

void AppWindow::diagnosticsActionActivated(GSimpleAction *action, GVariant *, gpointer userData)
{
    auto *window = static_cast<AppWindow *>(userData);
    if (std::strcmp(g_action_get_name(G_ACTION(action)), "open-log-folder") == 0)
        window->openLogFolder();
    else
        window->copyDiagnostics();
}

void AppWindow::showHelpDialog()
{
    core::trace::Scope trace("help: build");
    loadHelpState();
    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, "Help");
    adw_dialog_set_content_width(dialog, 640);
    adw_dialog_set_content_height(dialog, 560);

    AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());
    const std::pair<const char *, GtkWidget *> pages[] = {
        {"controls", buildControlsPage()},
        {"shortcuts", buildShortcutsPage()},
        {"releases", buildReleaseNotesPage()},
    };
    adw_view_stack_add_titled_with_icon(stack, pages[0].second, "controls", "Controls", "input-mouse-symbolic");
    adw_view_stack_add_titled_with_icon(stack, pages[1].second, "shortcuts", "Keyboard Shortcuts",
                                        "preferences-desktop-keyboard-shortcuts-symbolic");
    adw_view_stack_add_titled_with_icon(stack, pages[2].second, "releases", "Release Notes",
                                        "document-open-recent-symbolic");
    adw_view_stack_add_titled_with_icon(stack, buildAboutPage(), "about", "About", "help-about-symbolic");
    // Each tab's scroller, read when Help closes (not tracked as it moves:
    // closing resets them to 0 on the way out).
    auto *scrollers = new HelpScrollers;
    for (const auto &[tab, page] : pages)
        if (GtkAdjustment *adjustment = restoreHelpScroll(page, tab))
            scrollers->emplace_back(tab, GTK_ADJUSTMENT(g_object_ref(adjustment)));
    g_object_set_data_full(
        G_OBJECT(dialog), "help-scrollers", scrollers, +[](gpointer data) {
            auto *list = static_cast<HelpScrollers *>(data);
            for (auto &entry : *list)
                g_object_unref(entry.second);
            delete list;
        });
    if (!m_helpTab.empty() && adw_view_stack_get_child_by_name(stack, m_helpTab.c_str()))
        adw_view_stack_set_visible_child_name(stack, m_helpTab.c_str());
    g_signal_connect(stack, "notify::visible-child-name",
                     G_CALLBACK(+[](AdwViewStack *views, GParamSpec *, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         if (const char *name = adw_view_stack_get_visible_child_name(views)) {
                             window->m_helpTab = name;
                             window->m_settings->setHelpTab(name);
                         }
                     }),
                     this);
    // Scroll positions are read and written as Help starts closing.
    g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog *closing, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         auto *list =
                             static_cast<HelpScrollers *>(g_object_get_data(G_OBJECT(closing), "help-scrollers"));
                         for (const auto &[tab, adjustment] : *list)
                             window->m_helpScroll[tab] = gtk_adjustment_get_value(adjustment);
                         std::vector<std::string> positions;
                         for (const auto &[tab, value] : window->m_helpScroll)
                             positions.push_back(tab + "=" + std::to_string(static_cast<int>(value)));
                         window->m_settings->setHelpScroll(positions);
                     }),
                     this);

    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), stack);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);

    GtkWidget *headerBar = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(headerBar), switcher);

    GtkWidget *toolbarView = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), headerBar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), GTK_WIDGET(stack));

    adw_dialog_set_child(dialog, toolbarView);
    adw_dialog_present(dialog, GTK_WIDGET(m_window));
}

} // namespace ustudio::app
