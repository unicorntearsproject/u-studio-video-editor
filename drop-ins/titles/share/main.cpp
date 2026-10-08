// u-studio-share (ADR-020, doc 20, T7): browse, download and publish title
// template packs. The one program with network access; the editor and U-Stu
// Titles never link a network library.
//
//   u-studio-share                    browse the shared templates
//   u-studio-share --publish PACK     show what PACK uploads, then publish it
//
// Every request follows a user action: opening it to browse, a search, a
// download, signing in, publishing. The service is doc 21's API at
// $USTUDIO_SHARE_URL (tests: tools/share_mock.py), else the default below,
// which is a placeholder until the owner picks the domain (doc 20 question
// 1): .invalid never resolves, so nothing leaves the machine by accident.

#include "share/client.h"
#include "share/handoff.h"
#include "share/signin.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "package/archive.h"

#include <adwaita.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace Log = ustudio::core::Log;
namespace share = ustudio::titles::share;
namespace pack = ustudio::titles::pack;

constexpr const char *kDefaultService = "https://templates.ustudio.invalid";

std::string serviceUrl()
{
    const char *env = g_getenv("USTUDIO_SHARE_URL");
    return env && *env ? env : kDefaultService;
}

std::string downloadsDir()
{
    return ustudio::core::utf8String(ustudio::core::pathFromUtf8(g_get_user_cache_dir()) / "ustudio-share" /
                                     "downloads");
}

// Work off the main thread, its result back on it.
template <typename Result> void inBackground(std::function<Result()> work, std::function<void(Result)> done)
{
    struct Job
    {
        std::function<Result()> work;
        std::function<void(Result)> done;
        std::optional<Result> result;
    };
    GTask *task = g_task_new(
        nullptr, nullptr,
        [](GObject *, GAsyncResult *result, gpointer) {
            auto *job = static_cast<Job *>(g_task_get_task_data(G_TASK(result)));
            job->done(std::move(*job->result));
        },
        nullptr);
    g_task_set_task_data(task, new Job{std::move(work), std::move(done), std::nullopt},
                         [](gpointer data) { delete static_cast<Job *>(data); });
    g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer data, GCancellable *) {
        auto *job = static_cast<Job *>(data);
        job->result = job->work();
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);
}

class ShareWindow
{
  public:
    ShareWindow(GtkApplication *app, const std::string &publishPath) : m_publishPath(publishPath)
    {
        m_window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
        gtk_window_set_title(GTK_WINDOW(m_window), "Shared Templates");
        gtk_window_set_default_size(GTK_WINDOW(m_window), 960, 640);
        m_toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
        GtkWidget *view = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        m_account = gtk_button_new_with_label("Sign In");
        g_signal_connect(
            m_account, "clicked",
            G_CALLBACK(+[](GtkButton *, gpointer self) { static_cast<ShareWindow *>(self)->signIn(nullptr); }), this);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_account);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_toasts));
        adw_application_window_set_content(m_window, view);
        g_object_set_data_full(G_OBJECT(m_window), "share-window", this,
                               [](gpointer self) { delete static_cast<ShareWindow *>(self); });
        if (publishPath.empty())
            buildBrowse(header);
        else
            buildPublish();
    }

    GtkWindow *window() const
    {
        return GTK_WINDOW(m_window);
    }

  private:
    void toast(const std::string &text)
    {
        adw_toast_overlay_add_toast(m_toasts, adw_toast_new(text.c_str()));
    }

    GtkWidget *label(const std::string &text, const char *css = nullptr)
    {
        GtkWidget *l = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        if (css)
            gtk_widget_add_css_class(l, css);
        return l;
    }

    // --- Browsing ---------------------------------------------------------------

    void buildBrowse(GtkWidget *header)
    {
        m_search = gtk_search_entry_new();
        gtk_widget_set_size_request(m_search, 320, -1);
        adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), m_search);
        g_signal_connect(
            m_search, "activate",
            G_CALLBACK(+[](GtkSearchEntry *, gpointer self) { static_cast<ShareWindow *>(self)->search(); }), this);
        GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_position(GTK_PANED(paned), 380);
        m_results = gtk_list_box_new();
        gtk_widget_add_css_class(m_results, "navigation-sidebar");
        // Selecting a pack (a click, or the arrow keys) shows it.
        g_signal_connect(m_results, "row-selected", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer self) {
                             if (row)
                                 static_cast<ShareWindow *>(self)->showPack(gtk_list_box_row_get_index(row));
                         }),
                         this);
        GtkWidget *left = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(left), m_results);
        gtk_paned_set_start_child(GTK_PANED(paned), left);
        m_details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_margin_start(m_details, 18);
        gtk_widget_set_margin_end(m_details, 18);
        gtk_widget_set_margin_top(m_details, 18);
        gtk_box_append(GTK_BOX(m_details), label("Pick a pack to see what it holds.", "dim-label"));
        GtkWidget *right = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(right), m_details);
        gtk_paned_set_end_child(GTK_PANED(paned), right);
        adw_toast_overlay_set_child(m_toasts, paned);
        // Opening this window is the user asking to browse.
        search();
    }

    void search()
    {
        const std::string query = gtk_editable_get_text(GTK_EDITABLE(m_search));
        const std::string base = serviceUrl();
        using Result = std::expected<std::vector<share::CatalogueEntry>, std::string>;
        inBackground<Result>([base, query] { return share::Client(base).list(query, ""); },
                             [this](Result result) {
                                 while (GtkWidget *row = gtk_widget_get_first_child(m_results))
                                     gtk_list_box_remove(GTK_LIST_BOX(m_results), row);
                                 m_catalogue.clear();
                                 if (!result) {
                                     toast("Couldn't reach the template service: " + result.error());
                                     return;
                                 }
                                 m_catalogue = std::move(*result);
                                 for (const share::CatalogueEntry &e : m_catalogue) {
                                     GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                                     gtk_box_append(GTK_BOX(box), label(e.title, "heading"));
                                     gtk_box_append(GTK_BOX(box), label(e.author + " · " + e.licence + " · " +
                                                                            std::to_string(e.downloads) + " downloads",
                                                                        "dim-label"));
                                     gtk_list_box_append(GTK_LIST_BOX(m_results), box);
                                 }
                                 if (m_catalogue.empty())
                                     toast("No packs found");
                             });
    }

    void showPack(int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= m_catalogue.size())
            return;
        const std::string id = m_catalogue[static_cast<size_t>(index)].id, base = serviceUrl();
        using Result = std::expected<share::PackDetails, std::string>;
        inBackground<Result>(
            [base, id] { return share::Client(base).pack(id); },
            [this](Result result) {
                while (GtkWidget *child = gtk_widget_get_first_child(m_details))
                    gtk_box_remove(GTK_BOX(m_details), child);
                if (!result) {
                    toast("Couldn't load the pack: " + result.error());
                    return;
                }
                const share::PackDetails &d = *result;
                gtk_box_append(GTK_BOX(m_details), label(d.title, "title-2"));
                gtk_box_append(GTK_BOX(m_details), label("by " + d.author + " · licence " + d.licence, "dim-label"));
                if (!d.description.empty())
                    gtk_box_append(GTK_BOX(m_details), label(d.description));
                std::string templates = "Templates:";
                for (const std::string &t : d.templates)
                    templates += "\n  " + t;
                gtk_box_append(GTK_BOX(m_details), label(templates));
                if (d.versions.empty())
                    return;
                const std::string version = d.versions.back().version;
                GtkWidget *download = gtk_button_new_with_label(("Download and Install " + version).c_str());
                gtk_widget_add_css_class(download, "suggested-action");
                gtk_widget_set_halign(download, GTK_ALIGN_START);
                m_pendingId = d.id;
                m_pendingVersion = version;
                g_signal_connect(download, "clicked", G_CALLBACK(+[](GtkButton *, gpointer self) {
                                     static_cast<ShareWindow *>(self)->download();
                                 }),
                                 this);
                gtk_box_append(GTK_BOX(m_details), download);
            });
    }

    void download()
    {
        const std::string base = serviceUrl(), id = m_pendingId, version = m_pendingVersion, dir = downloadsDir();
        toast("Downloading…");
        using Result = std::expected<std::string, std::string>;
        inBackground<Result>(
            [base, id, version, dir]() -> Result {
                auto path = share::Client(base).download(id, version, dir);
                if (!path)
                    return std::unexpected(path.error());
                return share::handOff(*path);
            },
            [this](Result result) { toast(result ? *result : "Couldn't download it: " + result.error()); });
    }

    // --- Signing in ---------------------------------------------------------------

    // `then` runs once signed in (or not at all).
    void signIn(std::function<void()> then)
    {
        if (!m_token.empty()) {
            if (then)
                then();
            return;
        }
        const std::string base = serviceUrl();
        // A refresh token from the keyring first, when there is one.
        const std::string saved = share::loadRefreshToken(base);
        if (!saved.empty()) {
            using Result = std::expected<share::Tokens, std::string>;
            inBackground<Result>([base, saved] { return share::Client(base).refresh(share::kClientId, saved); },
                                 [this, then, base](Result result) {
                                     if (result) {
                                         signedIn(base, *result);
                                         if (then)
                                             then();
                                     } else {
                                         share::forgetRefreshToken(base);
                                         browserSignIn(then);
                                     }
                                 });
            return;
        }
        browserSignIn(then);
    }

    void browserSignIn(std::function<void()> then)
    {
        m_signIn = std::make_unique<share::LoopbackSignIn>();
        if (!m_signIn->listening()) {
            toast("Couldn't start signing in (no local port)");
            return;
        }
        const std::string base = serviceUrl();
        const std::string url = m_signIn->start(base, [this, then, base](std::string code, std::string error) {
            if (code.empty()) {
                toast(error);
                return;
            }
            const std::string verifier = m_signIn->pkce().verifier, redirect = m_signIn->redirectUri();
            using Result = std::expected<share::Tokens, std::string>;
            inBackground<Result>(
                [base, code, verifier, redirect] {
                    return share::Client(base).exchangeCode(share::kClientId, code, verifier, redirect);
                },
                [this, then, base](Result result) {
                    if (!result) {
                        toast(result.error());
                        return;
                    }
                    signedIn(base, *result);
                    if (then)
                        then();
                });
        });
        GtkUriLauncher *launcher = gtk_uri_launcher_new(url.c_str());
        gtk_uri_launcher_launch(launcher, GTK_WINDOW(m_window), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
        toast("Sign in in your browser, then come back here");
    }

    void signedIn(const std::string &base, const share::Tokens &tokens)
    {
        m_token = tokens.access;
        if (!tokens.refresh.empty())
            share::storeRefreshToken(base, tokens.refresh);
        gtk_button_set_label(GTK_BUTTON(m_account), "Signed In");
        gtk_widget_set_sensitive(m_account, FALSE);
    }

    // --- Publishing -----------------------------------------------------------------

    void buildPublish()
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_margin_start(box, 24);
        gtk_widget_set_margin_end(box, 24);
        gtk_widget_set_margin_top(box, 24);
        auto pack = pack::inspectPackage(m_publishPath);
        if (!pack) {
            gtk_box_append(GTK_BOX(box), label("This pack can't be published: " + pack.error()));
            adw_toast_overlay_set_child(m_toasts, box);
            return;
        }
        const pack::Manifest &m = pack->manifest;
        gtk_box_append(GTK_BOX(box), label("Publish “" + m.title + "” " + m.version, "title-2"));
        std::string what = "Everything below is uploaded, and becomes public once the service has checked it:\n";
        for (const auto &[path, data] : pack->files)
            what += "\n  " + path + " (" + std::to_string(data.size()) + " bytes)";
        what += "\n\nLicence: " + m.licence + " (anyone may use the templates under it)";
        gtk_box_append(GTK_BOX(box), label(what));
        m_publishButton = gtk_button_new_with_label("Sign In and Publish");
        gtk_widget_add_css_class(m_publishButton, "suggested-action");
        gtk_widget_set_halign(m_publishButton, GTK_ALIGN_START);
        g_signal_connect(m_publishButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer self) {
                             auto *w = static_cast<ShareWindow *>(self);
                             w->signIn([w] { w->publish(); });
                         }),
                         this);
        gtk_box_append(GTK_BOX(box), m_publishButton);
        adw_toast_overlay_set_child(m_toasts, box);
    }

    void publish()
    {
        gtk_widget_set_sensitive(m_publishButton, FALSE);
        const std::string base = serviceUrl(), token = m_token, path = m_publishPath;
        using Result = std::expected<std::string, std::string>;
        inBackground<Result>(
            [base, token, path] {
                share::Client client(base);
                client.setAccessToken(token);
                return client.publish(path);
            },
            [this](Result result) {
                if (!result) {
                    gtk_widget_set_sensitive(m_publishButton, TRUE);
                    toast("Couldn't publish it: " + result.error());
                    return;
                }
                m_uploadId = *result;
                toast("Uploaded; the service is checking it");
                poll();
            });
    }

    void poll()
    {
        const std::string base = serviceUrl(), token = m_token, upload = m_uploadId;
        using Result = std::expected<share::UploadStatus, std::string>;
        inBackground<Result>(
            [base, token, upload] {
                share::Client client(base);
                client.setAccessToken(token);
                return client.uploadStatus(upload);
            },
            [this](Result result) {
                if (!result) {
                    toast("Couldn't check the upload: " + result.error());
                    return;
                }
                switch (result->state) {
                case share::UploadState::Published:
                    toast("Published");
                    return;
                case share::UploadState::InReview:
                    toast("Uploaded: it's published once it has been reviewed");
                    return;
                case share::UploadState::Rejected: {
                    std::string why;
                    for (const std::string &r : result->reasons)
                        why += (why.empty() ? "" : "; ") + r;
                    toast("The service refused it: " + why);
                    return;
                }
                default:
                    // Checked again in a moment (the user started this).
                    g_timeout_add_seconds_once(
                        3, [](gpointer self) { static_cast<ShareWindow *>(self)->poll(); }, this);
                }
            });
    }

    AdwApplicationWindow *m_window = nullptr;
    AdwToastOverlay *m_toasts = nullptr;
    GtkWidget *m_account = nullptr, *m_search = nullptr, *m_results = nullptr, *m_details = nullptr,
              *m_publishButton = nullptr;
    std::vector<share::CatalogueEntry> m_catalogue;
    std::string m_pendingId, m_pendingVersion, m_publishPath, m_token, m_uploadId;
    std::unique_ptr<share::LoopbackSignIn> m_signIn;
};

std::string g_publish; // --publish

gint onLocalOptions(GApplication *, GVariantDict *options, gpointer)
{
    const char *path = nullptr;
    if (g_variant_dict_lookup(options, "publish", "^&ay", &path) && path) {
        char *utf8 = g_filename_to_utf8(path, -1, nullptr, nullptr, nullptr);
        g_publish = utf8 ? utf8 : "";
        g_free(utf8);
    }
    return -1;
}

void onStartup(GApplication *, gpointer)
{
    // Dark, and the editor's stylesheet: the same brand tokens as the
    // editor and U-Stu Titles (src/app/style/style.css).
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/com/ustudio/Share/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void onActivate(GApplication *app, gpointer)
{
    auto *window = new ShareWindow(GTK_APPLICATION(app), g_publish);
    gtk_window_present(window->window());
}

} // namespace

int main(int argc, char **argv)
{
    Log::init("u-studio-share");
    Log::info("[share] Starting u-studio-share");
    AdwApplication *app = adw_application_new("com.ustudio.Share", G_APPLICATION_NON_UNIQUE);
    g_application_add_main_option(G_APPLICATION(app), "publish", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
                                  "Publish a template pack (shows what it uploads first)", "PACK");
    g_signal_connect(app, "handle-local-options", G_CALLBACK(onLocalOptions), nullptr);
    g_signal_connect(app, "startup", G_CALLBACK(onStartup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
