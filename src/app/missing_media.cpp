// M4 B (doc 07, "Missing media and relink"): the banner that says media is
// missing, the relink dialog, and relinking itself. Assets are marked
// Missing when a project loads (core::markMissingMedia, on the pool with
// the parse) or when the engine couldn't open one; their clips play the
// placeholder meanwhile. Relinking is one RelinkAsset command per asset,
// together one undo step, changing nothing but path, fingerprint and status.

#include "app_window.h"

#include "core/media/image_sequence.h"
#include "core/media/media_search.h"
#include "core/media/missing_media.h"
#include "platform/files.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/log.h"
#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"
#include "engine/dispatcher.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {

std::string fileNameOf(const std::string &path)
{
    return core::utf8String(core::pathFromUtf8(path).filename());
}

} // namespace

std::vector<core::AssetId> AppWindow::missingAssets(bool usedOnly) const
{
    std::vector<core::AssetId> missing;
    for (const core::Asset &asset : m_model.project().bin) {
        if (asset.status != core::Asset::Status::Missing)
            continue;
        if (usedOnly) {
            const auto &clips = m_model.sequence().clips;
            if (std::none_of(clips.begin(), clips.end(),
                             [&](const auto &entry) { return entry.second.asset == asset.id; }))
                continue;
        }
        missing.push_back(asset.id);
    }
    return missing;
}

void AppWindow::refreshMissingBanner()
{
    const size_t count = missingAssets(false).size();
    if (count == 0 && !m_missingBanner)
        return; // nothing missing, and never was this run: no banner at all
    if (!m_missingBanner) {
        m_missingBanner = ADW_BANNER(adw_banner_new(""));
        adw_banner_set_button_label(m_missingBanner, "Relink…");
        g_signal_connect_swapped(m_missingBanner, "button-clicked",
                                 G_CALLBACK(+[](gpointer self) { static_cast<AppWindow *>(self)->showRelinkDialog(); }),
                                 this);
        adw_toolbar_view_add_top_bar(m_toolbarView, GTK_WIDGET(m_missingBanner));
    }
    const std::string title = count == 1 ? "1 media file is missing: its clips show red"
                                         : std::to_string(count) + " media files are missing: their clips show red";
    adw_banner_set_title(m_missingBanner, title.c_str());
    adw_banner_set_revealed(m_missingBanner, count > 0);
    if (m_relinkDialog)
        refreshRelinkDialog();
}

void AppWindow::onMediaUnavailable(const std::string &path)
{
    // The engine couldn't open a file the load check found (it moved since,
    // or it isn't media any more): the same as missing from here on.
    bool marked = false;
    for (const core::Asset &asset : m_model.project().bin) {
        if (asset.path == path && asset.status != core::Asset::Status::Missing) {
            m_model.setAssetStatus(asset.id, core::Asset::Status::Missing);
            marked = true;
        }
    }
    showStatus("Couldn't open \"" + fileNameOf(path) + "\": its clips show red until it's relinked.");
    if (marked) {
        refreshTimeline();
        refreshMediaBrowser();
        refreshMissingBanner();
    }
}

void AppWindow::showRelinkDialog()
{
    if (m_relinkDialog) {
        adw_dialog_present(m_relinkDialog, GTK_WIDGET(m_window));
        return;
    }
    m_relinkDialog = adw_dialog_new();
    adw_dialog_set_title(m_relinkDialog, "Relink Media");
    adw_dialog_set_content_width(m_relinkDialog, 620);
    adw_dialog_set_content_height(m_relinkDialog, 440);

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    m_relinkGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(m_relinkGroup, "Missing files");
    adw_preferences_group_set_description(m_relinkGroup,
                                          "Point each one at where the file is now. Relinking changes nothing else "
                                          "in the project, and undoes in one step.");
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *findAuto = gtk_button_new_with_label("Find Automatically");
    gtk_widget_add_css_class(findAuto, "flat");
    setTooltip(findAuto, "relink.find-automatically");
    g_signal_connect_swapped(
        findAuto, "clicked",
        G_CALLBACK(+[](gpointer self) { static_cast<AppWindow *>(self)->findMissingAutomatically(); }), this);
    gtk_box_append(GTK_BOX(buttons), findAuto);
    GtkWidget *search = gtk_button_new_with_label("Search a Folder…");
    gtk_widget_add_css_class(search, "flat");
    setTooltip(search, "relink.search-folder");
    g_signal_connect_swapped(
        search, "clicked",
        G_CALLBACK(+[](gpointer self) { static_cast<AppWindow *>(self)->chooseFolderToSearchForMissing(); }), this);
    gtk_box_append(GTK_BOX(buttons), search);
    adw_preferences_group_set_header_suffix(m_relinkGroup, buttons);
    adw_preferences_page_add(page, m_relinkGroup);

    GtkWidget *toolbarView = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), GTK_WIDGET(page));
    adw_dialog_set_child(m_relinkDialog, toolbarView);
    g_signal_connect(m_relinkDialog, "closed", G_CALLBACK(&AppWindow::relinkDialogClosedTrampoline), this);
    refreshRelinkDialog();
    adw_dialog_present(m_relinkDialog, GTK_WIDGET(m_window));
}

void AppWindow::refreshRelinkDialog()
{
    for (GtkWidget *row : m_relinkRows)
        adw_preferences_group_remove(m_relinkGroup, row);
    m_relinkRows.clear();
    const std::vector<core::AssetId> missing = missingAssets(false);
    if (missing.empty()) {
        adw_dialog_close(m_relinkDialog); // everything found
        return;
    }
    for (core::AssetId id : missing) {
        const core::Asset &asset = m_model.asset(id);
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      (asset.displayName.empty() ? fileNameOf(asset.path) : asset.displayName).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), asset.path.c_str());
        GtkWidget *locate = gtk_button_new_with_label("Locate…");
        gtk_widget_set_valign(locate, GTK_ALIGN_CENTER);
        setTooltip(locate, "relink.locate");
        g_object_set_data(G_OBJECT(locate), "ustudio-asset", reinterpret_cast<gpointer>(id.value));
        g_signal_connect(
            locate, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer self) {
                auto *window = static_cast<AppWindow *>(self);
                const core::AssetId picked{
                    reinterpret_cast<uint64_t>(g_object_get_data(G_OBJECT(button), "ustudio-asset"))};
                if (!window->m_model.hasAsset(picked))
                    return;
                GtkFileDialog *dialog = gtk_file_dialog_new();
                gtk_file_dialog_set_title(dialog, "Locate Missing Media");
                gtk_file_dialog_set_initial_name(dialog, fileNameOf(window->m_model.asset(picked).path).c_str());
                struct Pick
                {
                    AppWindow *window;
                    core::AssetId asset;
                };
                gtk_file_dialog_open(
                    dialog, GTK_WINDOW(window->m_window), nullptr,
                    [](GObject *source, GAsyncResult *result, gpointer data) {
                        std::unique_ptr<Pick> pick(static_cast<Pick *>(data));
                        GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
                        if (!file)
                            return;
                        char *path = g_file_get_path(file);
                        g_object_unref(file);
                        if (path)
                            pick->window->relinkLocated(pick->asset, path);
                        g_free(path);
                    },
                    new Pick{window, picked});
                g_object_unref(dialog);
            }),
            this);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), locate);
        adw_preferences_group_add(m_relinkGroup, row);
        m_relinkRows.push_back(row);
    }
}

void AppWindow::searchFolderForMissing(const std::string &folder)
{
    struct Wanted
    {
        core::AssetId id;
        std::string name, fingerprint;
    };
    std::vector<Wanted> wanted;
    for (core::AssetId id : missingAssets(false)) {
        const core::Asset &asset = m_model.asset(id);
        // An image sequence is looked for by its first file (M4 E).
        const std::string file =
            asset.info.isImageSequence ? core::imageSequenceFile(asset.path, asset.info.sequenceBegin) : asset.path;
        wanted.push_back({id, fileNameOf(file), asset.fileFingerprint});
    }
    if (wanted.empty())
        return;
    showStatus("Searching " + folder + " for missing media…");
    // Pool: walk the folder, match by file name, then by fingerprint among
    // several with that name (the same file, most likely).
    m_pool->submit(
        [this, folder, wanted = std::move(wanted), token = std::weak_ptr<void>(m_lifetime)](std::stop_token) {
            bool truncated = false;
            const std::vector<std::string> files = expandImportPaths({folder}, 200'000, &truncated);
            std::multimap<std::string, std::string> byName;
            for (const std::string &file : files)
                byName.emplace(fileNameOf(file), file);
            std::vector<std::pair<core::AssetId, std::string>> found;
            for (const Wanted &want : wanted) {
                auto [first, last] = byName.equal_range(want.name);
                if (first == last)
                    continue;
                std::string choice = first->second;
                for (auto it = first; it != last; ++it)
                    if (!want.fingerprint.empty() && core::fileFingerprint(it->second) == want.fingerprint) {
                        choice = it->second;
                        break;
                    }
                found.emplace_back(want.id, std::move(choice));
            }
            engine::MainThreadDispatcher::post(token, [this, found = std::move(found), folder]() mutable {
                if (found.empty()) {
                    showStatus("None of the missing files are in " + folder + ".");
                    return;
                }
                relinkTo(std::move(found));
            });
        });
}

void AppWindow::chooseFolderToSearchForMissing()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Search a Folder for Missing Media");
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!folder)
                return;
            char *path = g_file_get_path(folder);
            g_object_unref(folder);
            if (path)
                static_cast<AppWindow *>(data)->searchFolderForMissing(path);
            g_free(path);
        },
        this);
    g_object_unref(dialog);
}

void AppWindow::offerMissingMediaHelp()
{
    const std::vector<core::AssetId> missing = missingAssets(true);
    if (missing.empty())
        return;
    std::string names;
    for (size_t i = 0; i < missing.size() && i < 4; ++i) {
        const core::Asset &asset = m_model.asset(missing[i]);
        names += (i > 0 ? ", " : "") + (asset.displayName.empty() ? fileNameOf(asset.path) : asset.displayName);
    }
    if (missing.size() > 4)
        names += " and " + std::to_string(missing.size() - 4) + " more";
    const std::string heading =
        missing.size() == 1 ? "A media file is missing" : std::to_string(missing.size()) + " media files are missing";
    const std::string body = names + ". " + (missing.size() == 1 ? "Its clips show" : "Their clips show") +
                             " red until found. Find Automatically looks in the project's folder, where the files "
                             "were, your Videos, Pictures and Music, your home folder and mounted drives.";
    AdwDialog *dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "later", "Not Now", "locate", "Locate…", "folder",
                                   "Search a Folder…", "auto", "Find Automatically", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "auto", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "auto");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "later");
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            auto *self = static_cast<AppWindow *>(data);
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response == "auto")
                self->findMissingAutomatically();
            else if (response == "folder")
                self->chooseFolderToSearchForMissing();
            else if (response == "locate")
                self->showRelinkDialog();
        },
        this);
}

void AppWindow::findMissingAutomatically()
{
    if (m_findMissingDialog)
        return; // already searching
    std::vector<core::WantedMedia> wanted;
    core::MediaSearchPlaces places;
    if (!m_currentProjectPath.empty())
        places.projectFolder = core::utf8String(core::pathFromUtf8(m_currentProjectPath).parent_path());
    for (core::AssetId id : missingAssets(false)) {
        const core::Asset &asset = m_model.asset(id);
        const std::string file =
            asset.info.isImageSequence ? core::imageSequenceFile(asset.path, asset.info.sequenceBegin) : asset.path;
        wanted.push_back({id.value, fileNameOf(file), asset.fileFingerprint});
        places.missingFolders.push_back(core::utf8String(core::pathFromUtf8(file).parent_path()));
    }
    if (wanted.empty())
        return;
    for (const core::Asset &asset : m_model.project().bin)
        if (asset.status != core::Asset::Status::Missing && core::isFileResource(asset.path))
            places.mediaFolders.push_back(core::utf8String(core::pathFromUtf8(asset.path).parent_path()));
    for (GUserDirectory kind : {G_USER_DIRECTORY_VIDEOS, G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_MUSIC})
        if (const char *folder = g_get_user_special_dir(kind))
            places.userFolders.push_back(folder);
    places.home = g_get_home_dir();
    for (const std::filesystem::path &mount : platform::mountedVolumeRoots())
        places.mounts.push_back(core::utf8String(mount));
    const std::vector<std::string> roots = core::mediaSearchRoots(places);

    // The progress dialog: where it's looking now, and Cancel.
    m_findMissingCancel = std::make_shared<std::atomic<bool>>(false);
    m_findMissingDialog = adw_alert_dialog_new("Finding Missing Media", nullptr);
    m_findMissingLabel = GTK_LABEL(gtk_label_new("Starting…"));
    gtk_label_set_ellipsize(m_findMissingLabel, PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_width_chars(m_findMissingLabel, 40);
    gtk_widget_add_css_class(GTK_WIDGET(m_findMissingLabel), "dim-label");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(m_findMissingDialog), GTK_WIDGET(m_findMissingLabel));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(m_findMissingDialog), "cancel", "Cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(m_findMissingDialog), "cancel");
    g_signal_connect(m_findMissingDialog, "closed", G_CALLBACK(+[](AdwDialog *, gpointer data) {
                         auto *self = static_cast<AppWindow *>(data);
                         if (self->m_findMissingCancel)
                             self->m_findMissingCancel->store(true); // closed by Cancel, or by the result
                         self->m_findMissingDialog = nullptr;
                         self->m_findMissingLabel = nullptr;
                     }),
                     this);
    adw_dialog_present(m_findMissingDialog, GTK_WIDGET(m_window));
    showStatus("Finding missing media…");

    const uint64_t generation = m_projectGeneration;
    const size_t total = wanted.size();
    m_pool->submit([this, wanted = std::move(wanted), roots, cancel = m_findMissingCancel, generation, total,
                    token = std::weak_ptr<void>(m_lifetime)](std::stop_token) {
        auto last = std::chrono::steady_clock::time_point{};
        const std::map<uint64_t, std::string> found =
            core::findMediaFiles(wanted, roots, cancel.get(), [&](const core::MediaSearchProgress &progress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::milliseconds(150))
                    return;
                last = now;
                std::string text =
                    progress.folder + "\n" + std::to_string(progress.found) + " of " + std::to_string(total) + " found";
                engine::MainThreadDispatcher::post(token, [this, cancel, text = std::move(text)] {
                    if (m_findMissingLabel && m_findMissingCancel == cancel)
                        gtk_label_set_text(m_findMissingLabel, text.c_str());
                });
            });
        const bool cancelled = cancel->load();
        engine::MainThreadDispatcher::post(token, [this, found, cancel, cancelled, generation, total] {
            if (m_findMissingDialog && m_findMissingCancel == cancel)
                adw_dialog_close(m_findMissingDialog);
            if (generation != m_projectGeneration)
                return; // another project now
            if (cancelled && found.empty()) {
                showStatus("Stopped looking for the missing media.");
                return;
            }
            Log::info("[relink] Find automatically: " + std::to_string(found.size()) + " of " + std::to_string(total) +
                      " found" + (cancelled ? " (cancelled)" : ""));
            if (found.empty()) {
                showStatus("None of the missing files were found. Try Search a Folder or Locate.");
                return;
            }
            std::vector<std::pair<core::AssetId, std::string>> candidates;
            for (const auto &[id, path] : found)
                candidates.emplace_back(core::AssetId{id}, path);
            relinkTo(std::move(candidates));
        });
    });
}

void AppWindow::relinkLocated(core::AssetId asset, const std::string &path)
{
    // The others may well be beside it: look in that folder (and below) for
    // them, and offer them in the same relink.
    const std::string folder = core::utf8String(core::pathFromUtf8(path).parent_path());
    std::vector<core::WantedMedia> others;
    for (core::AssetId id : missingAssets(false)) {
        if (id == asset)
            continue;
        const core::Asset &missing = m_model.asset(id);
        const std::string file = missing.info.isImageSequence
                                     ? core::imageSequenceFile(missing.path, missing.info.sequenceBegin)
                                     : missing.path;
        others.push_back({id.value, fileNameOf(file), missing.fileFingerprint});
    }
    if (others.empty()) {
        relinkTo({{asset, path}});
        return;
    }
    const uint64_t generation = m_projectGeneration;
    m_pool->submit([this, asset, path, folder, others = std::move(others), generation,
                    token = std::weak_ptr<void>(m_lifetime)](std::stop_token) {
        const std::map<uint64_t, std::string> found = core::findMediaFiles(others, {folder}, nullptr, {}, 200'000);
        engine::MainThreadDispatcher::post(token, [this, asset, path, folder, found, generation] {
            if (generation != m_projectGeneration)
                return;
            if (found.empty()) {
                relinkTo({{asset, path}});
                return;
            }
            struct Offer
            {
                AppWindow *window;
                std::vector<std::pair<core::AssetId, std::string>> picked, others;
            };
            auto *offer = new Offer{this, {{asset, path}}, {}};
            for (const auto &[id, other] : found)
                offer->others.emplace_back(core::AssetId{id}, other);
            const std::string heading = found.size() == 1
                                            ? "Another missing file is there"
                                            : std::to_string(found.size()) + " more missing files are there";
            const std::string body = "Found in " + folder + ". Relink them too, in the same step?";
            AdwDialog *dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
            adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "one", "Just This One", "all", "Relink All",
                                           nullptr);
            adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "all", ADW_RESPONSE_SUGGESTED);
            adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "all");
            adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "one");
            adw_alert_dialog_choose(
                ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
                [](GObject *source, GAsyncResult *result, gpointer data) {
                    std::unique_ptr<Offer> owned(static_cast<Offer *>(data));
                    const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
                    if (response == "all")
                        owned->picked.insert(owned->picked.end(), owned->others.begin(), owned->others.end());
                    owned->window->relinkTo(std::move(owned->picked));
                },
                offer);
        });
    });
}

void AppWindow::relinkTo(std::vector<std::pair<core::AssetId, std::string>> candidates)
{
    struct Checked
    {
        core::AssetId id;
        std::string path, fingerprint;
        engine::EngineSync::ProbedMedia probed;
        std::optional<int> sequenceBegin; // an image sequence (M4 E): its first file number
        bool notASequence = false;
    };
    std::set<uint64_t> sequences;
    for (const auto &[id, path] : candidates)
        if (m_model.hasAsset(id) && m_model.asset(id).info.isImageSequence)
            sequences.insert(id.value);
    const uint64_t generation = m_projectGeneration;
    m_pool->submit([this, candidates = std::move(candidates), sequences, profile = m_model.sequence().profile,
                    generation, token = std::weak_ptr<void>(m_lifetime)](std::stop_token) {
        std::vector<Checked> checked;
        for (const auto &[id, path] : candidates) {
            if (!sequences.contains(id.value)) {
                checked.push_back({id,
                                   path,
                                   core::fileFingerprint(path),
                                   engine::EngineSync::probeMediaFile(profile, path),
                                   {},
                                   false});
                continue;
            }
            // A sequence: any of its files was picked (or its first found).
            const std::optional<core::ImageSequence> sequence = core::findImageSequence(path);
            if (!sequence) {
                checked.push_back({id, path, {}, {}, {}, true});
                continue;
            }
            const std::string first = core::imageSequenceFile(sequence->pattern, sequence->begin);
            engine::EngineSync::ProbedMedia probed = engine::EngineSync::probeMediaFile(profile, first);
            if (probed.length > 0)
                probed.length = sequence->count; // one frame per file
            probed.isStillImage = false;
            checked.push_back({id, sequence->pattern, core::fileFingerprint(first), probed, sequence->begin, false});
        }
        engine::MainThreadDispatcher::post(token, [this, checked = std::move(checked), generation] {
            if (generation != m_projectGeneration)
                return; // another project now
            std::vector<std::unique_ptr<core::Command>> steps;
            std::vector<std::string> problems;
            // A proxy of another file's content is stale (M4 C): dropped
            // once the relink lands.
            std::vector<core::AssetId> staleProxies;
            for (const Checked &check : checked) {
                if (!m_model.hasAsset(check.id))
                    continue;
                const core::Asset &asset = m_model.asset(check.id);
                const std::string name = fileNameOf(check.path);
                if (check.notASequence) {
                    problems.push_back(name + " isn't part of a numbered image sequence");
                    continue;
                }
                if (check.probed.length <= 0) {
                    problems.push_back(name + " can't be opened");
                    continue;
                }
                // Long enough for every clip cut from it (a still has no end).
                core::FrameIndex needed = 0;
                for (const auto &[clipId, clip] : m_model.sequence().clips)
                    if (clip.asset == check.id)
                        needed = std::max(needed, clip.out + 1);
                if (!check.probed.isStillImage && !asset.info.isBoundless() && check.probed.length < needed) {
                    problems.push_back(name + " is shorter (" + formatTimecode(static_cast<int>(check.probed.length)) +
                                       ") than its clips need (" + formatTimecode(static_cast<int>(needed)) + ")");
                    continue;
                }
                if (!asset.proxyPath.empty() && asset.fileFingerprint != check.fingerprint)
                    staleProxies.push_back(check.id);
                steps.push_back(
                    std::make_unique<core::RelinkAsset>(check.id, check.path, check.fingerprint, check.sequenceBegin));
            }
            const size_t relinked = steps.size();
            if (relinked > 0 &&
                !m_undoStack.execute(std::make_unique<core::CompositeCommand>("Relink media", std::move(steps)))) {
                showStatus("Couldn't relink the media.");
                return;
            }
            for (core::AssetId id : staleProxies)
                m_model.setAssetProxy(id, "");
            if (!staleProxies.empty())
                m_undoStack.markDirty();
            std::string status =
                relinked == 1 ? "Relinked 1 file." : "Relinked " + std::to_string(relinked) + " files.";
            if (!problems.empty())
                status += " Not relinked: " + problems.front() +
                          (problems.size() > 1 ? " (and " + std::to_string(problems.size() - 1) + " more)." : ".");
            Log::info("[relink] " + status);
            showStatus(status);
            refreshTimeline();
            refreshMediaBrowser();
            refreshMissingBanner();
        });
    });
}

// --- GTK signal trampolines ---

void AppWindow::relinkDialogClosedTrampoline(AdwDialog *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_relinkDialog = nullptr;
    self->m_relinkGroup = nullptr;
    self->m_relinkRows.clear();
}

} // namespace ustudio::app
