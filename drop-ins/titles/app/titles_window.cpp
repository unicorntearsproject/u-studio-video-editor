#include "titles_window.h"
#include "signal_guard.h"

#include "view_settings.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "export.h"
#include "gallery.h"

#include "core/brand_kit.h"
#include "core/export_formats.h"
#include "core/title_xml.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ustudio::titles::app {

namespace {

namespace Log = core::Log;

struct ActionEntry
{
    const char *name;
    const char *accel; // nullptr: none. Never a bare key: typing must not trigger one (audit A1)
};

// Every window action. Single keys (Delete, arrows) belong to the canvas's
// own key controller, so a text field never loses a keystroke to them.
constexpr ActionEntry kActions[] = {
    {"new", "<Control>n"},
    {"new-from-template", "<Control><Shift>n"},
    {"save-as-template", nullptr},
    {"open", "<Control>o"},
    {"save", "<Control>s"},
    {"save-as", "<Control><Shift>s"},
    {"export", "<Control>e"},
    {"undo", "<Control>z"},
    {"redo", "<Control><Shift>z"},
    {"add-text", "<Control>t"},
    {"add-rectangle", nullptr},
    {"add-rounded", "<Control><Shift>r"},
    {"add-ellipse", nullptr},
    {"add-line", nullptr},
    {"add-image", nullptr},
    {"add-animation", nullptr},
    {"backdrop-checkerboard", nullptr},
    {"backdrop-colour", nullptr},
    {"backdrop-image", nullptr},
    {"toggle-guides", "<Control>semicolon"},
};

std::string fileName(const std::string &path)
{
    return core::utf8String(core::pathFromUtf8(path).filename());
}

// A GFile's path as UTF-8 (GLib's filename encoding may differ, ADR-017).
std::string utf8Path(GFile *file)
{
    char *raw = g_file_get_path(file);
    if (!raw)
        return {};
    char *utf8 = g_filename_to_utf8(raw, -1, nullptr, nullptr, nullptr);
    std::string out = utf8 ? utf8 : "";
    g_free(utf8);
    g_free(raw);
    return out;
}

GFile *fileFromUtf8(const std::string &path)
{
    char *raw = g_filename_from_utf8(path.c_str(), -1, nullptr, nullptr, nullptr);
    GFile *file = g_file_new_for_path(raw ? raw : path.c_str());
    g_free(raw);
    return file;
}

// The Export dialog's choices, for its button's handler.
struct ExportChoice
{
    TitlesWindow *window;
    AdwDialog *dialog;
    GtkWidget *format, *seconds;
};

// The brand kit compiled into the app (data/brand.xml).
BrandKit loadBrandKit()
{
    GBytes *bytes = g_resources_lookup_data("/com/ustudio/Titles/brand.xml", G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes)
        return {};
    gsize size = 0;
    const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
    auto kit = parseBrandKit(std::string_view(data, size));
    g_bytes_unref(bytes);
    if (!kit) {
        Log::warn("[titles] the brand kit doesn't read: " + kit.error());
        return {};
    }
    return *kit;
}

GtkFileFilter *titleFilter()
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Titles");
    gtk_file_filter_add_suffix(filter, "ustitle");
    return filter;
}

} // namespace

TitlesWindow::TitlesWindow(GtkApplication *app, const std::string &path, const std::string &backdrop)
{
    m_window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    m_cancellable = g_cancellable_new();
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1440, 900);
    buildUi();

    for (const ActionEntry &entry : kActions) {
        GSimpleAction *action = entry.name == std::string("toggle-guides")
                                    ? g_simple_action_new_stateful(entry.name, nullptr, g_variant_new_boolean(TRUE))
                                    : g_simple_action_new(entry.name, nullptr);
        g_signal_connect(action, "activate", G_CALLBACK(onAction), this);
        g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(action));
        g_object_unref(action);
        if (entry.accel) {
            const std::string detailed = std::string("win.") + entry.name;
            const char *accels[] = {entry.accel, nullptr};
            gtk_application_set_accels_for_action(app, detailed.c_str(), accels);
        }
    }
    g_signal_connect(m_window, "close-request", G_CALLBACK(onCloseRequest), this);
    g_signal_connect(m_window, "destroy", G_CALLBACK(onDestroy), this);

    const ViewSettings view = loadViewSettings();
    m_canvas->setGuidesVisible(view.guides);
    if (view.backdrop == Backdrop::Colour)
        m_canvas->setBackdropColour(view.colour);
    if (!backdrop.empty()) {
        GFile *file = fileFromUtf8(backdrop);
        GError *error = nullptr;
        GdkTexture *texture = gdk_texture_new_from_file(file, &error);
        g_object_unref(file);
        if (texture) {
            m_canvas->setBackdropImage(texture);
            g_object_unref(texture);
        } else {
            Log::warn("[titles] backdrop " + backdrop + " doesn't load: " + (error ? error->message : "?"));
            g_clear_error(&error);
        }
    }

    if (!path.empty())
        open(path);
    refresh();
    // Where nothing is still coming in or going out.
    setFrame(static_cast<double>(m_history.document().timing.intro));
    // The canvas has focus, not the layers list (a focused row in a
    // single-selection list selects itself).
    gtk_window_set_focus(GTK_WINDOW(m_window), m_canvas->widget());
}

TitlesWindow::~TitlesWindow()
{
    // Called from "destroy", before GTK disposes the widgets: nothing in the
    // window may call back into this object after it (the text editor's
    // focus-leave, the banner, the play button; the parts do their own).
    disconnectFromTree(GTK_WIDGET(m_window), this);
    stopPlaying();
    g_cancellable_cancel(m_cancellable);
    g_object_unref(m_cancellable);
    if (m_finishIdle)
        g_source_remove(m_finishIdle);
}

void TitlesWindow::buildUi()
{
    m_canvas = std::make_unique<TitleCanvas>(TitleCanvas::Callbacks{
        [this](const std::optional<std::string> &id) { selectionChanged(id); },
        [this](const std::string &id, double x, double y, bool final) { moveLayerTo(id, x, y, final); },
        [this](const std::string &id, const Rect &box, bool final) { resizeLayer(id, box, final); },
        [this](const std::string &id) { deleteLayer(id); },
        [this](const std::string &id) { editTextOnCanvas(id); },
        [this] { togglePlay(); },
    });
    m_strip = std::make_unique<AnimationStrip>(AnimationStrip::Callbacks{
        [this](double frame) {
            stopPlaying();
            setFrame(frame);
        },
        [this](const Timing &timing, bool final) {
            edit(
                "Change Timing",
                [&](TitleDocument &doc) {
                    doc.timing = timing;
                    return true;
                },
                "timing");
            if (final)
                m_history.closeStep();
        },
        [this](const std::string &id) { m_canvas->setSelection(id); },
    });

    GtkWidget *header = adw_header_bar_new();
    m_title = ADW_WINDOW_TITLE(adw_window_title_new("", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(m_title));

    const auto button = [](const char *icon, const char *action, const char *tooltip) {
        GtkWidget *b = gtk_button_new_from_icon_name(icon);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
        gtk_widget_set_tooltip_text(b, tooltip);
        return b;
    };
    adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                              button("document-open-symbolic", "win.open", "Open a title (Ctrl+O)"));
    GMenu *add = g_menu_new();
    g_menu_append(add, "Text", "win.add-text");
    g_menu_append(add, "Rectangle", "win.add-rectangle");
    g_menu_append(add, "Rounded Rectangle", "win.add-rounded");
    g_menu_append(add, "Ellipse", "win.add-ellipse");
    g_menu_append(add, "Line", "win.add-line");
    g_menu_append(add, "Picture…", "win.add-image");
    g_menu_append(add, "Animation…", "win.add-animation");
    GtkWidget *addButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(addButton), "list-add-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(addButton), G_MENU_MODEL(add));
    gtk_widget_set_tooltip_text(addButton, "Add a layer");
    g_object_unref(add);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), addButton);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), button("edit-undo-symbolic", "win.undo", "Undo (Ctrl+Z)"));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), button("edit-redo-symbolic", "win.redo", "Redo (Ctrl+Shift+Z)"));

    GMenu *menu = g_menu_new();
    GMenu *file = g_menu_new();
    g_menu_append(file, "New Title", "win.new");
    g_menu_append(file, "New from Template…", "win.new-from-template");
    g_menu_append(file, "Save As…", "win.save-as");
    g_menu_append(file, "Save as Template…", "win.save-as-template");
    g_menu_append(file, "Export…", "win.export");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(file));
    GMenu *view = g_menu_new();
    g_menu_append(view, "Show Safe Areas", "win.toggle-guides");
    g_menu_append(view, "Checkerboard Background", "win.backdrop-checkerboard");
    g_menu_append(view, "Background Colour…", "win.backdrop-colour");
    g_menu_append(view, "Background Picture…", "win.backdrop-image");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));
    GtkWidget *menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menuButton), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(menuButton, "Main menu");
    g_object_unref(file);
    g_object_unref(view);
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menuButton);
    GtkWidget *saveButton = button("document-save-symbolic", "win.save", "Save (Ctrl+S)");
    gtk_widget_add_css_class(saveButton, "suggested-action");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveButton);

    m_layers = std::make_unique<LayersPanel>(LayersPanel::Callbacks{
        [this](const std::optional<std::string> &id) { m_canvas->setSelection(id); },
        [this](const std::string &id, bool visible) {
            edit(visible ? "Show Layer" : "Hide Layer",
                 [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.visible = visible; }); });
        },
        [this](const std::string &id, bool locked) {
            edit(locked ? "Lock Layer" : "Unlock Layer",
                 [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.locked = locked; }); });
        },
        [this](const std::string &id, size_t index) {
            edit("Restack Layer", [&](TitleDocument &doc) { return moveLayer(doc, id, index); });
        },
        [this](const std::string &id) { deleteLayer(id); },
    });
    m_inspector = std::make_unique<Inspector>(
        Inspector::Callbacks{
            [this](const std::string &label, const std::string &id, const std::function<void(Layer &)> &change,
                   const std::string &mergeKey) {
                m_editFromInspector = true;
                edit(label, [&](TitleDocument &doc) { return updateLayer(doc, id, change); }, mergeKey);
                m_editFromInspector = false;
            },
            [this](const std::string &label, const std::function<void(TitleDocument &)> &change,
                   const std::string &mergeKey) {
                m_editFromInspector = true;
                edit(
                    label,
                    [&](TitleDocument &doc) {
                        change(doc);
                        return true;
                    },
                    mergeKey);
                m_editFromInspector = false;
            },
            [this] {
                if (!edit("Apply Brand", [&](TitleDocument &doc) { return applyBrand(doc, m_inspector->kit()); }))
                    toast("Already in the brand");
            },
            [this] { m_inspector->show(m_history.document(), m_canvas->selection(), m_canvas->frame()); },
            [this](const std::string &id) { choosePicture(id); },
        },
        loadBrandKit());

    m_toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    GtkWidget *panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(panes), m_layers->widget());
    gtk_box_append(GTK_BOX(panes), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    // The canvas under an overlay: the text box for typing on it goes there.
    m_canvasOverlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(m_canvasOverlay), m_canvas->widget());
    gtk_widget_set_hexpand(m_canvasOverlay, TRUE);
    gtk_widget_set_vexpand(m_canvasOverlay, TRUE);
    // Under it: play and where the playhead is, then the animation strip.
    GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(transport, 8);
    gtk_widget_set_margin_top(transport, 4);
    gtk_widget_set_margin_bottom(transport, 4);
    m_playButton = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_add_css_class(m_playButton, "flat");
    gtk_widget_set_tooltip_text(m_playButton, "Play the intro, two seconds of the hold and the outro, again (Space)");
    g_signal_connect_swapped(m_playButton, "clicked",
                             G_CALLBACK(+[](gpointer self) { static_cast<TitlesWindow *>(self)->togglePlay(); }), this);
    gtk_box_append(GTK_BOX(transport), m_playButton);
    m_timeLabel = gtk_label_new("");
    gtk_widget_add_css_class(m_timeLabel, "dim-label");
    gtk_widget_add_css_class(m_timeLabel, "numeric");
    gtk_box_append(GTK_BOX(transport), m_timeLabel);
    GtkWidget *centre = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(centre, TRUE);
    gtk_box_append(GTK_BOX(centre), m_canvasOverlay);
    gtk_box_append(GTK_BOX(centre), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(centre), transport);
    GtkWidget *stripScroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(stripScroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(stripScroller), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(stripScroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(stripScroller), m_strip->widget());
    gtk_box_append(GTK_BOX(centre), stripScroller);
    gtk_box_append(GTK_BOX(panes), centre);
    gtk_box_append(GTK_BOX(panes), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(panes), m_inspector->widget());
    adw_toast_overlay_set_child(m_toasts, panes);
    GtkWidget *view_ = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view_), header);
    m_templateBanner = ADW_BANNER(adw_banner_new(""));
    adw_banner_set_button_label(m_templateBanner, "Update");
    g_signal_connect(m_templateBanner, "button-clicked", G_CALLBACK(&TitlesWindow::onTemplateBannerClicked), this);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view_), GTK_WIDGET(m_templateBanner));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view_), GTK_WIDGET(m_toasts));
    adw_application_window_set_content(m_window, view_);
}

void TitlesWindow::selectionChanged(const std::optional<std::string> &id)
{
    const TitleDocument &doc = m_history.document();
    m_layers->show(doc, id);
    m_inspector->show(doc, id, m_canvas->frame());
    m_strip->show(doc, id, m_canvas->frame());
}

void TitlesWindow::setFrame(double titleFrame)
{
    const TitleDocument &doc = m_history.document();
    m_canvas->setFrame(titleFrame);
    m_strip->show(doc, m_canvas->selection(), titleFrame);
    const double fps = static_cast<double>(doc.fpsNum) / std::max(1, doc.fpsDen);
    const auto frame = static_cast<int64_t>(titleFrame);
    const char *zone = frame < doc.timing.intro                     ? "Intro"
                       : frame < doc.timing.intro + doc.timing.hold ? "Hold"
                                                                    : "Outro";
    char text[64];
    std::snprintf(text, sizeof text, "%s · frame %lld · %.2f s", zone, static_cast<long long>(frame), titleFrame / fps);
    gtk_label_set_text(GTK_LABEL(m_timeLabel), text);
    if (!m_playTick) // while playing, the inspector keeps still
        m_inspector->show(doc, m_canvas->selection(), titleFrame);
}

void TitlesWindow::togglePlay()
{
    if (m_playTick) {
        stopPlaying();
        return;
    }
    m_playStart = 0;
    m_playTick = gtk_widget_add_tick_callback(m_canvas->widget(), &onPlayTick, this, nullptr);
    gtk_button_set_icon_name(GTK_BUTTON(m_playButton), "media-playback-pause-symbolic");
}

void TitlesWindow::stopPlaying()
{
    if (!m_playTick)
        return;
    gtk_widget_remove_tick_callback(m_canvas->widget(), m_playTick);
    m_playTick = 0;
    gtk_button_set_icon_name(GTK_BUTTON(m_playButton), "media-playback-start-symbolic");
    m_inspector->show(m_history.document(), m_canvas->selection(), m_canvas->frame());
}

void TitlesWindow::refresh()
{
    const TitleDocument &doc = m_history.document();
    m_canvas->setDocument(doc); // may clear a selection whose layer is gone (selectionChanged)
    // The playhead stays inside the title.
    const double end = static_cast<double>(doc.timing.length());
    if (m_canvas->frame() > end)
        m_canvas->setFrame(end);
    m_layers->show(doc, m_canvas->selection());
    m_strip->show(doc, m_canvas->selection(), m_canvas->frame());
    if (!m_editFromInspector)
        m_inspector->show(doc, m_canvas->selection(), m_canvas->frame());
    const std::string name = m_path.empty() ? "Untitled Title" : fileName(m_path);
    const std::string title = (m_history.isDirty() ? "• " : "") + name;
    adw_window_title_set_title(m_title, title.c_str());
    const std::string subtitle = std::to_string(doc.width) + "×" + std::to_string(doc.height) + " · " +
                                 std::to_string(doc.fpsNum / std::max(1, doc.fpsDen)) + " fps";
    adw_window_title_set_subtitle(m_title, subtitle.c_str());
    gtk_window_set_title(GTK_WINDOW(m_window), (name + " – U-Stu Titles").c_str());
    const auto enable = [this](const char *action, bool on) {
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_window), action)), on);
    };
    if (g_action_map_lookup_action(G_ACTION_MAP(m_window), "undo")) {
        enable("undo", m_history.canUndo());
        enable("redo", m_history.canRedo());
    }
    checkTemplate();
}

void TitlesWindow::checkTemplate()
{
    // Reads the template's file: only when the title's reference or
    // revision changed (opened, updated, undone), not on every refresh.
    const TitleDocument &doc = m_history.document();
    const std::string key = doc.templateRef + "@" + doc.templateRevision;
    if (!m_templateBanner || key == m_templateChecked)
        return;
    m_templateChecked = key;
    m_changedTemplate = changedTemplate(doc, builtInTemplatesDir(), userTemplatesDir());
    if (m_changedTemplate) {
        const std::string text = "Its template “" + m_changedTemplate->name + "” has changed";
        adw_banner_set_title(m_templateBanner, text.c_str());
    }
    adw_banner_set_revealed(m_templateBanner, m_changedTemplate.has_value());
}

void TitlesWindow::updateFromTemplate()
{
    if (!m_changedTemplate)
        return;
    auto update = titles::updateFromTemplate(m_history.document(), *m_changedTemplate, m_path);
    if (!update) {
        toast("Couldn't update from “" + m_changedTemplate->name + "”: " + update.error());
        return;
    }
    const std::string name = m_changedTemplate->name;
    edit("Update from Template", [&](TitleDocument &title) {
        const std::string folder = title.baseDirectory;
        title = std::move(update->document);
        if (!m_path.empty())
            title.baseDirectory = folder;
        return true;
    });
    m_canvas->setSelection(std::nullopt);
    refresh();
    std::string dropped;
    for (const std::string &field : update->droppedFields)
        dropped += (dropped.empty() ? "" : ", ") + field;
    toast(dropped.empty() ? "Updated from “" + name + "”; your field text is kept"
                          : "Updated from “" + name + "”; it no longer has: " + dropped);
}

void TitlesWindow::toast(const std::string &text)
{
    AdwToast *t = adw_toast_new(text.c_str());
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(m_toasts, t);
    Log::info("[titles] " + text);
}

bool TitlesWindow::edit(const std::string &label, const std::function<bool(TitleDocument &)> &change,
                        const std::string &mergeKey)
{
    if (!m_history.apply(label, change, mergeKey))
        return false;
    refresh();
    return true;
}

void TitlesWindow::open(const std::string &path)
{
    auto read = readTitle(path);
    if (!read) {
        toast("Couldn't open " + fileName(path) + ": " + read.error());
        return;
    }
    m_history.reset(std::move(read->document));
    m_path = path;
    m_canvas->setSelection(std::nullopt);
    refresh();
    setFrame(static_cast<double>(m_history.document().timing.intro));
    for (const std::string &warning : read->warnings)
        toast(warning);
}

void TitlesWindow::showTemplates()
{
    GalleryCallbacks callbacks;
    callbacks.use = [this](const TemplateInfo &info) { useTemplate(info); };
    callbacks.edit = [this](const TemplateInfo &info) {
        auto *other = new TitlesWindow(gtk_window_get_application(window()), info.path, {});
        gtk_window_present(other->window());
    };
    callbacks.toast = [this](const std::string &text) { toast(text); };
    showGallery(GTK_WIDGET(m_window), std::move(callbacks));
}

void TitlesWindow::useTemplate(const TemplateInfo &info)
{
    if (!m_history.document().layers.empty()) {
        auto *other = new TitlesWindow(gtk_window_get_application(window()), {}, {});
        gtk_window_present(other->window());
        other->useTemplate(info);
        return;
    }
    auto doc = templateDocument(info, m_path);
    if (!doc) {
        toast("Couldn't use “" + info.name + "”: " + doc.error());
        return;
    }
    edit("Use Template", [&](TitleDocument &title) {
        const std::string folder = title.baseDirectory;
        title = std::move(*doc);
        if (!m_path.empty())
            title.baseDirectory = folder;
        return true;
    });
    m_canvas->setSelection(std::nullopt);
    refresh();
    setFrame(static_cast<double>(m_history.document().timing.intro) +
             std::min(45.0, static_cast<double>(m_history.document().timing.hold) / 2.0));
}

void TitlesWindow::saveAsTemplate()
{
    const TitleDocument &doc = m_history.document();
    std::string name = doc.name;
    if (name.empty() && !m_path.empty())
        name = core::utf8String(core::pathFromUtf8(m_path).stem());
    if (name.empty())
        name = "My template";
    askTemplateName(GTK_WIDGET(m_window), "Save as Template", name, [this](const std::string &chosen) {
        auto saved = saveTemplate(m_history.document(), userTemplatesDir(), chosen);
        toast(saved ? "Saved “" + chosen + "” to My Templates" : "Couldn't save the template: " + saved.error());
    });
}

void TitlesWindow::save(const std::string &path)
{
    // Pictures stored relative to the old folder keep pointing at the same
    // files from the new one.
    const std::string folder = core::utf8String(core::pathFromUtf8(path).parent_path());
    const std::string oldFolder = m_history.document().baseDirectory;
    if (!oldFolder.empty() && oldFolder != folder) {
        m_history.apply("Save As", [&](TitleDocument &doc) {
            for (Layer &layer : doc.layers) {
                if (!drawnFromFile(layer))
                    continue;
                const std::filesystem::path src = core::pathFromUtf8(layer.src);
                if (src.is_absolute())
                    continue;
                const std::filesystem::path absolute = core::pathFromUtf8(oldFolder) / src;
                layer.src = core::utf8String(absolute.lexically_relative(core::pathFromUtf8(folder)));
            }
            return true;
        });
    }
    m_history.setBaseDirectory(folder);
    const std::string error = saveTitle(m_history.document(), path);
    if (!error.empty()) {
        toast("Couldn't save: " + error);
        return;
    }
    m_path = path;
    m_history.markSaved();
    refresh();
    toast("Saved " + fileName(path));
}

void TitlesWindow::chooseSavePath()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Title");
    gtk_file_dialog_set_initial_name(dialog, m_path.empty() ? "Title.ustitle" : fileName(m_path).c_str());
    // The title's own folder, else Videos (else home): never wherever the
    // process happened to start.
    GFile *folder = nullptr;
    if (!m_path.empty()) {
        GFile *file = fileFromUtf8(m_path);
        folder = g_file_get_parent(file);
        g_object_unref(file);
    } else {
        const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        folder = g_file_new_for_path(videos ? videos : g_get_home_dir());
    }
    if (folder) {
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    GtkFileFilter *filter = titleFilter();
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_save(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            std::string path = utf8Path(file);
            g_object_unref(file);
            if (!isTitleFile(path))
                path += ".ustitle";
            static_cast<TitlesWindow *>(self)->save(path);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::chooseOpenPath()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Title");
    GtkFileFilter *filter = titleFilter();
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            const std::string path = utf8Path(file);
            g_object_unref(file);
            auto *window = static_cast<TitlesWindow *>(self);
            if (window->m_history.isDirty() || !window->m_path.empty()) {
                // Unsaved or another file here: its own window.
                auto *other = new TitlesWindow(gtk_window_get_application(window->window()), path, {});
                gtk_window_present(other->window());
            } else {
                window->open(path);
            }
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::chooseBackdropColour()
{
    GtkColorDialog *dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_title(dialog, "Background Colour");
    gtk_color_dialog_set_with_alpha(dialog, FALSE);
    const GdkRGBA initial = loadViewSettings().colour;
    gtk_color_dialog_choose_rgba(
        dialog, GTK_WINDOW(m_window), &initial, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GdkRGBA *colour = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), result, nullptr);
            if (!colour)
                return;
            auto *window = static_cast<TitlesWindow *>(self);
            window->m_canvas->setBackdropColour(*colour);
            ViewSettings view = loadViewSettings();
            view.backdrop = Backdrop::Colour;
            view.colour = *colour;
            saveViewSettings(view);
            gdk_rgba_free(colour);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::chooseBackdropImage()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Background Picture");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Pictures");
    for (const char *type : {"image/png", "image/jpeg", "image/webp"})
        gtk_file_filter_add_mime_type(filter, type);
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            auto *window = static_cast<TitlesWindow *>(self);
            GError *error = nullptr;
            GdkTexture *texture = gdk_texture_new_from_file(file, &error);
            g_object_unref(file);
            if (!texture) {
                window->toast(std::string("Couldn't load that picture: ") + (error ? error->message : "?"));
                g_clear_error(&error);
                return;
            }
            window->m_canvas->setBackdropImage(texture);
            g_object_unref(texture);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::addLayerOf(Layer layer, const std::string &label)
{
    const std::string id = layer.id;
    if (edit(label, [&](TitleDocument &doc) { return addLayer(doc, layer); }))
        m_canvas->setSelection(id);
}

void TitlesWindow::showExportDialog()
{
    // The render tool reads the file: an unsaved title is saved first.
    if (m_path.empty()) {
        toast("Save the title first, then export it");
        chooseSavePath();
        return;
    }
    if (m_history.isDirty())
        save(m_path);
    if (m_history.isDirty())
        return; // the save failed; its toast says why

    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Export Title");
    adw_dialog_set_content_width(dialog, 420);
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          "For OBS and other apps: with a transparent background (alpha), or "
                                          "flattened onto the title's own background.");
    GtkStringList *labels = gtk_string_list_new(nullptr);
    for (const ExportFormat &format : exportFormats())
        gtk_string_list_append(labels, format.label);
    GtkWidget *format = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(format), "Format");
    adw_combo_row_set_model(ADW_COMBO_ROW(format), G_LIST_MODEL(labels));
    g_object_unref(labels);
    const TitleDocument &doc = m_history.document();
    const double designed = static_cast<double>(std::max<int64_t>(1, doc.timing.length())) * doc.fpsDen / doc.fpsNum;
    // An action row with a spin button, not an AdwSpinRow, which isn't in
    // the AT-SPI tree (libadwaita 1.9.2; notes/gtk-upstream.md).
    GtkWidget *lengthRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lengthRow), "Length (seconds)");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(lengthRow), "The hold stretches; intro and outro keep their timing");
    GtkWidget *seconds = gtk_spin_button_new_with_range(0.1, 3600, 0.5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(seconds), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(seconds), designed);
    gtk_widget_set_valign(seconds, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(seconds), GTK_ACCESSIBLE_PROPERTY_LABEL, "Length (seconds)", -1);
    adw_action_row_add_suffix(ADW_ACTION_ROW(lengthRow), seconds);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(lengthRow), seconds);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), format);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), lengthRow);
    GtkWidget *go = gtk_button_new_with_label("Export…");
    gtk_widget_add_css_class(go, "suggested-action");
    gtk_widget_add_css_class(go, "pill");
    gtk_widget_set_halign(go, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(go, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), go);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(dialog, view);
    g_signal_connect_data(
        go, "clicked", G_CALLBACK(onExportChosen), new ExportChoice{this, dialog, format, seconds},
        [](gpointer data, GClosure *) { delete static_cast<ExportChoice *>(data); }, G_CONNECT_DEFAULT);
    adw_dialog_present(dialog, GTK_WIDGET(m_window));
}

void TitlesWindow::chooseExportPath(const std::string &formatName, double seconds)
{
    const ExportFormat *format = exportFormat(formatName);
    if (!format)
        return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, format->extension[0] ? "Export Title" : "Export Title (a folder of PNGs)");
    const std::filesystem::path title = core::pathFromUtf8(m_path);
    std::string name = core::utf8String(title.stem());
    if (format->extension[0])
        name += std::string(".") + format->extension;
    gtk_file_dialog_set_initial_name(dialog, name.c_str());
    GFile *folder = fileFromUtf8(core::utf8String(title.parent_path()));
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_object_unref(folder);
    struct Request
    {
        TitlesWindow *window;
        std::string format;
        double seconds;
    };
    gtk_file_dialog_save(
        dialog, GTK_WINDOW(m_window), m_cancellable,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Request> request(static_cast<Request *>(data));
            GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            const std::string path = utf8Path(file);
            g_object_unref(file);
            TitlesWindow *window = request->window;
            AdwToast *progress = adw_toast_new(("Exporting " + fileName(path) + "…").c_str());
            adw_toast_set_timeout(progress, 0);
            adw_toast_overlay_add_toast(window->m_toasts, ADW_TOAST(g_object_ref(progress)));
            exportTitle(window->m_path, path, request->format, request->seconds, window->m_cancellable,
                        [window, progress, path](const std::string &error) {
                            adw_toast_dismiss(progress);
                            g_object_unref(progress);
                            window->toast(error.empty() ? "Exported " + fileName(path) : "Couldn't export: " + error);
                        });
        },
        new Request{this, formatName, seconds});
    g_object_unref(dialog);
}

std::string TitlesWindow::pictureSource(const std::string &path) const
{
    // Relative when it's in (or under) the title's folder, so the two move
    // together; absolute otherwise, and for a title not yet saved.
    const std::string &folder = m_history.document().baseDirectory;
    if (!folder.empty()) {
        const std::filesystem::path relative = core::pathFromUtf8(path).lexically_relative(core::pathFromUtf8(folder));
        const std::string text = core::utf8String(relative);
        if (!relative.empty() && !text.starts_with(".."))
            return text;
    }
    return path;
}

void TitlesWindow::choosePicture(const std::optional<std::string> &replaceId)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, replaceId ? "Replace Picture" : "Add Picture");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PNG pictures");
    gtk_file_filter_add_suffix(filter, "png");
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    struct Request
    {
        TitlesWindow *window;
        std::optional<std::string> replaceId;
    };
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Request> request(static_cast<Request *>(data));
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            const std::string path = utf8Path(file);
            g_object_unref(file);
            TitlesWindow *window = request->window;
            // Its size, and whether it reads at all (PNG only, doc 16).
            cairo_surface_t *png = cairo_image_surface_create_from_png(path.c_str());
            const bool ok = cairo_surface_status(png) == CAIRO_STATUS_SUCCESS;
            const int width = ok ? cairo_image_surface_get_width(png) : 0;
            cairo_surface_destroy(png);
            if (!ok) {
                window->toast("That isn't a PNG picture that reads");
                return;
            }
            const std::string src = window->pictureSource(path);
            if (request->replaceId) {
                const std::string id = *request->replaceId;
                window->edit("Replace Picture",
                             [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.src = src; }); });
                return;
            }
            const TitleDocument &doc = window->m_history.document();
            Layer layer;
            layer.id = uniqueLayerId(doc, "picture");
            layer.kind = LayerKind::Image;
            layer.src = src;
            // At its own size, or half the canvas wide if it's bigger.
            if (width > doc.width / 2)
                layer.w = doc.width / 2.0;
            layer.x = doc.width / 4.0;
            layer.y = doc.height / 4.0;
            window->addLayerOf(layer, "Add Picture");
        },
        new Request{this, replaceId});
    g_object_unref(dialog);
}

void TitlesWindow::chooseAnimation()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Add Animation");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Lottie animations (.json)");
    gtk_file_filter_add_suffix(filter, "json");
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), m_cancellable,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return; // cancelled, or the window is gone
            auto *window = static_cast<TitlesWindow *>(data);
            const std::string path = utf8Path(file);
            g_object_unref(file);
            // Read and checked on a worker: up to 8 MB of untrusted JSON
            // (ADR-021 decision 3) is too long for the main loop.
            struct Job
            {
                std::string path;
                std::expected<lottie::Facts, std::string> facts = std::unexpected(std::string());
            };
            GTask *task = g_task_new(
                nullptr, window->m_cancellable,
                [](GObject *, GAsyncResult *res, gpointer self) {
                    GTask *done = G_TASK(res);
                    if (g_cancellable_is_cancelled(g_task_get_cancellable(done)))
                        return; // the window closed meanwhile
                    auto *job = static_cast<Job *>(g_task_get_task_data(done));
                    auto *w = static_cast<TitlesWindow *>(self);
                    if (!job->facts)
                        w->toast("Couldn't add the animation: " + job->facts.error());
                    else
                        w->addAnimation(job->path, *job->facts);
                },
                window);
            g_task_set_task_data(task, new Job{path}, [](gpointer job) { delete static_cast<Job *>(job); });
            g_task_run_in_thread(task, [](GTask *, gpointer, gpointer jobData, GCancellable *) {
                auto *job = static_cast<Job *>(jobData);
                std::error_code ec;
                const auto size = std::filesystem::file_size(core::pathFromUtf8(job->path), ec);
                if (ec) {
                    job->facts = std::unexpected(std::string("it doesn't read"));
                    return;
                }
                if (size > lottie::kMaxBytes) {
                    job->facts = std::unexpected("it's over " + std::to_string(lottie::kMaxBytes >> 20) + " MB");
                    return;
                }
                std::ifstream in(core::pathFromUtf8(job->path), std::ios::binary);
                const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                job->facts = lottie::check(json);
            });
            g_object_unref(task);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::addAnimation(const std::string &path, const lottie::Facts &facts)
{
    addLayerOf(makeAnimationLayer(m_history.document(), pictureSource(path), facts.width, facts.height),
               "Add Animation");
    if (facts.hasText)
        toast("Its text is drawn in your system's fonts, which may differ from the file's");
}

void TitlesWindow::editTextOnCanvas(const std::string &id)
{
    finishTextEdit(true);
    const TitleDocument &doc = m_history.document();
    const std::optional<size_t> index = layerIndex(doc, id);
    if (!index || doc.layers[*index].kind != LayerKind::Text)
        return;
    const Layer &layer = doc.layers[*index];
    const std::vector<LayerGeometry> geometry = measureLayers(doc, m_canvas->frame(), {});
    const Rect box = geometry[*index].box;
    const TitleCanvas::Mapping map = m_canvas->mapping();

    m_textEditId = id;
    m_textEditor = gtk_text_view_new();
    gtk_widget_add_css_class(m_textEditor, "title-text-editor");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_textEditor),
                                layer.fit == Fit::Wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    static constexpr GtkJustification kJustify[] = {GTK_JUSTIFY_LEFT, GTK_JUSTIFY_CENTER, GTK_JUSTIFY_RIGHT};
    gtk_text_view_set_justification(GTK_TEXT_VIEW(m_textEditor), kJustify[static_cast<size_t>(layer.align)]);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(m_textEditor));
    gtk_text_buffer_set_text(buffer, layer.text.c_str(), -1);
    // The layer's font at the canvas's scale (a tag: no per-widget CSS).
    // GTK sizes text in points at 96 dpi: 0.75 pt a pixel.
    GtkTextTag *look =
        gtk_text_buffer_create_tag(buffer, "look", "family", layer.font.family.c_str(), "weight", layer.font.weight,
                                   "size-points", std::max(4.0, layer.font.size * map.scale * 0.75), nullptr);
    (void)look;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_apply_tag_by_name(buffer, "look", &start, &end);
    // Keep typed text in the same look.
    g_signal_connect(buffer, "changed", G_CALLBACK(onTextEditorChanged), nullptr);

    gtk_widget_set_halign(m_textEditor, GTK_ALIGN_START);
    gtk_widget_set_valign(m_textEditor, GTK_ALIGN_START);
    gtk_widget_set_margin_start(m_textEditor, static_cast<int>(std::lround(map.x + box.x * map.scale)));
    gtk_widget_set_margin_top(m_textEditor, static_cast<int>(std::lround(map.y + box.y * map.scale)));
    gtk_widget_set_size_request(m_textEditor, std::max(120, static_cast<int>(std::lround(box.w * map.scale)) + 24),
                                std::max(24, static_cast<int>(std::lround(box.h * map.scale))));
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onTextEditorKey), this);
    gtk_widget_add_controller(m_textEditor, keys);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(onTextEditorFocusLeave), this);
    gtk_widget_add_controller(m_textEditor, focus);
    gtk_overlay_add_overlay(GTK_OVERLAY(m_canvasOverlay), m_textEditor);

    // The canvas shows the title without this layer while you type over it.
    TitleDocument shown = doc;
    shown.layers[*index].visible = false;
    m_canvas->setDocument(shown);
    gtk_widget_grab_focus(m_textEditor);
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_select_range(buffer, &end, &start);
}

void TitlesWindow::finishTextEdit(bool commit)
{
    if (!m_textEditor)
        return;
    GtkWidget *editor = m_textEditor;
    m_textEditor = nullptr; // re-entry (focus leaves as it's removed)
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    const std::string text = raw;
    g_free(raw);
    gtk_overlay_remove_overlay(GTK_OVERLAY(m_canvasOverlay), editor);
    const std::string id = m_textEditId;
    m_textEditId.clear();
    if (!commit ||
        !edit("Edit Text", [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.text = text; }); }))
        refresh(); // shows the layer again
    gtk_widget_grab_focus(m_canvas->widget());
}

bool TitlesWindow::confirmClose()
{
    if (!m_history.isDirty() || m_closing)
        return true;
    AdwDialog *dialog = adw_alert_dialog_new("Save changes?", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "“%s” has changes that aren't saved.",
                                 m_path.empty() ? "Untitled Title" : fileName(m_path).c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "discard", "_Discard", "save",
                                   "_Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            auto *window = static_cast<TitlesWindow *>(self);
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response == "cancel")
                return;
            if (response == "save") {
                if (window->m_path.empty()) {
                    window->chooseSavePath();
                    return; // closes on the next attempt, once saved
                }
                window->save(window->m_path);
                if (window->m_history.isDirty())
                    return; // the save failed; the toast says why
            }
            window->m_closing = true;
            gtk_window_close(window->window());
        },
        this);
    return false;
}

void TitlesWindow::moveLayerTo(const std::string &id, double x, double y, bool final)
{
    // By how much the layer's box moves; its position and any position
    // keyframes move with it.
    const std::vector<LayerGeometry> geometry = measureLayers(m_history.document(), m_canvas->frame(), {});
    auto it = std::find_if(geometry.begin(), geometry.end(), [&](const LayerGeometry &g) { return g.id == id; });
    if (it == geometry.end())
        return;
    const double dx = x - it->box.x, dy = y - it->box.y;
    edit(
        "Move Layer",
        [&](TitleDocument &doc) {
            return updateLayer(doc, id, [&](Layer &layer) {
                layer.x += dx;
                layer.y += dy;
                for (PropertyTrack &track : layer.animation)
                    for (TitleKey &key : track.keys) {
                        if (track.property == Property::X)
                            key.key.value += dx;
                        else if (track.property == Property::Y)
                            key.key.value += dy;
                    }
            });
        },
        "move:" + id);
    if (final)
        m_history.closeStep();
}

void TitlesWindow::resizeLayer(const std::string &id, const Rect &box, bool final)
{
    edit(
        "Resize Layer",
        [&](TitleDocument &doc) {
            return updateLayer(doc, id, [&](Layer &layer) {
                layer.x = box.x;
                layer.w = box.w;
                // A text layer's height follows its text unless it had a box.
                if (layer.kind == LayerKind::Shape || layer.h > 0.0) {
                    layer.y = box.y;
                    layer.h = box.h;
                }
            });
        },
        "resize:" + id);
    if (final)
        m_history.closeStep();
}

void TitlesWindow::deleteLayer(const std::string &id)
{
    edit("Delete Layer", [&](TitleDocument &doc) { return removeLayer(doc, id); });
}

// --- GTK trampolines ---------------------------------------------------------

gboolean TitlesWindow::onCloseRequest(GtkWindow *, gpointer self)
{
    return static_cast<TitlesWindow *>(self)->confirmClose() ? FALSE : TRUE;
}

gboolean TitlesWindow::onPlayTick(GtkWidget *, GdkFrameClock *clock, gpointer self)
{
    auto *window = static_cast<TitlesWindow *>(self);
    const gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (window->m_playStart == 0)
        window->m_playStart = now;
    const TitleDocument &doc = window->m_history.document();
    const double fps = static_cast<double>(doc.fpsNum) / std::max(1, doc.fpsDen);
    // Intro, then at most two seconds of the hold, then the outro.
    const auto intro = static_cast<double>(doc.timing.intro);
    const double hold = std::min(static_cast<double>(doc.timing.hold), 2.0 * fps);
    const auto outro = static_cast<double>(doc.timing.outro);
    const double cycle = std::max(1.0, intro + hold + outro);
    const double played = std::fmod(static_cast<double>(now - window->m_playStart) / 1e6 * fps, cycle);
    double frame = played;
    if (played >= intro + hold)
        frame = intro + static_cast<double>(doc.timing.hold) + (played - intro - hold);
    window->setFrame(std::floor(frame));
    return G_SOURCE_CONTINUE;
}

gboolean TitlesWindow::onTextEditorKey(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state,
                                       gpointer self)
{
    auto *window = static_cast<TitlesWindow *>(self);
    if (keyval == GDK_KEY_Escape) {
        window->finishTextEdit(false);
        return TRUE;
    }
    // Enter commits; Shift+Enter is a new line.
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !(state & GDK_SHIFT_MASK)) {
        window->finishTextEdit(true);
        return TRUE;
    }
    return FALSE;
}

void TitlesWindow::onTextEditorChanged(GtkTextBuffer *buffer, gpointer)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_apply_tag_by_name(buffer, "look", &start, &end);
}

void TitlesWindow::onTextEditorFocusLeave(GtkEventControllerFocus *, gpointer self)
{
    // From an idle: the focus change is still being delivered to it.
    auto *window = static_cast<TitlesWindow *>(self);
    if (!window->m_finishIdle)
        window->m_finishIdle = g_idle_add(&onFinishTextEdit, window);
}

gboolean TitlesWindow::onFinishTextEdit(gpointer self)
{
    auto *window = static_cast<TitlesWindow *>(self);
    window->m_finishIdle = 0;
    window->finishTextEdit(true);
    return G_SOURCE_REMOVE;
}

void TitlesWindow::onExportChosen(GtkButton *, gpointer data)
{
    auto *choice = static_cast<ExportChoice *>(data);
    const guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(choice->format));
    const double seconds = gtk_spin_button_get_value(GTK_SPIN_BUTTON(choice->seconds));
    TitlesWindow *window = choice->window;
    const std::vector<ExportFormat> &formats = exportFormats();
    const std::string format = index < formats.size() ? formats[index].name : formats.front().name;
    adw_dialog_close(choice->dialog);
    window->chooseExportPath(format, seconds);
}

void TitlesWindow::onTemplateBannerClicked(AdwBanner *, gpointer self)
{
    static_cast<TitlesWindow *>(self)->updateFromTemplate();
}

void TitlesWindow::onDestroy(GtkWidget *, gpointer self)
{
    delete static_cast<TitlesWindow *>(self);
}

void TitlesWindow::onAction(GSimpleAction *action, GVariant *, gpointer self)
{
    auto *window = static_cast<TitlesWindow *>(self);
    const std::string name = g_action_get_name(G_ACTION(action));
    const TitleDocument &doc = window->m_history.document();
    if (name == "new") {
        auto *other = new TitlesWindow(gtk_window_get_application(window->window()), {}, {});
        gtk_window_present(other->window());
    } else if (name == "new-from-template") {
        window->showTemplates();
    } else if (name == "save-as-template") {
        window->saveAsTemplate();
    } else if (name == "open") {
        window->chooseOpenPath();
    } else if (name == "save") {
        if (window->m_path.empty())
            window->chooseSavePath();
        else
            window->save(window->m_path);
    } else if (name == "save-as") {
        window->chooseSavePath();
    } else if (name == "export") {
        window->showExportDialog();
    } else if (name == "undo") {
        if (window->m_history.undo())
            window->refresh();
    } else if (name == "redo") {
        if (window->m_history.redo())
            window->refresh();
    } else if (name == "add-text") {
        window->addLayerOf(makeTextLayer(doc, "Your text"), "Add Text");
    } else if (name == "add-rectangle") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::Rect), "Add Rectangle");
    } else if (name == "add-rounded") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::RoundedRect), "Add Rounded Rectangle");
    } else if (name == "add-ellipse") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::Ellipse), "Add Ellipse");
    } else if (name == "add-line") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::Line), "Add Line");
    } else if (name == "add-image") {
        window->choosePicture(std::nullopt);
    } else if (name == "add-animation") {
        window->chooseAnimation();
    } else if (name == "backdrop-checkerboard") {
        window->m_canvas->setBackdropImage(nullptr);
        ViewSettings view = loadViewSettings();
        view.backdrop = Backdrop::Checkerboard;
        saveViewSettings(view);
    } else if (name == "backdrop-colour") {
        window->chooseBackdropColour();
    } else if (name == "backdrop-image") {
        window->chooseBackdropImage();
    } else if (name == "toggle-guides") {
        GVariant *state = g_action_get_state(G_ACTION(action));
        const bool on = !g_variant_get_boolean(state);
        g_variant_unref(state);
        g_simple_action_set_state(action, g_variant_new_boolean(on));
        window->m_canvas->setGuidesVisible(on);
        ViewSettings view = loadViewSettings();
        view.guides = on;
        saveViewSettings(view);
    }
}

} // namespace ustudio::titles::app
