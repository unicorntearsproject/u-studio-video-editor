#include "gallery.h"

#include "packs.h"

#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "package/pack.h"
#include "platform/process.h"
#include "render/title_renderer.h"

#include <adwaita.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <vector>

// GLib's main-context lock is invisible to ThreadSanitizer: annotate the
// hand-off, as MainThreadDispatcher does (src/engine/dispatcher.cpp).
#ifdef __SANITIZE_THREAD__
#include <sanitizer/tsan_interface.h>
#define TITLES_TSAN_RELEASE(addr) __tsan_release(addr)
#define TITLES_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#else
#define TITLES_TSAN_RELEASE(addr) ((void)(addr))
#define TITLES_TSAN_ACQUIRE(addr) ((void)(addr))
#endif

namespace ustudio::titles::app {

namespace {

constexpr int kThumbWidth = 288, kThumbHeight = 162; // 16:9

bool isDirectory(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

// One gallery on screen: its dialog, what it shows, and the thumbnails'
// worker. Freed with the dialog.
struct Gallery
{
    AdwDialog *dialog = nullptr;
    GtkWidget *content = nullptr;
    GalleryCallbacks callbacks;
    std::vector<GtkWidget *> pictures; // by thumbnail index, this build of the content
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::thread worker;

    void stopWorker()
    {
        if (cancelled)
            cancelled->store(true);
        if (worker.joinable())
            worker.join();
    }
    ~Gallery()
    {
        stopWorker();
    }
    void rebuild();
    // `readOnly`: built-ins and packs' templates, changed only through a
    // copy. `headerExtra` sits beside the heading (a pack's Remove button).
    void addSection(const std::string &heading, const std::vector<TemplateInfo> &templates,
                    std::vector<std::pair<size_t, std::string>> &jobs, bool readOnly = false,
                    GtkWidget *headerExtra = nullptr);
};

struct Posted
{
    std::weak_ptr<std::atomic<bool>> alive; // expired or set: that build is gone
    Gallery *gallery;
    size_t index;
    RenderedFrame frame;
};

gboolean deliver(gpointer data)
{
    TITLES_TSAN_ACQUIRE(data);
    std::unique_ptr<Posted> posted(static_cast<Posted *>(data));
    auto cancelled = posted->alive.lock();
    if (!cancelled || cancelled->load() || posted->index >= posted->gallery->pictures.size())
        return G_SOURCE_REMOVE;
    const RenderedFrame &frame = posted->frame;
    GBytes *bytes = g_bytes_new(frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
    GdkTexture *texture = gdk_memory_texture_new(frame.width, frame.height, GDK_MEMORY_DEFAULT, bytes,
                                                 static_cast<gsize>(frame.width) * 4);
    g_bytes_unref(bytes);
    gtk_picture_set_paintable(GTK_PICTURE(posted->gallery->pictures[posted->index]), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    return G_SOURCE_REMOVE;
}

// A click that runs a function: the button owns it.
void onClicked(GtkButton *, gpointer data)
{
    (*static_cast<std::function<void()> *>(data))();
}
void connectClick(GtkWidget *button, std::function<void()> fn)
{
    g_signal_connect_data(
        button, "clicked", G_CALLBACK(onClicked), new std::function<void()>(std::move(fn)),
        [](gpointer data, GClosure *) { delete static_cast<std::function<void()> *>(data); }, G_CONNECT_DEFAULT);
}

GtkWidget *menuItem(GtkWidget *popover, GtkWidget *box, const char *label, std::function<void()> fn)
{
    GtkWidget *item = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(item, "flat");
    gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(item)), GTK_ALIGN_START);
    connectClick(item, [popover, fn = std::move(fn)] {
        gtk_popover_popdown(GTK_POPOVER(popover));
        fn();
    });
    gtk_box_append(GTK_BOX(box), item);
    return item;
}

void confirmDelete(Gallery *gallery, const TemplateInfo &info)
{
    AdwDialog *alert = adw_alert_dialog_new("Delete template?", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(alert), "“%s” will be deleted from My Templates.", info.name.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "_Cancel", "delete", "_Delete", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
    struct Pending
    {
        Gallery *gallery;
        TemplateInfo info;
    };
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(alert), GTK_WIDGET(gallery->dialog), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
            if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "delete")
                return;
            if (auto removed = removeTemplate(pending->info); !removed)
                pending->gallery->callbacks.toast("Couldn't delete it: " + removed.error());
            pending->gallery->rebuild();
        },
        new Pending{gallery, info});
}

void confirmRemovePack(Gallery *gallery, const pack::InstalledPack &installed)
{
    AdwDialog *alert = adw_alert_dialog_new("Remove pack?", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(alert),
                                 "“%s” and its templates will be removed. Titles already made from them stay as they "
                                 "are.",
                                 installed.manifest.title.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "_Cancel", "remove", "_Remove", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
    struct Pending
    {
        Gallery *gallery;
        pack::InstalledPack installed;
    };
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(alert), GTK_WIDGET(gallery->dialog), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
            if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "remove")
                return;
            if (auto removed = pack::remove(pending->installed); !removed)
                pending->gallery->callbacks.toast("Couldn't remove it: " + removed.error());
            pending->gallery->rebuild();
        },
        new Pending{gallery, installed});
}

void Gallery::addSection(const std::string &heading, const std::vector<TemplateInfo> &templates,
                         std::vector<std::pair<size_t, std::string>> &jobs, bool readOnly, GtkWidget *headerExtra)
{
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(header, 12);
    GtkWidget *label = gtk_label_new(heading.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "title-4");
    gtk_box_append(GTK_BOX(header), label);
    if (headerExtra)
        gtk_box_append(GTK_BOX(header), headerExtra);
    gtk_box_append(GTK_BOX(content), header);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 12);
    for (const TemplateInfo &info : templates) {
        GtkWidget *overlay = gtk_overlay_new();
        GtkWidget *button = gtk_button_new();
        gtk_widget_add_css_class(button, "flat");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *picture = gtk_picture_new();
        gtk_widget_set_size_request(picture, kThumbWidth, kThumbHeight);
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_add_css_class(picture, "card");
        gtk_box_append(GTK_BOX(box), picture);
        GtkWidget *name = gtk_label_new(info.name.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), name);
        gtk_button_set_child(GTK_BUTTON(button), box);
        gtk_widget_set_tooltip_text(button, ("Use “" + info.name + "”").c_str());
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, info.name.c_str(), -1);
        connectClick(button, [this, info] {
            // Closing frees the gallery (and this lambda): keep copies.
            GalleryCallbacks keep = callbacks;
            TemplateInfo picked = info;
            adw_dialog_close(dialog);
            keep.use(picked);
        });
        gtk_overlay_set_child(GTK_OVERLAY(overlay), button);

        // Its menu: what can be done with this template.
        GtkWidget *more = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
        gtk_widget_add_css_class(more, "osd");
        gtk_widget_add_css_class(more, "circular");
        gtk_widget_set_halign(more, GTK_ALIGN_END);
        gtk_widget_set_valign(more, GTK_ALIGN_START);
        gtk_widget_set_margin_top(more, 10);
        gtk_widget_set_margin_end(more, 10);
        gtk_widget_set_tooltip_text(more, "More for this template");
        GtkWidget *popover = gtk_popover_new();
        GtkWidget *items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        const std::string library = userTemplatesDir();
        if (info.builtIn || readOnly) {
            menuItem(popover, items, "Edit a Copy", [this, info, library] {
                auto copy = duplicateTemplate(info, library, info.name);
                if (!copy) {
                    callbacks.toast("Couldn't copy it: " + copy.error());
                    return;
                }
                GalleryCallbacks keep = callbacks;
                const std::string copied = info.name;
                const TemplateInfo edited = *copy;
                adw_dialog_close(dialog);
                keep.toast("“" + copied + "” copied to My Templates");
                keep.edit(edited);
            });
        } else {
            menuItem(popover, items, "Edit", [this, info] {
                GalleryCallbacks keep = callbacks;
                TemplateInfo edited = info;
                adw_dialog_close(dialog);
                keep.edit(edited);
            });
            menuItem(popover, items, "Rename…", [this, info] {
                askTemplateName(GTK_WIDGET(dialog), "Rename Template", info.name, [this, info](const std::string &n) {
                    if (auto renamed = renameTemplate(info, n); !renamed)
                        callbacks.toast("Couldn't rename it: " + renamed.error());
                    rebuild();
                });
            });
            menuItem(popover, items, "Duplicate", [this, info, library] {
                if (auto copy = duplicateTemplate(info, library, info.name + " copy"); !copy)
                    callbacks.toast("Couldn't duplicate it: " + copy.error());
                rebuild();
            });
            menuItem(popover, items, "Delete…", [this, info] { confirmDelete(this, info); });
        }
        gtk_popover_set_child(GTK_POPOVER(popover), items);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(more), popover);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), more);

        gtk_flow_box_append(GTK_FLOW_BOX(flow), overlay);
        jobs.push_back({pictures.size(), info.path});
        pictures.push_back(picture);
    }
    gtk_box_append(GTK_BOX(content), flow);
}

void Gallery::rebuild()
{
    stopWorker();
    while (GtkWidget *child = gtk_widget_get_first_child(content))
        gtk_box_remove(GTK_BOX(content), child);
    pictures.clear();

    std::vector<std::pair<size_t, std::string>> jobs; // thumbnail index, template file
    // Built-ins by category, the most used first; any other after, as
    // listed.
    static const std::vector<std::string> kOrder = {"Lower thirds", "Bugs and badges", "Cards",   "End screens",
                                                    "Countdowns",   "Live and social", "Captions"};
    const std::vector<TemplateInfo> builtIns = listTemplates(builtInTemplatesDir(), true);
    std::vector<std::string> categories;
    for (const TemplateInfo &info : builtIns)
        if (std::find(categories.begin(), categories.end(), info.category) == categories.end())
            categories.push_back(info.category);
    const auto rank = [](const std::string &category) {
        return std::find(kOrder.begin(), kOrder.end(), category) - kOrder.begin();
    };
    std::stable_sort(categories.begin(), categories.end(),
                     [&](const std::string &a, const std::string &b) { return rank(a) < rank(b); });
    for (const std::string &category : categories) {
        std::vector<TemplateInfo> in;
        std::copy_if(builtIns.begin(), builtIns.end(), std::back_inserter(in),
                     [&](const TemplateInfo &info) { return info.category == category; });
        addSection(category.empty() ? "Built-in" : category, in, jobs);
    }
    const std::vector<TemplateInfo> mine = listTemplates(userTemplatesDir(), false);
    if (mine.empty()) {
        GtkWidget *heading = gtk_label_new("My Templates");
        gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
        gtk_widget_add_css_class(heading, "title-4");
        gtk_widget_set_margin_top(heading, 12);
        gtk_box_append(GTK_BOX(content), heading);
        GtkWidget *hint = gtk_label_new("None yet. Save a title as a template (main menu › Save as Template…), or "
                                        "use a built-in's Edit a Copy.");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_box_append(GTK_BOX(content), hint);
    } else {
        addSection("My Templates", mine, jobs);
    }

    // Installed packs (doc 20), each a section of its own; their fonts
    // are made available first, for the thumbnails and the canvas.
    for (const pack::InstalledPack &installed : pack::installedPacks(userTemplatesDir())) {
        addFontDirectory(core::utf8String(core::pathFromUtf8(installed.folder) / "fonts"));
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        if (shareAvailable()) {
            GtkWidget *publishButton = gtk_button_new_with_label("Publish…");
            gtk_widget_add_css_class(publishButton, "flat");
            gtk_widget_set_tooltip_text(publishButton, "Share this pack with everyone (opens U-Stu Share)");
            const std::string folder = installed.folder;
            connectClick(publishButton, [this, folder] { publishPack(folder, callbacks.toast); });
            gtk_box_append(GTK_BOX(actions), publishButton);
        }
        GtkWidget *removeButton = gtk_button_new_with_label("Remove Pack…");
        gtk_widget_add_css_class(removeButton, "flat");
        gtk_widget_set_tooltip_text(removeButton, "Remove this pack and its templates from My Templates");
        connectClick(removeButton, [this, installed] { confirmRemovePack(this, installed); });
        gtk_box_append(GTK_BOX(actions), removeButton);
        const std::string heading = installed.manifest.title + " (pack " + installed.manifest.version + ")";
        addSection(heading, listTemplates(installed.folder, false), jobs, true, actions);
    }

    // Thumbnails mid-hold, on a worker, delivered one by one.
    cancelled = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<std::atomic<bool>> alive = cancelled;
    worker = std::thread([jobs, cancelled = cancelled, alive, gallery = this] {
        for (const auto &[index, path] : jobs) {
            if (cancelled->load())
                return;
            auto read = readTitle(path);
            if (!read)
                continue;
            const TitleDocument &doc = read->document;
            const double at =
                static_cast<double>(doc.timing.intro) + std::min(45.0, static_cast<double>(doc.timing.hold) / 2.0);
            auto *posted = new Posted{alive, gallery, index, renderTitle(doc, at, {}, kThumbWidth, kThumbHeight).frame};
            TITLES_TSAN_RELEASE(posted);
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &deliver, posted, nullptr);
        }
    });
}

} // namespace

std::string builtInTemplatesDir()
{
    namespace fs = std::filesystem;
    const fs::path self = platform::executablePath();
    if (!self.empty()) {
        const fs::path installed = self.parent_path().parent_path() / "share" / "u-studio" / "titles" / "templates";
        if (isDirectory(installed))
            return core::utf8String(installed);
    }
    return TITLES_TEMPLATES_SOURCE_DIR;
}

std::string userTemplatesDir()
{
    return pack::templatesLibrary();
}

void askTemplateName(GtkWidget *parent, const std::string &heading, const std::string &initial,
                     std::function<void(const std::string &)> done)
{
    AdwDialog *alert = adw_alert_dialog_new(heading.c_str(), nullptr);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Template name", -1);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), entry);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "_Cancel", "save", "_Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
    struct Pending
    {
        GtkWidget *entry;
        std::function<void(const std::string &)> done;
    };
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(alert), parent, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
            if (std::string(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result)) != "save")
                return;
            const std::string name = gtk_editable_get_text(GTK_EDITABLE(pending->entry));
            if (!name.empty())
                pending->done(name);
        },
        new Pending{entry, std::move(done)});
    gtk_widget_grab_focus(entry);
}

void showGallery(GtkWidget *parent, GalleryCallbacks callbacks)
{
    auto *gallery = new Gallery;
    gallery->callbacks = std::move(callbacks);
    gallery->dialog = adw_dialog_new();
    adw_dialog_set_title(gallery->dialog, "Templates");
    adw_dialog_set_content_width(gallery->dialog, 1320);
    adw_dialog_set_content_height(gallery->dialog, 760);
    g_object_set_data_full(G_OBJECT(gallery->dialog), "gallery", gallery,
                           [](gpointer data) { delete static_cast<Gallery *>(data); });

    gallery->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(gallery->content, 18);
    gtk_widget_set_margin_end(gallery->content, 18);
    gtk_widget_set_margin_bottom(gallery->content, 18);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), gallery->content);
    // Template packs (doc 20): open one into My Templates, or make one.
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *openPack = gtk_button_new_with_label("Open Pack…");
    gtk_widget_set_tooltip_text(openPack, "Install a template pack (.zip or .tar.gz) into My Templates");
    connectClick(openPack, [gallery] {
        openPackage(GTK_WIDGET(gallery->dialog), gallery->callbacks.toast, [gallery] { gallery->rebuild(); });
    });
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), openPack);
    GtkWidget *savePack = gtk_button_new_with_label("Save as Pack…");
    gtk_widget_set_tooltip_text(savePack, "Make a template pack from My Templates, to share");
    connectClick(savePack, [gallery] { savePackage(GTK_WIDGET(gallery->dialog), gallery->callbacks.toast); });
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), savePack);
    if (shareAvailable()) {
        GtkWidget *browse = gtk_button_new_with_label("Browse Shared…");
        gtk_widget_set_tooltip_text(browse, "Find template packs others have shared (opens U-Stu Share)");
        connectClick(browse, [gallery] { browseShared(gallery->callbacks.toast); });
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), browse);
    }
    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);
    adw_dialog_set_child(gallery->dialog, view);
    gallery->rebuild();
    adw_dialog_present(gallery->dialog, parent);
}

} // namespace ustudio::titles::app
