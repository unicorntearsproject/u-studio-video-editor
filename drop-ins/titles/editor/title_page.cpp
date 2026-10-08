#include "title_page.h"

#include "core/clip_fields.h"
#include "core/export_formats.h"
#include "core/media/utf8_path.h"
#include "core/title_xml.h"
#include "title_bake.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ustudio::titles {

namespace {

class TitlePage
{
  public:
    // The window it serves: a new host replaces the old one's page.
    void bind(app::ShellHost &host)
    {
        m_host = &host;
        m_clip.reset();
        m_rows.clear();
        m_cachedPath.clear();
        build();
        host.addHints({{"titles.page-edit", "Titles", "Edit title",
                        "Open this title in U-Stu Titles to change its design", "titles-edit", nullptr},
                       {"titles.page-export-format", "Titles", "Export format",
                        "With transparency (alpha) for OBS and other apps, or flattened onto the title's background",
                        nullptr, nullptr},
                       {"titles.page-export", "Titles", "Export title",
                        "The clip's title on its own, at the clip's length, with its fields", nullptr, nullptr},
                       {"titles.page-field", "Titles", "Title field",
                        "This clip's text for the field; the title's default when left as it is", nullptr, nullptr}});
        host.setTooltip(m_edit, "titles.page-edit");
        host.setTooltip(m_bake, "titles.bake");
        host.setTooltip(m_exportCaptions, "titles.export-captions");
        host.setTooltip(m_format, "titles.page-export-format");
        host.setTooltip(m_exportButton, "titles.page-export");
        host.addInspectorPage({"titles.title", "Title", "insert-text-symbolic", m_root});
        host.selectionChanged().connect([this] { refresh(); });
        host.projectChanged().connect([this] { refresh(); });
        refresh();
    }

  private:
    struct Row
    {
        std::string name, defaultValue;
        GtkWidget *entry = nullptr;
    };

    void build()
    {
        m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_start(m_root, 12);
        gtk_widget_set_margin_end(m_root, 12);
        gtk_widget_set_margin_top(m_root, 12);
        gtk_widget_set_margin_bottom(m_root, 12);

        m_empty = gtk_label_new("Select a title clip to fill in its fields.");
        gtk_label_set_wrap(GTK_LABEL(m_empty), TRUE);
        gtk_widget_add_css_class(m_empty, "dim-label");
        gtk_box_append(GTK_BOX(m_root), m_empty);

        m_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        m_name = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(m_name), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(m_name), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(m_name, TRUE);
        gtk_widget_add_css_class(m_name, "heading");
        gtk_box_append(GTK_BOX(header), m_name);
        m_edit = gtk_button_new_with_label("Edit Title…");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(m_edit), "win.titles-edit");
        gtk_box_append(GTK_BOX(header), m_edit);
        m_bake = gtk_button_new_with_label("Bake…");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(m_bake), "win.titles-bake");
        gtk_box_append(GTK_BOX(header), m_bake);
        gtk_box_append(GTK_BOX(m_content), header);

        m_fieldsLabel = gtk_label_new("Fields");
        gtk_label_set_xalign(GTK_LABEL(m_fieldsLabel), 0.0f);
        gtk_widget_add_css_class(m_fieldsLabel, "heading");
        gtk_box_append(GTK_BOX(m_content), m_fieldsLabel);
        m_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(m_content), m_fields);
        m_noFields = gtk_label_new("This title has no fields. Add one in U-Stu Titles by typing {{name}} in a "
                                   "text layer.");
        gtk_label_set_wrap(GTK_LABEL(m_noFields), TRUE);
        gtk_label_set_xalign(GTK_LABEL(m_noFields), 0.0f);
        gtk_widget_add_css_class(m_noFields, "dim-label");
        gtk_box_append(GTK_BOX(m_content), m_noFields);
        // A caption (T5.1): every caption in the project to a subtitle file.
        m_exportCaptions = gtk_button_new_with_label("Export Captions…");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(m_exportCaptions), "win.titles-export-captions");
        gtk_widget_set_halign(m_exportCaptions, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(m_content), m_exportCaptions);

        // Export on its own (doc 16, T2d): for OBS and other tools.
        GtkWidget *exportLabel = gtk_label_new("Export on its own");
        gtk_label_set_xalign(GTK_LABEL(exportLabel), 0.0f);
        gtk_widget_add_css_class(exportLabel, "heading");
        gtk_box_append(GTK_BOX(m_content), exportLabel);
        GtkWidget *exportRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkStringList *labels = gtk_string_list_new(nullptr);
        for (const ExportFormat &format : exportFormats())
            gtk_string_list_append(labels, format.label);
        m_format = gtk_drop_down_new(G_LIST_MODEL(labels), nullptr);
        gtk_widget_set_hexpand(m_format, TRUE);
        gtk_box_append(GTK_BOX(exportRow), m_format);
        GtkWidget *exportButton = gtk_button_new_with_label("Export…");
        g_signal_connect(exportButton, "clicked", G_CALLBACK(&TitlePage::onExportTrampoline), this);
        gtk_box_append(GTK_BOX(exportRow), exportButton);
        gtk_box_append(GTK_BOX(m_content), exportRow);
        m_exportButton = exportButton;
        gtk_box_append(GTK_BOX(m_root), m_content);
    }

    // The first selected clip that plays a title.
    std::optional<core::ClipId> selectedTitleClip() const
    {
        const core::Model &model = m_host->model();
        for (core::ClipId id : m_host->currentSelection().clips)
            if (model.hasClip(id)) {
                const core::Clip &clip = model.clip(id);
                if (model.hasAsset(clip.asset) && isTitleFile(model.asset(clip.asset).path))
                    return id;
            }
        return std::nullopt;
    }

    // The title's fields, read again only when its file changed.
    const std::vector<Field> &fieldsOf(const core::Asset &asset)
    {
        if (asset.path != m_cachedPath || asset.fileFingerprint != m_cachedFingerprint) {
            m_cachedPath = asset.path;
            m_cachedFingerprint = asset.fileFingerprint;
            auto read = readTitle(asset.path);
            m_cachedFields = read ? read->document.fields : std::vector<Field>{};
        }
        return m_cachedFields;
    }

    void refresh()
    {
        const std::optional<core::ClipId> id = selectedTitleClip();
        gtk_widget_set_visible(m_empty, !id);
        gtk_widget_set_visible(m_content, id.has_value());
        if (!id) {
            m_clip.reset();
            return;
        }
        const core::Model &model = m_host->model();
        const core::Clip &clip = model.clip(*id);
        const core::Asset &asset = model.asset(clip.asset);
        gtk_label_set_text(GTK_LABEL(m_name), asset.displayName.c_str());
        const std::vector<Field> &fields = fieldsOf(asset);
        const std::map<std::string, std::string> values = clipFieldValues(clip);

        bool same = m_clip == id && m_rows.size() == fields.size();
        for (size_t i = 0; same && i < fields.size(); ++i)
            same = m_rows[i].name == fields[i].name && m_rows[i].defaultValue == fields[i].defaultValue;
        if (!same) {
            ++m_gesture; // another clip or title: a new undo step
            m_clip = id;
            while (GtkWidget *child = gtk_widget_get_first_child(m_fields))
                gtk_box_remove(GTK_BOX(m_fields), child);
            m_rows.clear();
            for (const Field &field : fields) {
                GtkWidget *label = gtk_label_new(field.name.c_str());
                gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
                gtk_widget_add_css_class(label, "caption");
                gtk_box_append(GTK_BOX(m_fields), label);
                GtkWidget *entry = gtk_entry_new();
                m_host->setTooltip(entry, "titles.page-field");
                gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, field.name.c_str(),
                                               -1);
                gtk_box_append(GTK_BOX(m_fields), entry);
                g_signal_connect(entry, "changed", G_CALLBACK(&TitlePage::onChangedTrampoline), this);
                GtkEventController *focus = gtk_event_controller_focus_new();
                g_signal_connect(focus, "leave", G_CALLBACK(&TitlePage::onFocusLeaveTrampoline), this);
                gtk_widget_add_controller(entry, focus);
                m_rows.push_back({field.name, field.defaultValue, entry});
            }
        }
        gtk_widget_set_visible(m_fieldsLabel, !fields.empty());
        gtk_widget_set_visible(m_noFields, fields.empty());
        gtk_widget_set_visible(m_exportCaptions, values.contains("caption"));
        // Undo, redo, another clip: show the clip's values, but never
        // rewrite what's being typed (it's what the model has already).
        m_updating = true;
        for (Row &row : m_rows) {
            auto value = values.find(row.name);
            const std::string &text = value != values.end() ? value->second : row.defaultValue;
            if (text != gtk_editable_get_text(GTK_EDITABLE(row.entry)))
                gtk_editable_set_text(GTK_EDITABLE(row.entry), text.c_str());
        }
        m_updating = false;
    }

    void onExport()
    {
        if (!m_clip || !m_host->model().hasClip(*m_clip))
            return;
        const std::vector<ExportFormat> &formats = exportFormats();
        const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(m_format));
        const ExportFormat &format = index < formats.size() ? formats[index] : formats.front();
        const core::Model &model = m_host->model();
        const std::string title = model.asset(model.clip(*m_clip).asset).path;
        const std::filesystem::path path = core::pathFromUtf8(title);
        std::string name = core::utf8String(path.stem());
        if (format.extension[0])
            name += std::string(".") + format.extension;
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, format.extension[0] ? "Export Title" : "Export Title (a folder of PNGs)");
        gtk_file_dialog_set_initial_name(dialog, name.c_str());
        GFile *folder = g_file_new_for_path(core::utf8String(path.parent_path()).c_str());
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
        struct Request
        {
            TitlePage *page;
            core::ClipId clip;
            std::string format;
        };
        gtk_file_dialog_save(
            dialog, GTK_WINDOW(gtk_widget_get_root(m_root)), nullptr,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                std::unique_ptr<Request> request(static_cast<Request *>(data));
                GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
                if (!file)
                    return;
                gchar *chosen = g_file_get_path(file);
                g_object_unref(file);
                if (!chosen)
                    return;
                exportTitleClip(*request->page->m_host, request->clip, request->format, chosen);
                g_free(chosen);
            },
            new Request{this, *m_clip, format.name});
        g_object_unref(dialog);
    }

    void onChanged()
    {
        if (m_updating || !m_clip || !m_host->model().hasClip(*m_clip))
            return;
        // Values for fields the title no longer has stay with the clip.
        std::map<std::string, std::string> values = clipFieldValues(m_host->model().clip(*m_clip));
        for (const Row &row : m_rows) {
            const std::string text = gtk_editable_get_text(GTK_EDITABLE(row.entry));
            if (text == row.defaultValue)
                values.erase(row.name);
            else
                values[row.name] = text;
        }
        m_host->execute(std::make_unique<SetClipFields>(*m_clip, std::move(values), m_gesture));
    }

    app::ShellHost *m_host = nullptr;
    GtkWidget *m_root = nullptr, *m_empty = nullptr, *m_content = nullptr, *m_name = nullptr, *m_edit = nullptr,
              *m_bake = nullptr, *m_fieldsLabel = nullptr, *m_fields = nullptr, *m_noFields = nullptr,
              *m_format = nullptr, *m_exportButton = nullptr, *m_exportCaptions = nullptr;
    std::optional<core::ClipId> m_clip;
    std::vector<Row> m_rows;
    std::string m_cachedPath, m_cachedFingerprint;
    std::vector<Field> m_cachedFields;
    uint64_t m_gesture = 1;
    bool m_updating = false;

    // --- GTK trampolines -------------------------------------------------
    static void onChangedTrampoline(GtkEditable *, gpointer self)
    {
        static_cast<TitlePage *>(self)->onChanged();
    }
    static void onExportTrampoline(GtkButton *, gpointer self)
    {
        static_cast<TitlePage *>(self)->onExport();
    }
    static void onFocusLeaveTrampoline(GtkEventControllerFocus *, gpointer self)
    {
        ++static_cast<TitlePage *>(self)->m_gesture; // the next visit is its own undo step
    }
};

} // namespace

void addTitlePage(app::ShellHost &host)
{
    // For the process, as the host requires.
    static TitlePage page;
    page.bind(host);
}

} // namespace ustudio::titles
