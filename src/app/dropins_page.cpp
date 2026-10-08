#include "dropins_page.h"

#include "settings.h"
#include "dropins/registry.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ustudio::app {

namespace {

struct PageState
{
    Settings *settings;
    std::function<void()> onChanged;
};

void switchToggled(AdwSwitchRow *row, GParamSpec *, gpointer data);

GtkWidget *textRow(const std::string &title, const std::string &subtitle)
{
    GtkWidget *row = adw_action_row_new();
    // Names, paths and loader messages are text, not Pango markup.
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
    if (!subtitle.empty()) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
        adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE); // package names can be copied
    }
    return row;
}

AdwPreferencesGroup *addGroup(AdwPreferencesPage *page, const char *title, const char *description)
{
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, title);
    if (description)
        adw_preferences_group_set_description(group, description);
    adw_preferences_page_add(page, group);
    return group;
}

} // namespace

AdwPreferencesPage *buildDropInsPage(const dropins::DropInRegistry *registry, Settings &settings,
                                     std::function<void()> onChanged)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Drop-ins");
    adw_preferences_page_set_icon_name(page, "application-x-addon-symbolic");
    auto *state = new PageState{&settings, std::move(onChanged)};
    g_object_set_data_full(G_OBJECT(page), "ustudio-dropins-state", state,
                           [](gpointer data) { delete static_cast<PageState *>(data); });

    const std::vector<dropins::DropInRegistry::Entry> none;
    const auto &entries = registry ? registry->entries() : none;
    const std::vector<std::string> disabled = settings.disabledDropIns();

    AdwPreferencesGroup *installed =
        addGroup(page, "Installed", "Switching a drop-in on or off applies the next time U-Stu starts.");
    if (entries.empty())
        adw_preferences_group_add(installed, textRow("No drop-ins installed", ""));
    for (const dropins::DropInRegistry::Entry &entry : entries) {
        GtkWidget *row = adw_switch_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), entry.name.c_str());
        // A module switched off isn't opened, so only its file is known.
        std::string subtitle = !entry.describe             ? "Off: not loaded this run\n"
                               : entry.description.empty() ? ""
                                                           : entry.description + "\n";
        subtitle += entry.path.empty() ? "Built in" : entry.path;
        if (!entry.version.empty())
            subtitle += " · " + entry.version;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
        const bool on = std::find(disabled.begin(), disabled.end(), entry.name) == disabled.end();
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), on);
        g_object_set_data_full(G_OBJECT(row), "ustudio-dropin-name", g_strdup(entry.name.c_str()), g_free);
        g_signal_connect(row, "notify::active", G_CALLBACK(&switchToggled), state);
        adw_preferences_group_add(installed, row);
    }

    std::vector<std::string> missing;
    if (registry)
        for (const std::string &name : registry->known())
            if (std::none_of(entries.begin(), entries.end(), [&](const auto &e) { return e.name == name; }))
                missing.push_back(name);
    if (!missing.empty()) {
        AdwPreferencesGroup *group =
            addGroup(page, "Not installed", "U-Stu never downloads drop-ins; install them like any other package.");
        for (const std::string &name : missing)
            adw_preferences_group_add(group,
                                      textRow(name, "Install the u-studio-video-editor-dropin-" + name +
                                                        " package or its Flatpak extension, or build with -Ddropin_" +
                                                        name + "=module"));
    }

    if (registry && !registry->refusals().empty()) {
        AdwPreferencesGroup *group = addGroup(page, "Couldn't load", nullptr);
        for (const std::string &refusal : registry->refusals())
            adw_preferences_group_add(group, textRow("Not loaded", refusal));
    }
    return page;
}

namespace {

// --- GTK signal trampolines ---

void switchToggled(AdwSwitchRow *row, GParamSpec *, gpointer data)
{
    auto *state = static_cast<PageState *>(data);
    const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "ustudio-dropin-name"));
    state->settings->setDropInEnabled(name, adw_switch_row_get_active(row));
    if (state->onChanged)
        state->onChanged();
}

} // namespace

} // namespace ustudio::app
