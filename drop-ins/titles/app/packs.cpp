#include "packs.h"

#include "gallery.h"

#include "core/media/utf8_path.h"
#include "core/template_library.h"
#include "core/title_xml.h"
#include "package/archive.h"
#include "platform/process.h"
#include "render/title_renderer.h"

#include <adwaita.h>
#include <cairo.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace ustudio::titles::app {

namespace {

std::string pathOf(GFile *file)
{
    char *raw = g_file_get_path(file);
    char *utf8 = raw ? g_filename_to_utf8(raw, -1, nullptr, nullptr, nullptr) : nullptr;
    std::string out = utf8 ? utf8 : "";
    g_free(utf8);
    g_free(raw);
    return out;
}

GtkFileFilter *packFilter()
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Template packs (.zip, .tar.gz)");
    gtk_file_filter_add_suffix(filter, "zip");
    gtk_file_filter_add_suffix(filter, "gz");
    gtk_file_filter_add_suffix(filter, "tgz");
    return filter;
}

struct Toasting
{
    std::function<void(const std::string &)> toast;
    std::function<void()> done;
    GtkWidget *parent;
    std::string path;
    pack::ValidPack pack;
};

void install(Toasting &t, pack::Replace replace)
{
    auto installed = pack::install(t.pack, userTemplatesDir(), replace);
    if (!installed) {
        t.toast("Couldn't install the pack: " + installed.error());
        return;
    }
    t.toast("Installed “" + installed->manifest.title + "” in My Templates");
    if (t.done)
        t.done();
}

// What's inside, then Install (or Replace, or Install Older).
void confirmInstall(std::unique_ptr<Toasting> t)
{
    const pack::Manifest &m = t->pack.manifest;
    size_t templates = 0;
    for (const auto &[path, data] : t->pack.files)
        templates += path.starts_with("templates/");
    std::string body = m.title + " " + m.version + (m.author.empty() ? "" : " by " + m.author) + "\n" +
                       std::to_string(templates) + (templates == 1 ? " template" : " templates") + ", licence " +
                       m.licence;
    if (!m.fonts.empty()) {
        body += "\nFonts:";
        for (const pack::ManifestFont &font : m.fonts)
            body += " " + font.family + " (" + font.licence + ")";
    }
    if (!m.description.empty())
        body += "\n\n" + m.description;
    std::string action = "install", label = "_Install";
    for (const pack::InstalledPack &installed : pack::installedPacks(userTemplatesDir())) {
        if (installed.manifest.id != m.id)
            continue;
        const int order = pack::compareVersions(m.version, installed.manifest.version);
        body += "\n\nVersion " + installed.manifest.version + " is installed. ";
        if (order > 0) {
            body += "This one is newer and replaces it.";
            action = "replace";
            label = "_Replace";
        } else {
            body += order == 0 ? "This is the same version." : "This one is older.";
            action = "downgrade";
            label = order == 0 ? "_Install Again" : "_Install Older";
        }
    }
    AdwDialog *alert = adw_alert_dialog_new("Install template pack?", body.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", "_Cancel", action.c_str(), label.c_str(),
                                   nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), action.c_str(),
                                             action == "downgrade" ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
    GtkWidget *parent = t->parent;
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(alert), parent, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Toasting> chosen(static_cast<Toasting *>(data));
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response == "install")
                install(*chosen, pack::Replace::Never);
            else if (response == "replace")
                install(*chosen, pack::Replace::IfNewer);
            else if (response == "downgrade")
                install(*chosen, pack::Replace::Always);
        },
        t.release());
}

cairo_status_t appendPng(void *closure, const unsigned char *data, unsigned int length)
{
    static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

std::string shareToolPath()
{
    const std::string name = std::string("u-studio-share") + platform::executableSuffix();
    const std::filesystem::path self = platform::executablePath();
    std::error_code ec;
    if (!self.empty() && std::filesystem::is_regular_file(self.parent_path() / name, ec))
        return core::utf8String(self.parent_path() / name);
#ifdef TITLES_SHARE_BUILD_PATH
    if (std::filesystem::is_regular_file(core::pathFromUtf8(TITLES_SHARE_BUILD_PATH), ec))
        return TITLES_SHARE_BUILD_PATH;
#endif
    gchar *found = g_find_program_in_path(name.c_str());
    const std::string out = found ? found : "";
    g_free(found);
    return out;
}

bool launchShare(const std::vector<std::string> &args, const std::function<void(const std::string &)> &toast)
{
    const std::string tool = shareToolPath();
    if (tool.empty()) {
        toast("Sharing templates needs U-Stu Share, which isn't installed (it's a separate download, with the "
              "network access this app doesn't have)");
        return false;
    }
    std::vector<const char *> argv = {tool.c_str()};
    for (const std::string &arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    GError *error = nullptr;
    GSubprocess *process = g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_NONE, &error);
    if (!process) {
        toast(std::string("Couldn't start U-Stu Share: ") + (error ? error->message : "?"));
        g_clear_error(&error);
        return false;
    }
    g_object_unref(process);
    return true;
}

} // namespace

bool shareAvailable()
{
    return !shareToolPath().empty();
}

void browseShared(std::function<void(const std::string &)> toast)
{
    launchShare({}, toast);
}

void publishPack(const std::string &folder, std::function<void(const std::string &)> toast)
{
    namespace fs = std::filesystem;
    const fs::path root = core::pathFromUtf8(folder);
    std::ifstream in(root / "pack.xml", std::ios::binary);
    const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto manifest = pack::parseManifest(xml);
    if (!manifest) {
        toast("Couldn't read the pack: " + manifest.error());
        return;
    }
    std::vector<pack::PackTemplate> templates;
    for (const TemplateInfo &info : listTemplates(folder, false)) {
        std::string png;
        if (!info.preview.empty()) {
            std::ifstream preview(core::pathFromUtf8(info.preview), std::ios::binary);
            png.assign(std::istreambuf_iterator<char>(preview), std::istreambuf_iterator<char>());
        } else {
            png = previewPng(info.path);
        }
        templates.push_back({info.folder, png});
    }
    auto entries = pack::build(*manifest, templates);
    if (!entries) {
        toast("Couldn't make the pack to publish: " + entries.error());
        return;
    }
    const fs::path out = core::pathFromUtf8(g_get_user_cache_dir()) / "ustudio-titles" / "publish" /
                         core::pathFromUtf8(templateSlug(manifest->id) + "-" + manifest->version + ".zip");
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (auto written = pack::writeArchive(*entries, core::utf8String(out)); !written) {
        toast("Couldn't make the pack to publish: " + written.error());
        return;
    }
    launchShare({"--publish", core::utf8String(out)}, toast);
}

std::string previewPng(const std::string &templatePath, int width, int height)
{
    auto read = readTitle(templatePath);
    if (!read)
        return {};
    const TitleDocument &doc = read->document;
    const double at =
        static_cast<double>(doc.timing.intro) + std::min(45.0, static_cast<double>(doc.timing.hold) / 2.0);
    RenderResult result = renderTitle(doc, at, {}, width, height);
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char *>(result.frame.pixels.data()), CAIRO_FORMAT_ARGB32, result.frame.width,
        result.frame.height, result.frame.width * 4);
    std::string png;
    cairo_surface_write_to_png_stream(surface, &appendPng, &png);
    cairo_surface_destroy(surface);
    return png;
}

void openPackage(GtkWidget *parent, std::function<void(const std::string &)> toast, std::function<void()> done)
{
    if (!pack::archivesSupported()) {
        toast("This build can't open template packs (built without libarchive)");
        return;
    }
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Template Pack");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *filter = packFilter();
    g_list_store_append(filters, filter);
    g_object_unref(filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
    auto *t = new Toasting{std::move(toast), std::move(done), parent, {}, {}};
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(gtk_widget_get_root(parent)), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Toasting> opened(static_cast<Toasting *>(data));
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            opened->path = pathOf(file);
            g_object_unref(file);
            auto inspected = pack::inspectPackage(opened->path);
            if (!inspected) {
                opened->toast("Couldn't open the pack: " + inspected.error());
                return;
            }
            opened->pack = std::move(*inspected);
            confirmInstall(std::move(opened));
        },
        t);
    g_object_unref(dialog);
}

void savePackage(GtkWidget *parent, std::function<void(const std::string &)> toast)
{
    if (!pack::archivesSupported()) {
        toast("This build can't save template packs (built without libarchive)");
        return;
    }
    const std::vector<TemplateInfo> mine = listTemplates(userTemplatesDir(), false);
    if (mine.empty()) {
        toast("Save some titles as templates first: a pack is made from My Templates");
        return;
    }
    // The pack's details and its templates, then where to save it.
    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Save as Template Pack");
    adw_dialog_set_content_width(dialog, 520);
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *details = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(details), "Pack");
    const auto row = [&](const char *title, const char *text) {
        GtkWidget *entry = adw_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), title);
        gtk_editable_set_text(GTK_EDITABLE(entry), text);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(details), entry);
        return entry;
    };
    GtkWidget *title = row("Title", "My templates");
    GtkWidget *author = row("Author", g_get_real_name());
    GtkWidget *version = row("Version", "1.0.0");
    GtkWidget *licence = row("Licence (SPDX, e.g. CC-BY-4.0)", "CC-BY-4.0");
    GtkWidget *description = row("Description", "");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(details));
    GtkWidget *which = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(which), "Templates");
    std::vector<std::pair<GtkWidget *, TemplateInfo>> checks;
    for (const TemplateInfo &info : mine) {
        GtkWidget *check = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(check), info.name.c_str());
        adw_switch_row_set_active(ADW_SWITCH_ROW(check), TRUE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(which), check);
        checks.push_back({check, info});
    }
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(which));
    GtkWidget *save = gtk_button_new_with_label("Save…");
    gtk_widget_add_css_class(save, "suggested-action");
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), save);
    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(dialog, view);

    struct Form
    {
        AdwDialog *dialog;
        GtkWidget *parent, *title, *author, *version, *licence, *description;
        std::vector<std::pair<GtkWidget *, TemplateInfo>> checks;
        std::function<void(const std::string &)> toast;
    };
    auto *form = new Form{dialog, parent, title, author, version, licence, description, std::move(checks), toast};
    g_signal_connect_data(
        save, "clicked", G_CALLBACK(+[](GtkButton *, gpointer self) {
            auto *f = static_cast<Form *>(self);
            pack::Manifest m;
            m.title = gtk_editable_get_text(GTK_EDITABLE(f->title));
            m.author = gtk_editable_get_text(GTK_EDITABLE(f->author));
            m.version = gtk_editable_get_text(GTK_EDITABLE(f->version));
            m.licence = gtk_editable_get_text(GTK_EDITABLE(f->licence));
            m.description = gtk_editable_get_text(GTK_EDITABLE(f->description));
            // Stable across versions: the author's and the title's slugs.
            m.id = templateSlug(m.author.empty() ? "me" : m.author) + "/" + templateSlug(m.title);
            std::vector<pack::PackTemplate> templates;
            for (const auto &[check, info] : f->checks)
                if (adw_switch_row_get_active(ADW_SWITCH_ROW(check)))
                    templates.push_back({info.folder, previewPng(info.path)});
            auto entries = pack::build(m, templates);
            if (!entries) {
                f->toast("Couldn't make the pack: " + entries.error());
                return;
            }
            // Checked like any pack before it's saved: what we send out is
            // what others will accept.
            uint64_t unpacked = 0;
            for (const pack::Entry &entry : *entries)
                unpacked += entry.data.size();
            if (auto valid = pack::validate(*entries, unpacked); !valid) {
                f->toast("The pack wouldn't open: " + valid.error());
                return;
            }
            struct Pending
            {
                std::vector<pack::Entry> entries;
                std::function<void(const std::string &)> toast;
            };
            GtkFileDialog *chooser = gtk_file_dialog_new();
            gtk_file_dialog_set_title(chooser, "Save Template Pack");
            gtk_file_dialog_set_initial_name(chooser, (templateSlug(m.title) + ".zip").c_str());
            gtk_file_dialog_save(
                chooser, GTK_WINDOW(gtk_widget_get_root(f->parent)), nullptr,
                [](GObject *source, GAsyncResult *result, gpointer data) {
                    std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
                    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
                    if (!file)
                        return;
                    const std::string path = pathOf(file);
                    g_object_unref(file);
                    auto written = pack::writeArchive(pending->entries, path);
                    pending->toast(written ? "Saved the pack " + core::utf8String(core::pathFromUtf8(path).filename())
                                           : "Couldn't save the pack: " + written.error());
                },
                new Pending{std::move(*entries), f->toast});
            g_object_unref(chooser);
            adw_dialog_close(f->dialog);
        }),
        form, [](gpointer data, GClosure *) { delete static_cast<Form *>(data); }, G_CONNECT_DEFAULT);
    adw_dialog_present(dialog, parent);
}

} // namespace ustudio::titles::app
