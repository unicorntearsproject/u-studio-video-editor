#include "action_registry.h"

#include "app_window.h"
#include "core/log.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace ustudio::app {

const std::vector<ActionSpec> &actionSpecs()
{
    // clang-format off
    static const std::vector<ActionSpec> kSpecs = {
        // --- Playback (doc 05's M2 transport deliverables) ---
        // Space toggles play/pause, same as every other video editor --
        // shuttle-forward (L) below already covers "start playing
        // forward" too, but Space is the one everyone reaches for first.
        {"play-pause",          "Play/Pause",                 "Playback", {"space"},              &AppWindow::playPauseActivated},
        {"shuttle-forward",     "Shuttle Forward (accelerate)", "Playback", {"l"},                 &AppWindow::shuttleForwardActivated},
        {"shuttle-reverse",     "Shuttle Reverse (accelerate)", "Playback", {"j"},                 &AppWindow::shuttleReverseActivated},
        {"shuttle-stop",        "Shuttle Stop",               "Playback", {"k"},                   &AppWindow::shuttleStopActivated},
        {"step-forward",        "Step Forward One Frame",     "Playback", {"Right"},               &AppWindow::stepForwardActivated},
        {"step-backward",       "Step Backward One Frame",    "Playback", {"Left"},                &AppWindow::stepBackwardActivated},
        {"step-forward-10",     "Step Forward 10 Frames",     "Playback", {"<Control>Right"},      &AppWindow::stepForward10Activated},
        {"step-backward-10",    "Step Backward 10 Frames",    "Playback", {"<Control>Left"},       &AppWindow::stepBackward10Activated},
        {"step-forward-minute", "Step Forward 1 Minute",      "Playback", {"<Alt>Right"},          &AppWindow::stepForwardMinuteActivated},
        {"step-backward-minute","Step Backward 1 Minute",     "Playback", {"<Alt>Left"},           &AppWindow::stepBackwardMinuteActivated},
        {"seek-home",            "Seek to Start",             "Playback", {"Home"},                &AppWindow::seekHomeActivated},
        {"seek-end",             "Seek to End",                "Playback", {"End"},                &AppWindow::seekEndActivated},
        {"loop-set-in",          "Set Loop In",                "Playback", {"i"},                  &AppWindow::loopSetInActivated},
        {"loop-set-out",         "Set Loop Out",               "Playback", {"o"},                  &AppWindow::loopSetOutActivated},

        // --- Editing ---
        {"undo",                 "Undo",                       "Editing", {"<Control>z"},          nullptr},
        {"redo",                 "Redo",                       "Editing", {"<Control><Shift>z"},   nullptr},
        // A/F: previous/next cut on the active track. S/D: active track
        // up/down -- the row edits/imports land on, same as clicking a row.
        {"seek-previous-cut",    "Seek to Previous Cut or Marker", "Editing", {"a"},              &AppWindow::seekPreviousCutActivated},
        {"seek-next-cut",        "Seek to Next Cut or Marker",     "Editing", {"f"},              &AppWindow::seekNextCutActivated},
        {"seek-previous-cut-on-active-track", "Seek to Previous Cut on Active Track", "Editing", {"<Shift>a"}, &AppWindow::seekPreviousCutOnActiveTrackActivated},
        {"seek-next-cut-on-active-track",     "Seek to Next Cut on Active Track",     "Editing", {"<Shift>f"}, &AppWindow::seekNextCutOnActiveTrackActivated},
        {"active-track-up",      "Active Track Up",            "Editing", {"s", "Up"},              &AppWindow::activeTrackUpActivated},
        {"active-track-down",    "Active Track Down",          "Editing", {"d", "Down"},            &AppWindow::activeTrackDownActivated},
        {"active-track-top",     "Move Clip Up a Track / Active Track to Top",       "Editing", {"<Shift>s"}, &AppWindow::activeTrackTopActivated},
        {"active-track-bottom",  "Move Clip Down a Track / Active Track to Bottom", "Editing", {"<Shift>d"}, &AppWindow::activeTrackBottomActivated},
        {"delete-selected-clip", "Delete Selected Clip",       "Editing", {"Delete"},               &AppWindow::deleteSelectedClipActivated},
        {"split-at-playhead",    "Split Clip at Playhead",     "Editing", {"x"},                    &AppWindow::splitAtPlayheadActivated},
        {"ripple-delete-selected", "Ripple Delete Selected",   "Editing", {"<Shift>Delete"},        &AppWindow::rippleDeleteSelectedActivated},
        {"add-marker",           "Add Marker at Playhead",     "Editing", {"m"},                    &AppWindow::addMarkerActivated},
        {"remove-marker",        "Remove Marker at Playhead",  "Editing", {"<Shift>m"},             &AppWindow::removeMarkerActivated},
        // doc 06's keyboard-only editing: walk the clips on the active
        // track, and nudge the selection.
        {"select-next-clip",     "Select Next Clip",           "Editing", {"Tab"},                  &AppWindow::selectNextClipActivated},
        {"select-previous-clip", "Select Previous Clip",       "Editing", {"<Shift>Tab", "<Shift>ISO_Left_Tab"}, &AppWindow::selectPreviousClipActivated},
        {"nudge-left",           "Nudge Selection Left 1 Frame",   "Editing", {"comma"},            &AppWindow::nudgeLeftActivated},
        {"nudge-right",          "Nudge Selection Right 1 Frame",  "Editing", {"period"},           &AppWindow::nudgeRightActivated},
        {"nudge-left-10",        "Nudge Selection Left 10 Frames", "Editing", {"<Shift>comma", "less"}, &AppWindow::nudgeLeft10Activated},
        {"nudge-right-10",       "Nudge Selection Right 10 Frames", "Editing", {"<Shift>period", "greater"}, &AppWindow::nudgeRight10Activated},
        // Stateful (on/off), so installActions() makes it itself.
        {"ripple-mode",          "Ripple Mode On/Off",         "Editing", {"r"},                    nullptr},
        {"select-all",           "Select All Clips",           "Editing", {"<Control>a"},           &AppWindow::selectAllActivated},
        {"clear-selection",      "Clear Selection",            "Editing", {"Escape"},               &AppWindow::clearSelectionActivated},

        // --- Timeline view (doc 06) ---
        {"zoom-in",              "Zoom In",                    "Timeline", {"plus", "equal", "KP_Add"}, &AppWindow::zoomInActivated},
        {"zoom-out",             "Zoom Out",                   "Timeline", {"minus", "KP_Subtract"},   &AppWindow::zoomOutActivated},
        {"zoom-fit",             "Zoom to Fit",                "Timeline", {"0"},                      &AppWindow::zoomFitActivated},

        // --- Project ---
        {"save",         "Save",              "Project", {"<Control>s"},             &AppWindow::saveActionActivated},
        {"save-as",       "Save As…",          "Project", {"<Control><Shift>s"},      &AppWindow::saveAsActivated},
        {"project-frame-rate", "Project Frame Rate and Background…", "Project", {},   &AppWindow::projectFrameRateActivated},
        {"open-project",  "Open Project…",     "Project", {"<Control>o"},             &AppWindow::openProjectActionActivated},
        {"new-project",   "New Project",       "Project", {"<Control>n"},             &AppWindow::newProjectActionActivated},
        {"quit",          "Quit",              "Project", {"<Control>q"},             &AppWindow::quitActionActivated},
        {"import",        "Import…",           "Project", {"<Control>i"},             &AppWindow::importActionActivated},
        {"import-folder", "Import Folder…",    "Project", {"<Control><Shift>i"},      &AppWindow::importFolderActivated},
        {"import-image-sequence", "Import Image Sequence…", "Project", {"<Control><Alt>i"}, &AppWindow::importImageSequenceActivated},

        // --- Help: for bug reports (0.49.0-beta.2) ---
        {"open-log-folder",      "Open Log Folder",            "Help", {},                  &AppWindow::diagnosticsActionActivated},
        {"copy-diagnostics",     "Copy Diagnostics",           "Help", {},                  &AppWindow::diagnosticsActionActivated},

        // --- Transform: the selected clip's picture (M4 F2, ADR-018) ---
        {"transform-edit",       "Edit Transform…",            "Transform", {"<Control>t"}, &AppWindow::transformActionActivated},
        {"transform-reset",      "Reset Transform",            "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-fit",        "Fit to Frame",               "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-stretch",    "Stretch to Frame",           "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-centre",     "Centre",                     "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-centre-h",   "Centre Horizontally",        "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-centre-v",   "Centre Vertically",          "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-flip-h",     "Flip Horizontally",          "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-flip-v",     "Flip Vertically",            "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-rotate-cw",  "Rotate 90° Clockwise",       "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-rotate-ccw", "Rotate 90° Anticlockwise",   "Transform", {},             &AppWindow::transformActionActivated},
        {"transform-rotate-180", "Rotate 180°",                "Transform", {},             &AppWindow::transformActionActivated},
    };
    // clang-format on
    return kSpecs;
}

namespace {

std::vector<ContributedAction> &contributions()
{
    static std::vector<ContributedAction> list;
    return list;
}

// Accelerators compared as GTK parses them ("<Ctrl>s" == "<Control>s").
bool sameAccel(const char *a, const char *b)
{
    guint keyA = 0, keyB = 0;
    GdkModifierType modsA{}, modsB{};
    if (!gtk_accelerator_parse(a, &keyA, &modsA) || !gtk_accelerator_parse(b, &keyB, &modsB))
        return std::strcmp(a, b) == 0;
    return keyA == keyB && modsA == modsB;
}

} // namespace

std::vector<ContributedAction> contributeActions(const std::vector<ActionSpec> &specs, gpointer target)
{
    std::vector<ContributedAction> accepted;
    for (const ActionSpec &spec : specs) {
        const std::string name = spec.name != nullptr ? spec.name : "";
        if (name.empty() || spec.label == nullptr || spec.category == nullptr || spec.activated == nullptr) {
            core::Log::warn("[actions] refused contributed action '" + name +
                            "': needs a name, label, category and handler");
            continue;
        }
        std::vector<ActionSpec> existing = allActionSpecs();
        if (std::any_of(existing.begin(), existing.end(), [&](const ActionSpec &e) { return name == e.name; })) {
            core::Log::warn("[actions] refused contributed action '" + name + "': the name is taken");
            continue;
        }
        ActionSpec kept = spec;
        kept.accels.clear();
        for (const char *accel : spec.accels) {
            bool taken = std::any_of(existing.begin(), existing.end(), [&](const ActionSpec &e) {
                return std::any_of(e.accels.begin(), e.accels.end(),
                                   [&](const char *a) { return sameAccel(a, accel); });
            });
            if (taken)
                core::Log::warn("[actions] '" + name + "' can't have " + accel + ": another action has it");
            else
                kept.accels.push_back(accel);
        }
        contributions().push_back({kept, target});
        accepted.push_back({kept, target});
    }
    return accepted;
}

const std::vector<ContributedAction> &contributedActions()
{
    return contributions();
}

std::vector<ActionSpec> allActionSpecs()
{
    std::vector<ActionSpec> all = actionSpecs();
    for (const ContributedAction &action : contributions())
        all.push_back(action.spec);
    return all;
}

void clearContributedActions()
{
    contributions().clear();
}

} // namespace ustudio::app
