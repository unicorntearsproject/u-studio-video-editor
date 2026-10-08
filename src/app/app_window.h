#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <thread>
#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "action_registry.h"
#include "autosave.h"
#include "render_profiles.h"
#include "render_queue.h"
#include "core/commands/undo_stack.h"
#include "core/concurrency/thread_pool.h"
#include "core/model/model.h"
#include "core/xml/reader.h"
#include "engine/dispatcher.h"
#include "core/audio/align.h"
#include "engine/engine.h"
#include "engine/engine_sync.h"
#include "engine/thumbnail_cache.h"
#include "engine/waveform_cache.h"
#include "import_queue.h"
#include "gpu_acceleration.h"
#include "proxy_queue.h"
#include "project_loader.h"
#include "save_queue.h"
#include "settings.h"
#include "shell_host.h"
#include "dropins/dropin_host.h"
#include "timeline/timeline_controller.h"
#include "timeline/texture_cache.h"
#include "timeline/timeline_renderer.h"
#include "timeline/viewport.h"
#include "media_badges.h"
#include "transform_gestures.h"

namespace ustudio::app {

// What the inline name-edit popover (double-click a track label or a
// clip) is currently open for, if anything.
enum class InlineEditKind
{
    None,
    Track,
    Clip,
};

// A row/frame-index view of one model clip, rebuilt from Model on every
// refreshTimeline() call -- what the timeline actually draws and hit-tests
// against. `trackIndex` is a position in model->sequence().tracks (row 0 =
// top), not a TrackId; `id` is the model's stable ClipId, used to build
// Commands. Kept close to v1's ClipInfo shape deliberately, so the
// drag/draw/hit-test logic below (unchanged from v1) still applies to it.
struct ClipDisplay
{
    core::ClipId id;
    std::string name;
    std::string resource; // asset path, for waveform lookups
    int trackIndex = 0;
    int startFrame = 0;
    int frames = 0;
    int in = 0;
    int out = 0;
};

// The app shell: owns the project Model, its UndoStack, the EngineSync
// projection of it into MLT, a PlaybackController that plays whatever
// tractor EngineSync last built (doc 05/ADR-002), and every top-level
// widget. UI widgets are built imperatively in C++ (no .ui/GResource
// files) to keep the build to a single translation unit per concern and
// avoid an extra resource-compile step for this first milestone.
//
// Edit flow: a UI gesture builds a core::Command, UndoStack::execute()
// applies it to the model, and UndoStack::changed publishes the model's
// snapshot to the engine thread (doc 02: "Model -> engine -> screen, never
// backwards"; doc 19 MT2). The engine thread rebuilds the graph and swaps
// it into playback on its own; this window only ever talks to
// engine::Engine, whose state it reads from a main-thread mirror.
class AppWindow : public ShellHost
{
    // action_registry.cpp's table stores pointers to the private static
    // trampolines below (playPauseActivated and friends) -- see
    // action_registry.h's own comment for why they live in one shared
    // table instead of two separately-maintained lists.
    friend const std::vector<ActionSpec> &actionSpecs();

  public:
    // `shellExtensions`: the drop-ins' (doc 15 IP5), called once the UI is
    // built.
    explicit AppWindow(GtkApplication *app, const std::vector<dropins::ShellExtension> &shellExtensions = {});

    // ShellHost (shell_host.h; implemented in shell_hosts.cpp).
    const core::Model &model() const override
    {
        return m_model;
    }
    bool execute(std::unique_ptr<core::Command> command) override;
    core::Signal<> &projectChanged() override
    {
        return m_shellProjectChanged;
    }
    core::FrameIndex currentFrame() const override;
    void seek(core::FrameIndex frame) override;
    void showInspectorPage(const char *id) override;
    // The docking inspector's window minimum (shell_hosts.cpp): measured on
    // the content and pinned; again after every change that can widen it.
    std::pair<int, int> pinInspectorMinimum();
    core::Signal<> &playheadMoved() override
    {
        return m_shellPlayheadMoved;
    }
    void showStatus(const std::string &text) override;
    ShellSelection currentSelection() const override;
    core::Signal<> &selectionChanged() override
    {
        return m_shellSelectionChanged;
    }
    void addInspectorPage(const InspectorPage &page) override;
    void addActions(const std::vector<ActionSpec> &specs, gpointer target) override;
    void addHints(const std::vector<HintSpec> &hints) override;
    void setTooltip(GtkWidget *widget, const char *hintId) override
    {
        app::setTooltip(widget, hintId);
    }
    void addPreviewOverlay(GtkWidget *overlay) override;
    PreviewMapping previewMapping() const override;
    void redrawPreviewOverlays() override;
    void addTimelineOverlay(timeline::TimelineOverlayProvider *provider) override;
    void redrawTimeline() override;
    void addImportHandler(ImportHandler handler) override;
    void assetChangedOnDisk(core::AssetId asset) override;
    std::string projectFolder() const override;
    void addHeaderButton(GtkWidget *button) override;

    GtkWidget *widget() const
    {
        return GTK_WIDGET(m_window);
    }

    // Stops the playback worker thread before Factory::close() runs on
    // quit (main.cpp's "shutdown" handler). AppWindow itself is never
    // destroyed, so its members' destructors are not the shutdown path.
    void prepareForShutdown();

  private:
    void buildUi(GtkApplication *app);
    void installActions(GtkApplication *app);
    // Enhancement #5: gtk_file_dialog_open_multiple() -- onFileOpened()
    // imports every selected file, each appended after the previous one on
    // the active track.
    void onImportClicked();
    void onFileOpened(GObject *sourceObject, GAsyncResult *result);
    // doc 19 MT1: every import -- the Import dialog, a timeline drop, a
    // media-browser drop -- probes its files in parallel on m_pool through
    // m_importQueue and applies them here on the main thread, in the order
    // given. With no `trackId` the files go to the project bin only; with
    // one and no `position` each is appended after the track's last clip;
    // with a `position` the first lands there and each next one right after
    // the one before. A file that fails is reported and skipped.
    void startImport(std::vector<std::string> paths, std::optional<core::TrackId> trackId,
                     std::optional<core::FrameIndex> position);
    // Computes the insert length, refuses if the target range isn't free
    // (audit-C2-style dynamic overlap check, same as onTimelineDrop() below)
    // and runs an AddAsset+InsertClip CompositeCommand. Returns the new
    // clip's end, or why it couldn't be imported.
    // `batchKey`: the import these are part of, one undo step together.
    std::expected<core::FrameIndex, std::string> importProbedToTrack(const std::string &path,
                                                                     const engine::EngineSync::ProbedMedia &probed,
                                                                     core::TrackId trackId, core::FrameIndex position,
                                                                     uint64_t batchKey);
    uint64_t m_importBatchSerial = 0;
    // Import Folder… reads at most this many files (the rest are noted).
    static constexpr size_t kImportFileLimit = 5000;
    // A multi-file import with failures: every failed file and why, not
    // just the first (M4 acceptance).
    void showImportReport(size_t imported, size_t total, const std::vector<std::string> &failures,
                          const std::string &notes);
    void onImportFolderClicked();
    void onImportImageSequenceClicked(); // M4 E
    void importImageSequence(const std::string &pickedFile);

    // M4 B, missing media (app/missing_media.cpp). Assets marked Missing on
    // load (core::markMissingMedia) or when the engine couldn't open them.
    // The banner and relink dialog exist only once something is missing.
    std::vector<core::AssetId> missingAssets(bool usedOnly) const;
    // " 2 media files are missing.", for the Opened status; "" if none.
    std::string missingMediaNotice() const;
    void refreshMissingBanner();
    void showRelinkDialog();
    void refreshRelinkDialog();
    // Probes each candidate on the pool (it must open and be long enough
    // for the clips using it), then relinks the good ones as one command.
    void relinkTo(std::vector<std::pair<core::AssetId, std::string>> candidates);
    void searchFolderForMissing(const std::string &folder);
    // After a project opens with missing media: one dialog with every way to
    // find it (Find Automatically, Search a Folder, Locate, Not Now).
    void offerMissingMediaHelp();
    // Searches the logical places (core::mediaSearchRoots()) on the pool,
    // with progress and Cancel, and relinks what it finds as one step.
    void findMissingAutomatically();
    // The folder picker for Search a Folder.
    void chooseFolderToSearchForMissing();
    // A Locate pick: the other missing files found beside it are offered
    // too, so the whole relink is one undo step.
    void relinkLocated(core::AssetId asset, const std::string &path);
    void onMediaUnavailable(const std::string &path);
    // M4 F2, transform handles over the preview (app/transform_overlay.cpp).
    void setUpTransformOverlay();
    void drawTransformOverlay(cairo_t *cr);
    // The one selected clip, when its picture shows at the current frame.
    std::optional<gestures::VisibleClip> transformTarget() const;
    void onTransformDragBegin(double x, double y);
    void onTransformDragUpdate(double dx, double dy);
    void onTransformDragEnd();
    void onTransformMotion(double x, double y);
    bool onTransformKey(guint keyval, GdkModifierType state);
    bool applyTransform(core::ClipId clip, const core::Transform &transform, uint64_t gesture);
    void noteTransformSelection();
    void endTransformDrag();
    void setPreviewOwnsArrows(bool owns);
    // The actions, the right-click menu and the dialog (app/transform_actions.cpp).
    std::optional<core::ClipId> transformActionTarget();
    core::Transform explicitTransformOf(core::ClipId clip) const;
    void runTransformAction(const std::string &name);
    void buildTransformMenu();
    void onTransformMenuRequested(double x, double y);
    void showEditTransformDialog();
    GtkWidget *m_transformMenu = nullptr;
    void refreshTransformDialog();
    GtkWidget *m_transformDialog = nullptr;
    void *m_transformDialogState = nullptr; // EditTransformState, owned by m_transformDialog
    GtkWidget *m_transformOverlay = nullptr;
    GtkGesture *m_transformDragGesture = nullptr;
    struct TransformDrag
    {
        core::ClipId clip;
        gestures::DragStart start;
        uint64_t gesture = 0;
        double widgetX = 0, widgetY = 0; // where it began
    };
    std::optional<TransformDrag> m_transformDrag;
    std::vector<gestures::Guide> m_transformGuides;
    core::ClipId m_transformHover;
    core::ClipId m_transformShownSelection;
    uint64_t m_nextTransformGesture = 1;
    bool m_applyingTransform = false;
    // M4 C, proxies (app/proxies.cpp).
    void setUpProxies();
    void buildProxyToggle(GtkWidget *transport);
    void addProxyMenuItems(GtkWidget *menuBox);
    void updateProxyMenuItems();
    void addProxySettingsRow(AdwPreferencesGroup *group);
    void createProxies(const std::vector<core::AssetId> &assets, int height);
    void onProxyDone(core::AssetId asset, const std::string &output);
    void removeProxy(core::AssetId asset);
    // After an import: sources taller than 1080 are offered proxies once per
    // project, or get them if the project said "always".
    void offerProxiesAfterImport();
    std::unique_ptr<ProxyQueue> m_proxyQueue;
    GtkWidget *m_proxyToggle = nullptr;
    // ADR-019 (gpu_ui.cpp): the GPU pipeline, and the open Settings dialog's
    // GPU row (a weak pointer: null once the dialog closes).
    void setUpGpu();
    void refreshGpuSettingsRow();
    std::unique_ptr<GpuAcceleration> m_gpu;
    GtkWidget *m_gpuSettingsRow = nullptr;
    GtkWidget *m_createProxyButton = nullptr;
    GtkWidget *m_conformProxyButton = nullptr;
    GtkWidget *m_removeProxyButton = nullptr;

    AdwBanner *m_missingBanner = nullptr;
    AdwDialog *m_relinkDialog = nullptr;
    AdwPreferencesGroup *m_relinkGroup = nullptr;
    std::vector<GtkWidget *> m_relinkRows;
    static void relinkDialogClosedTrampoline(AdwDialog *dialog, gpointer userData);
    // Find Automatically's progress dialog while it runs, and its cancel flag.
    AdwDialog *m_findMissingDialog = nullptr;
    GtkLabel *m_findMissingLabel = nullptr;
    std::shared_ptr<std::atomic<bool>> m_findMissingCancel;
    // Enhancement #7 (media-browser half): adds the file to the project bin
    // only -- no clip, no track needed.
    // Doc 13 R7: the profile an import should set first (the first video
    // into an empty project), if any.
    std::optional<core::Profile> profileToAdopt(const engine::EngineSync::ProbedMedia &probed) const;
    // A probed length in frames of `fps` (it was counted at the rate the
    // sequence had when probed).
    core::FrameIndex lengthAtRate(const engine::EngineSync::ProbedMedia &probed, core::Rational fps) const;
    // Queues the import summary's note about format or rate.
    void noteImportedRate(const std::string &path, const engine::EngineSync::ProbedMedia &probed, bool adopted);
    std::vector<std::string> m_importNotes;
    // This import's clips and their rates, summarised at the end
    // (core::frameRateSummary()).
    std::vector<std::pair<std::string, core::Rational>> m_importRates;
    std::expected<void, std::string>
    importProbedAssetOnly(const std::string &path, const engine::EngineSync::ProbedMedia &probed, uint64_t batchKey);
    // Open, New, Reload and Recover replace the project: imports still
    // probing for the old one are cancelled and their results dropped.
    // Saves of the old project still finish writing (the user asked for
    // them), but m_projectGeneration keeps them from touching the new
    // one's path or dirty state.
    void cancelProjectJobs();
    // A close waiting for a write to land (see onCloseRequest()) retries
    // once nothing is left to write.
    void onSaveQueueSettled();
    // Coalesces the timeline and media-browser refresh after imports into
    // one idle callback (queuedRefreshTrampoline).
    void queueRefresh();
    guint m_refreshSourceId = 0; // 0 = none queued
    static constexpr guint kImportRefreshIntervalMs = 400;
    static gboolean queuedRefreshTrampoline(gpointer userData);
    // Always opens the "Save Project" dialog (Save As), regardless of
    // m_currentProjectPath -- bound to Ctrl+Shift+S and the "Save
    // project…" button. See saveInPlaceOrPrompt() for Ctrl+S's "save in
    // place when possible" semantics (low-hanging enhancement #2).
    void onSaveClicked();
    void onSaveFinished(GObject *sourceObject, GAsyncResult *result);
    // Validates, then writes a snapshot to `path` on the pool (doc 19 MT1).
    // When the write lands: marks the state it captured clean, clears any
    // pending recovered-autosave cleanup, and destroys the window if
    // `closeAfterSave` (audit A2's close-and-save flow). Shared by
    // onSaveFinished() (after the Save As dialog) and saveInPlaceOrPrompt()
    // (straight back to m_currentProjectPath) so both behave identically.
    // Returns false if refused before writing.
    bool performSaveToPath(const std::string &path, bool closeAfterSave);
    // Ctrl+S (enhancement #2): saves straight back to m_currentProjectPath
    // with no dialog when the project already has one; falls back to
    // onSaveClicked()'s Save As dialog for an untitled project, since
    // there's no "in place" to save to yet. Also used by onCloseRequest()'s
    // "Save" response, which needs the same in-place-when-possible choice.
    void saveInPlaceOrPrompt(bool closeAfterSave);
    // Confirms first if there are unsaved changes (audit A4, 2026-09-23),
    // same as Reload/New Project below -- navigating the file-picker
    // dialog to choose what to open says nothing about whether it's safe
    // to discard the *current* project, so it's not the "implicit
    // confirmation" this used to be assumed to be.
    void onOpenProjectClicked();
    void onOpenProjectFinished(GObject *sourceObject, GAsyncResult *result);
    // Shared by onOpenProjectFinished() (after the file dialog returns a
    // path) and openRecentProject() (a path picked from the recent-
    // projects menu, no dialog) -- everything loadProject() success/
    // failure needs to do to the model/undo-stack/engine/UI either way.
    void loadProjectFromPath(const std::string &path);
    // doc 19 MT1: Open, Reload and Recover parse on the pool through
    // m_projectLoader; a newer load (or New Project) drops an older one.
    // `adopt` runs on the main thread with the parsed model -- after asking
    // again if the user edited the current project while it parsed.
    // Every failure is logged (path and reason) before `failed` runs.
    void loadProjectAsync(const std::string &path, std::function<void(core::Model)> adopt,
                          std::function<void(const core::ProjectLoadError &)> failed);
    // Why a project didn't open, in words that fit the kind (missing, saved
    // by a newer version, not a U Stu project, ...), with the full reason.
    void showProjectLoadError(const std::string &path, const core::ProjectLoadError &error);
    // The part of every project swap that's the same: cancels the old
    // project's jobs, swaps the model in, clears undo, resets the engine
    // and the selection. The caller sets the path and the clean point.
    void replaceProject(core::Model model);
    // Enhancement #15: GtkRecentManager-backed "recent projects" menu, a
    // GtkMenuButton next to the header bar's Open button.
    // recordRecentProject() is called after every successful save/open
    // (loadProjectFromPath()/performSaveToPath()); refreshRecentProjectsMenu()
    // rebuilds the popover's contents from GtkRecentManager, filtered to
    // this app's own ".ustudio" entries (GtkRecentManager is shared
    // system-wide across every app, so this is the only practical way to
    // avoid listing whatever some other app opened most recently too);
    // openRecentProject() confirms unsaved changes first (same as the
    // Open button, audit A4), then loads the picked path.
    void recordRecentProject(const std::string &path);
    void refreshRecentProjectsMenu();
    void openRecentProject(const std::string &path);
    // Re-loads m_currentProjectPath from disk, discarding in-memory edits --
    // a quick way to pick up a fix or re-attempt opening the same file
    // without going through the file-picker dialog again. A no-op (with a
    // status message) if the project has never been saved/opened, since
    // there's nothing on disk yet to reload from. Confirms first if there
    // are unsaved changes (audit A2) -- a single click next to Save, with
    // nothing else in the way.
    void onReloadProjectClicked();
    void performReload();
    // Resets to a brand new, empty, untitled project -- same effect as
    // Open Project loading a fresh Model::createEmpty(), just without a
    // file dialog. Does not touch whatever's on disk at
    // m_currentProjectPath. Confirms first if there are unsaved changes
    // -- see onReloadProjectClicked's own comment.
    void onNewProjectClicked();
    void performNewProject();
    // Shared by onReloadProjectClicked/onNewProjectClicked (audit A2): a
    // no-op (running `onConfirmed` immediately) if the project has no
    // unsaved changes; otherwise shows a "Discard unsaved changes?"
    // alert (matching offerRecoveryIfAny's own dialog conventions) and
    // runs `onConfirmed` only if the owner picks "Discard".
    void confirmDiscardIfDirty(std::function<void()> onConfirmed);
    // Audit A2: closing the window used to quit immediately with no
    // unsaved-changes prompt and no final autosave. GTK's "close-request"
    // signal lets a handler veto the close by returning GDK_EVENT_STOP;
    // this one does that whenever the project is dirty and shows a
    // Save/Discard/Cancel alert (confirmDiscardIfDirty's Discard/Cancel
    // pair doesn't fit here -- unlike Reload/New Project, closing loses
    // the work for good, so it needs a way to keep it too). The dialog is
    // async, so the confirmed paths destroy the window explicitly:
    // Discard calls gtk_window_destroy() directly (bypassing
    // close-request, so it can't loop back into this same prompt);
    // Save sets m_closeAfterSave and re-enters the normal Save flow,
    // which destroys the window itself once onSaveFinished() sees a
    // successful save with that flag set.
    gboolean onCloseRequest();
    void onRenderClicked();
    void onRenderFinished(GObject *sourceObject, GAsyncResult *result);
    void onAddTrackClicked();
    void onUndo();
    void onRedo();
    void onPlayToggled();
    void onSeekChanged();
    // J/K/L shuttle (doc 05): L accelerates forward (1x -> 2x -> 4x -> 8x),
    // J mirrors it in reverse, K stops. Repeated presses ramp the existing
    // direction's speed rather than resetting to 1x.
    void onShuttleForward();
    void onShuttleReverse();
    void onShuttleStop();
    void onStepForward();
    void onStepBackward();
    // Ctrl+Left/Right (10 frames) and Alt+Left/Right (1 minute) -- both
    // just call stepFrame() with a bigger delta; its seek() already clamps
    // to [0, totalFrames()-1], so "not a full minute left in that
    // direction" (the user's own spec) falls out for free.
    void onStepForward10();
    void onStepBackward10();
    void onStepForwardMinute();
    void onStepBackwardMinute();
    // fps() rounded to the nearest whole frame, times 60; falls back to
    // 25fps (matching the timecode label's own fallback) for the brief
    // window before any project/tractor exists.
    int oneMinuteInFrames() const;
    void onSeekHome();
    void onSeekEnd();
    // Jumps the playhead to the nearest clip boundary (a clip's start or
    // end) on the active track before/after the current frame -- the
    // timeline's own start (0) and end (sequenceFrames()) count
    // as boundaries too, so there's always somewhere to land even on an
    // otherwise-empty or single-clip track. Doesn't pause playback,
    // matching onSeekHome/onSeekEnd's own precedent (a mid-playback jump
    // is a scrub, not a stop).
    // Plain a/f: the nearest cut on ANY track; with Shift, on the active
    // track only (owner request, 2026-09-25).
    void onSeekPreviousCut(bool activeTrackOnly);
    void onSeekNextCut(bool activeTrackOnly);
    // Every clip boundary on every track and every marker, plus 0 and the
    // last frame, sorted and deduplicated.
    std::vector<int> cutBoundariesAllTracks() const;
    // Shift+S/Shift+D with exactly one clip selected (owner, 2026-09-25):
    // move it to the nearest track above (-1) or below (+1) of the same kind
    // where it fits at the same position, skipping tracks where it would
    // land on another clip or that are locked (MoveClip refuses those). A
    // clip in a dissolve is refused. The active track follows the clip.
    void moveSelectedClipAcrossTracks(int direction);
    // Every clip boundary on the active track, plus 0 and totalFrames(),
    // sorted and deduplicated -- shared by the two methods above, and (as
    // cutBoundariesForTrack(m_activeTrack)) the active-track case of the
    // snap targets below.
    std::vector<int> cutBoundariesOnActiveTrack() const;
    // Same as above but for an arbitrary row.
    std::vector<int> cutBoundariesForTrack(int row) const;
    // Moves which track row is "active" (where imports/splits/single-key
    // edits land) up or down by one, clamped to the track list -- the
    // same target onTimelineClicked's plain click already sets, just
    // reachable without the mouse.
    void onActiveTrackUp();
    void onActiveTrackDown();
    // I/O set the loop-in/loop-out at the current playhead (clamped so
    // in < out); there's no ruler widget yet to show the range visually
    // (that's UsTimelineView/ADR-008, M3), so m_loopStatusLabel is the only
    // feedback for now.
    void onSetLoopIn();
    void onSetLoopOut();
    void onClearLoopClicked();
    void onVolumeChanged();
    void onPreviewScaleChanged();
    // "Auto (Full)" / "Auto (Half)": what Auto resolved to, from the engine.
    void updatePreviewScaleLabel();
    bool m_relabellingPreviewScale = false;
    void onSplitClicked();
    // Double-clicks only (renames): a single click arrives as a drag that
    // never moved (onTrackDragEnd), so modifier clicks aren't applied twice.
    void onTimelineClicked(int nPress, double x, double y, timeline::Modifiers mods);
    void onTimelineRightClicked(double x, double y);
    void onDeleteClipClicked();
    // Enhancement #3: Delete key removes the selected clips (the
    // controller's selection), unlike onDeleteClipClicked() above, which
    // acts on whichever clip a right-click's context menu was opened
    // over -- a different, independently-tracked selection.
    void onDeleteSelectedClip();
    // Shift+Delete: removes the selected clips and closes each gap (doc 06).
    void onRippleDeleteSelected();
    // M / Shift+M: a marker at the playhead, or remove the one there.
    void onAddMarker();
    // Tab / Shift+Tab: select the next / previous clip on the active track
    // (from the selection, or the playhead) and move the playhead to it.
    void selectAdjacentClip(bool forward);
    // , . (Shift for 10): move the selected clips by whole frames.
    void nudgeSelection(core::FrameIndex frames);
    bool rippleMode() const;
    void onRemoveMarker();
    void onSplitAudioClicked();
    void onCloseGapClicked();
    void onRemoveTrackClicked();
    enum class TrackFlag
    {
        Lock,
        Hide,
        Mute
    };
    // Track right-click menu: flips one of SetTrackFlags' three flags.
    void onToggleTrackFlag(TrackFlag flag);
    void onEditTrackNameClicked();
    void onTrackVolumeChanged();
    void onWaveformReady();
    void onThumbnailReady();
    void onToggleMediaBrowserClicked();
    // Rebuilds m_mediaBrowserGrid from scratch against m_model.project().bin
    // -- called at every call site that can change the bin: after a
    // successful import, and after Open/Reload/New Project/Recover, which
    // each replace m_model wholesale (same call sites refreshTimeline()
    // itself already runs at, for the same reason).
    void refreshMediaBrowser();
    void setUpMediaList(GtkWidget *scroller);
    void setUpMediaRow(GtkListItem *item);
    void bindMediaRow(GtkListItem *item);
    core::AssetId mediaItemAsset(GtkListItem *item) const;
    void replaceMediaItem(guint position, uint64_t id);
    void refreshMediaThumbnails();
    std::vector<MediaBadge> mediaBadgesOf(const core::Asset &asset) const;
    // Right-click on a media browser row: records which asset it landed
    // on (m_contextMenuAssetId) and pops m_mediaBrowserContextMenu at the
    // click point. `row` is the specific row widget the click landed on
    // (rows are rebuilt from scratch by refreshMediaBrowser(), so it's
    // passed in rather than cached); the popover itself stays parented
    // to the stable m_mediaBrowserPanel (see buildUi()'s own comment on
    // why not m_mediaBrowserList), so (x, y) -- relative to `row` -- is
    // translated into m_mediaBrowserPanel's coordinate space first.
    void onMediaBrowserRowRightClicked(core::AssetId assetId, GtkWidget *row, double x, double y);
    // "Remove from Project": RemoveAsset also removes every clip using
    // the asset (Model::check() invariant 5), so this is a single
    // undoable command, not a two-step confirm -- the file on disk is
    // untouched.
    void onRemoveAssetClicked();
    // "Delete File...": confirms (the file itself is not recoverable via
    // undo, unlike removing it from the project), then does both --
    // RemoveAsset via the undo stack, then an actual filesystem delete.
    // If the delete fails (permissions, already gone) the asset stays
    // removed from the project regardless; a status message says why.
    void onDeleteAssetFileClicked();
    // Drop target for dragging a media browser row onto the timeline
    // (GtkDropTarget on m_timeline, G_TYPE_INT64 carrying the AssetId's
    // value -- see refreshMediaBrowser()'s GtkDragSource on each row).
    // Inserts the asset's full length at the exact row/frame the drop
    // landed on; refuses (with a status message) if that space isn't
    // free or the target track is locked, same as any other insert.
    gboolean onTimelineDrop(const GValue *value, double x, double y);
    // Enhancement #7: a second drop target on m_timeline, accepting
    // GDK_TYPE_FILE_LIST (files dragged in from the file manager, not
    // this app's own asset-row drag above). Each file is imported via
    // importFileToTrack() at the row/frame the drop landed on, placed
    // one after another starting there (same "append after the previous
    // one" rule onFileOpened() uses for multi-select Import).
    gboolean onTimelineFileDrop(GdkFileList *files, double x, double y);
    // Enhancement #7 (media-browser half): a GDK_TYPE_FILE_LIST drop
    // target on m_mediaBrowserPanel -- each file is added to the bin via
    // importAssetOnly(), no clip/track involved.
    gboolean onMediaBrowserFileDrop(GdkFileList *files);
    // Enhancement #8: double-click a media-browser row to insert its
    // asset's full length at the playhead on the active track -- reuses
    // the same overlap/lock refusal importFileToTrack()/onTimelineDrop()
    // already have, via insertAssetAtPosition().
    bool insertAssetAtPosition(core::AssetId assetId, core::TrackId trackId, core::FrameIndex position);
    void onMediaBrowserRowActivated(core::AssetId assetId);
    // Shared by import (a freshly probed asset not yet in the bin --
    // EngineSync::ProbedMedia, not yet a core::MediaInfo) and dragging an
    // existing bin asset onto the timeline (core::MediaInfo::isBoundless
    // ()): a still image or generator has no fixed duration of its own,
    // so its clip spans the rest of the *current* project length from
    // the insert point, falling back to a modest 10s when there's
    // nothing yet to cover -- see onFileOpened's own comment for why.
    core::FrameIndex effectiveInsertLength(bool isBoundless, core::FrameIndex knownLength,
                                           core::FrameIndex insertPos) const;
    // query-tooltip handler (GTK4's mechanism for a per-region tooltip on
    // a custom-drawn widget): true + gtk_tooltip_set_* if (x, y) is over a
    // clip, false to suppress the tooltip anywhere else.
    // The clip tooltip's text: name, in–out, length, source.
    std::string clipTooltipText(core::ClipId clip) const;
    void onEditClipNameClicked();
    void onRemoveClipNameClicked();
    void onRemoveTransitionClicked();
    // "Sync Tracks (Audio)" (owner, 2026-09-25): two clips selected, the
    // right-clicked one is the anchor; the other moves by the offset that
    // lines its sound up with the anchor's, found on the pool
    // (core::audio::align) within +/- kSyncSearchSeconds.
    void onSyncClipsClicked();
    void applySyncResult(core::ClipId anchor, core::ClipId other, core::UndoStack::State stateBefore,
                         uint64_t generation, std::optional<core::audio::Alignment> alignment,
                         const std::string &error);
    static constexpr double kSyncSearchSeconds = 5.0;
    static void syncClipsClickedTrampoline(GtkButton *button, gpointer userData);
    void onAddTransitionClicked();
    // Opens the inline name-edit popover anchored over track `row`'s
    // label strip, or over `clip`, pre-filled with its current name.
    // Enter or clicking away commits (a no-op Command if the text didn't
    // actually change -- see onInlineNameEditClosed); Escape cancels.
    void beginTrackNameEdit(int row);
    void beginClipNameEdit(const ClipDisplay &clip);
    void showInlineNameEditor(GdkRectangle anchor, const std::string &currentName);
    void onInlineNameEditClosed();
    // Returns TRUE (GDK_EVENT_STOP) only for Escape, so the popover's own
    // default key handling (which would otherwise treat Escape as "close",
    // i.e. commit via onInlineNameEditClosed) never runs for it.
    gboolean onInlineNameEditKeyPressed(guint keyval);
    // Returns true if the press hit something draggable (a track handle or
    // a clip) and the gesture should claim the sequence — denying the
    // competing click gesture on the same widget, which would otherwise
    // also treat this as a seek. A press on empty space returns false so
    // the plain click gesture still handles seeking there.
    // The timeline's drag gesture, forwarded to m_timelineController.
    bool onTrackDragBegin(double x, double y, timeline::Modifiers mods);
    void onTrackDragUpdate(double offsetX, double offsetY);
    void onTrackDragEnd(double offsetX, double offsetY);
    // What the controller reads: the model, the viewport, the playhead.
    timeline::TimelineContext timelineContext() const;
    // Runs a controller outcome: the first attempt the undo stack accepts,
    // its status or the failure status, the seek, the active row, a rename.
    void applyTimelineOutcome(timeline::TimelineOutcome &outcome);
    // The three UsTimelineViews' snapshot callbacks (timeline/
    // timeline_renderer.h does the drawing). The playhead is its own
    // overlay widget so playback redraws only the line (audit A5).
    void snapshotTimelineView(GtkSnapshot *snapshot, int width, int height);
    void snapshotPlayheadOverlay(GtkSnapshot *snapshot, int width, int height);
    void snapshotRulerView(GtkSnapshot *snapshot, int width, int height);
    void onFrameReady(std::vector<uint8_t> rgba, int width, int height, int frameNumber);

    void refreshTimeline();
    // Keeps m_viewport in step with the timeline's width and the sequence
    // length, then the scrollbar with m_viewport; redraws what moved.
    void updateViewportGeometry();
    void syncTimelineScrollbar();
    void onTimelineViewportChanged();
    double xForFrame(double frame) const
    {
        return m_viewport.xForFrame(frame);
    }
    int frameAtX(double x) const
    {
        return static_cast<int>(m_viewport.frameForX(x));
    }
    void zoomTimeline(double factor);
    void onZoomIn();
    void onZoomOut();
    void onZoomFit();
    // Ctrl+wheel zooms around the pointer, Shift+wheel and horizontal
    // wheels scroll sideways; a plain wheel is left to the vertical
    // scroller.
    gboolean onTimelineScroll(GtkEventControllerScroll *controller, double dx, double dy);
    void onTimelineHScrollChanged();
    void refreshTransport(int frameNumber);
    void refreshPlayButtonIcon();
    void refreshLoopStatusLabel();
    void updateWindowTitle();
    std::string formatTimecode(int frame) const;
    // True if `path` names the same file as one of the project's own bin
    // assets (audit A6) -- Save and Render both refuse to write there
    // rather than overwrite a user's source media, per CLAUDE.md's data
    // safety rule. Compares canonicalized paths, not raw strings, so a
    // relative or symlinked spelling of the same file is still caught.
    bool pathIsProjectAsset(const std::string &path) const;

    // Row index (position in model->sequence().tracks) -> TrackId. Asserts
    // in debug builds if out of range; callers only pass rows the timeline
    // itself just drew, so this should never be reached with a stale one.
    core::TrackId trackIdForRow(int row) const;

    // Creates a stateless GSimpleAction named `name`, wires `activated` as
    // its "activate" handler with `this` as user data, adds it to the
    // window's action map, and binds `accels` to "win.<name>" -- the
    // boilerplate installActions()'s loop over action_registry.h's table
    // shares for every entry except undo/redo (see setAccelsForAction).
    void addAction(GtkApplication *app, const char *name, void (*activated)(GSimpleAction *, GVariant *, gpointer),
                   const std::vector<const char *> &accels);
    // Just the "win.<name>" -> accels half of addAction(), factored out so
    // installActions() can set undo/redo's default accelerators from the
    // same action_registry.h table as every other action, without also
    // creating a second GSimpleAction for them (they're constructed
    // separately -- see installActions()'s own comment).
    void setAccelsForAction(GtkApplication *app, const char *name, const std::vector<const char *> &accels);

    // Multi-tab Help dialog (Keyboard Shortcuts, built from
    // action_registry.h's table grouped by category; About, static text
    // matching data/com.ustudio.VideoEditor.metainfo.xml) and Settings
    // dialog (General + Playback tabs bound to Settings/GSettings, plus a
    // placeholder Keyboard Shortcuts tab for the future rebinding
    // feature). Both build a fresh AdwDialog per call, same pattern as
    // every other AdwAlertDialog in this file -- no persistent member.
    void showHelpDialog();
    void showSettingsDialog();
    // Help's Controls tab: every ui_hints.h entry, grouped by category, so
    // a drop-in's registered hints appear there too.
    // Help (app/help_dialog.cpp). M4 G: collapsible sections that come
    // back as they were left (the session's here, and Settings').
    GtkWidget *buildControlsPage();
    GtkWidget *buildShortcutsPage();
    GtkWidget *buildReleaseNotesPage();
    GtkWidget *buildAboutPage();
    // Help's diagnostics (0.49.0-beta.2): the log folder in the file
    // manager, and a report for the clipboard (app/help_dialog.cpp).
    void openLogFolder();
    void copyDiagnostics();
    void loadHelpState();
    AdwExpanderRow *helpSection(AdwPreferencesGroup *group, const char *tab, const std::string &title,
                                const std::string &subtitle);
    GtkAdjustment *restoreHelpScroll(GtkWidget *page, const char *tab);
    bool m_helpStateLoaded = false;
    std::set<std::string> m_helpOpenSections; // "<tab>:<section>"
    std::string m_helpTab;
    std::map<std::string, double> m_helpScroll;

    // Audit A1: the transport actions installActions() binds to bare
    // letters and Left/Right/Home/End (+ Ctrl/Alt variants) are global
    // window accelerators, so they fire even while the inline track/clip
    // name GtkEntry has focus -- typing "a" there seeks to the previous
    // cut instead of inserting the letter. showInlineNameEditor() disables
    // them for the popover's lifetime; onInlineNameEditClosed() re-enables
    // them. Undo/redo (Ctrl+Z/Ctrl+Shift+Z) are deliberately not in this
    // set: they're modifier combos, not bare keys a text entry would ever
    // want to consume itself.
    void setTransportActionsEnabled(bool enabled);
    // The same guard for every text field in the window (dialog entries,
    // spin buttons, drop-in inspector pages): follows the focus widget.
    void onFocusWidgetChanged();
    void applyTransportActionsEnabled();
    // A press on the timeline or the preview takes the keys back from a
    // text field: neither is focusable, so without this an inspector
    // entry would keep them off until the user tabbed out of it.
    void addTextFocusRelease(GtkWidget *widget);

    static void importClickedTrampoline(GtkButton *button, gpointer userData);
    static void fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void saveClickedTrampoline(GtkButton *button, gpointer userData);
    static void saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void openProjectClickedTrampoline(GtkButton *button, gpointer userData);
    // Reads the path stashed via g_object_set_data_full() on `button`
    // itself (refreshRecentProjectsMenu() builds one such button per
    // recent entry) and opens it.
    static void recentProjectClickedTrampoline(GtkButton *button, gpointer userData);
    // GtkRecentManager's own "changed" signal -- the correct source of
    // truth for refreshRecentProjectsMenu() (see recordRecentProject()'s
    // own comment on why calling it directly right after add_item()
    // isn't).
    static void recentManagerChangedTrampoline(GtkRecentManager *manager, gpointer userData);
    static void openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void reloadProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void newProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleMediaBrowserClickedTrampoline(GtkButton *button, gpointer userData);
    static void mediaBrowserRowRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                                    gpointer userData);
    static void removeAssetClickedTrampoline(GtkButton *button, gpointer userData);
    static void deleteAssetFileClickedTrampoline(GtkButton *button, gpointer userData);
    static gboolean timelineDropTrampoline(GtkDropTarget *target, const GValue *value, double x, double y,
                                           gpointer userData);
    static gboolean timelineFileDropTrampoline(GtkDropTarget *target, const GValue *value, double x, double y,
                                               gpointer userData);
    static gboolean mediaBrowserFileDropTrampoline(GtkDropTarget *target, const GValue *value, double x, double y,
                                                   gpointer userData);
    static void mediaBrowserRowActivatedTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                                   gpointer userData);
    static void renderClickedTrampoline(GtkButton *button, gpointer userData);
    static void renderFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void renderButtonRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                                 gpointer userData);
    static void renderMenuItemClickedTrampoline(GtkButton *button, gpointer userData);
    static void helpClickedTrampoline(GtkButton *button, gpointer userData);
    static void settingsClickedTrampoline(GtkButton *button, gpointer userData);
    // Settings dialog rows: each reads its new value straight off the row
    // passed in (the "notify::..." signal's own object), not a stored
    // AppWindow member -- these rows only live for the dialog's lifetime,
    // unlike the transport bar's m_previewScaleDropdown.
    static void settingsAutosaveDelayChangedTrampoline(GtkSpinButton *row, GParamSpec *pspec, gpointer userData);
    static void settingsRecentProjectsMaxChangedTrampoline(GtkSpinButton *row, GParamSpec *pspec, gpointer userData);
    static void settingsShuttleMaxSpeedChangedTrampoline(GtkSpinButton *row, GParamSpec *pspec, gpointer userData);
    static void settingsWorkerThreadsChangedTrampoline(GtkSpinButton *row, GParamSpec *pspec, gpointer userData);
    static gboolean settingsWorkerThreadsOutputTrampoline(GtkSpinButton *row, gpointer userData);
    static gint settingsWorkerThreadsInputTrampoline(GtkSpinButton *row, double *newValue, gpointer userData);
    static void settingsPreviewScaleChangedTrampoline(AdwComboRow *row, GParamSpec *pspec, gpointer userData);
    static void settingsCacheJobsChangedTrampoline(GtkSpinButton *row, GParamSpec *pspec, gpointer userData);
    static gboolean settingsCacheJobsOutputTrampoline(GtkSpinButton *row, gpointer userData);
    static void settingsToggleChangedTrampoline(AdwSwitchRow *row, GParamSpec *pspec, gpointer userData);
    static void settingsChooseFolderClickedTrampoline(GtkButton *button, gpointer userData);
    static void settingsClearFolderClickedTrampoline(GtkButton *button, gpointer userData);
    static void addTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void undoClickedTrampoline(GtkButton *button, gpointer userData);
    static void redoClickedTrampoline(GtkButton *button, gpointer userData);
    static void playToggledTrampoline(GtkButton *button, gpointer userData);
    static void seekChangedTrampoline(GtkRange *range, gpointer userData);
    static void splitClickedTrampoline(GtkButton *button, gpointer userData);
    static gboolean timelineScrollTrampoline(GtkEventControllerScroll *controller, double dx, double dy,
                                             gpointer userData);
    static void timelineHScrollChangedTrampoline(GtkAdjustment *adjustment, gpointer userData);
    static void unparentPopoverTrampoline(GtkWidget *parent, gpointer popover);
    // Every registered action runs through this (addAction), inside a
    // core::trace::Scope named after it, for the stall monitor.
    struct TracedAction
    {
        AppWindow *self;
        void (*activated)(GSimpleAction *, GVariant *, gpointer);
        const char *name; // from action_registry's static table
    };
    static void tracedActionTrampoline(GSimpleAction *action, GVariant *parameter, gpointer data);
    static void timelineMotionTrampoline(GtkEventControllerMotion *controller, double x, double y, gpointer userData);
    static void timelineLeaveTrampoline(GtkEventControllerMotion *controller, gpointer userData);
    static gboolean hoverPreviewTimerTrampoline(gpointer userData);

    // The clip tooltip: resting the pointer on a clip for
    // kHoverPreviewDelayMs shows its name, in–out, length and source, and
    // on a video clip (Settings > Toggles > thumbnails in clip tooltips) the
    // source frame under the pointer, about 240 px wide, with the timeline
    // timecode on top. A popover styled as a tooltip, not a GtkTooltip:
    // GTK 4 fixes the tooltip delay at 500 ms. The frames have their own
    // small cache: the timeline's strips are smaller, and drop frames a
    // redraw didn't ask for again.
    std::unique_ptr<engine::ThumbnailCache> m_hoverThumbnails;
    timeline::TextureCache m_hoverTextures{16};
    GtkPopover *m_hoverPreview = nullptr;
    GtkPicture *m_hoverPicture = nullptr;
    GtkLabel *m_hoverTimecode = nullptr;
    GtkLabel *m_hoverText = nullptr;
    guint m_hoverTimerId = 0;
    gint64 m_hoverShownAt = 0; // monotonic µs
    struct HoverTarget
    {
        std::string resource; // "" for no thumbnail (audio, or the toggle's off)
        int sourceFrame = 0;
        core::FrameIndex timelineFrame = 0;
        std::string text;
        double x = 0.0, y = 0.0;
        bool waiting = false; // shown; the thumbnail is still to come
    } m_hover;
    void onTimelineHover(double x, double y);
    void showHoverPreview();
    void showHoverPreviewIfReady();
    void hideHoverPreview();
    static void rippleDeleteSelectedActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void addMarkerActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void removeMarkerActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void selectNextClipActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void selectPreviousClipActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void nudgeLeftActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void nudgeRightActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void nudgeLeft10Activated(GSimpleAction *, GVariant *, gpointer userData);
    static void nudgeRight10Activated(GSimpleAction *, GVariant *, gpointer userData);
    static void rippleModeChangedTrampoline(GObject *action, GParamSpec *, gpointer userData);
    static void timelinePinchTrampoline(GtkGestureZoom *gesture, double scale, gpointer userData);
    static void timelinePinchBeginTrampoline(GtkGesture *gesture, GdkEventSequence *, gpointer userData);
    static void selectAllActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void clearSelectionActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void zoomInActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void zoomOutActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void zoomFitActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void toggleInspectorActivated(GSimpleAction *, GVariant *, gpointer userData);
    static void onInspectorCollapsedTrampoline(GObject *, GParamSpec *, gpointer userData);
    static void onInspectorShownTrampoline(GObject *, GParamSpec *, gpointer userData);
    static void timelineClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y, gpointer userData);
    static void timelineRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                             gpointer userData);
    static void deleteClipClickedTrampoline(GtkButton *button, gpointer userData);
    static void splitAudioClickedTrampoline(GtkButton *button, gpointer userData);
    static void closeGapClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleLockClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleHideClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleMuteClickedTrampoline(GtkButton *button, gpointer userData);
    static void editTrackNameClickedTrampoline(GtkButton *button, gpointer userData);
    static void trackVolumeChangedTrampoline(GtkRange *range, gpointer userData);
    static void trackDragBeginTrampoline(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void trackDragUpdateTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void trackDragEndTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void trackDragCancelTrampoline(GtkGesture *gesture, GdkEventSequence *sequence, gpointer userData);
    static void undoActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void redoActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void playPauseActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    // "open-log-folder" and "copy-diagnostics": dispatched by name.
    static void diagnosticsActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    // Every "transform-*" action (M4 F2): dispatched by name.
    static void transformActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void saveActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void saveAsActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void projectFrameRateActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void projectFrameRateClickedTrampoline(GtkButton *button, gpointer userData);
    // Asks for the project's new frame rate and applies it as one undoable
    // ChangeSequenceFrameRate (FR2).
    void onProjectFrameRateClicked();
    static void saveButtonRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                               gpointer userData);
    static void openProjectActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    // Ctrl+Q: closes the window the normal way (onCloseRequest(): the
    // unsaved-changes prompt, waiting for a save), which ends the app.
    static void quitActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void newProjectActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void importActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void importFolderActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void importImageSequenceActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void deleteSelectedClipActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void splitAtPlayheadActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void shuttleForwardActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void shuttleReverseActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void shuttleStopActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepForwardActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepBackwardActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepForward10Activated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepBackward10Activated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepForwardMinuteActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepBackwardMinuteActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekHomeActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekEndActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void loopSetInActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void loopSetOutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekPreviousCutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekNextCutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void activeTrackUpActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void activeTrackDownActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekPreviousCutOnActiveTrackActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekNextCutOnActiveTrackActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void activeTrackTopActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void activeTrackBottomActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void clearLoopClickedTrampoline(GtkButton *button, gpointer userData);
    static void volumeChangedTrampoline(GtkRange *range, gpointer userData);
    static void previewScaleChangedTrampoline(GtkDropDown *dropdown, GParamSpec *pspec, gpointer userData);
    static void editClipNameClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeClipNameClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeTransitionClickedTrampoline(GtkButton *button, gpointer userData);
    static void addTransitionClickedTrampoline(GtkButton *button, gpointer userData);
    static void inlineNameEditActivateTrampoline(GtkEntry *entry, gpointer userData);
    static void inlineNameEditClosedTrampoline(GtkPopover *popover, gpointer userData);
    static gboolean inlineNameEditKeyTrampoline(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                                GdkModifierType state, gpointer userData);

    AdwApplicationWindow *m_window = nullptr;
    GtkPicture *m_preview = nullptr;
    // Media browser: a collapsible panel to the left of the preview
    // (same row) listing every imported asset as a row (thumbnail, name,
    // length, fps, format, badges). m_mediaBrowserPanel is the whole
    // collapsible widget (a GtkScrolledWindow); m_mediaBrowserList is a
    // GtkListView resynced by refreshMediaBrowser() each time the bin
    // changes (mirrors refreshTimeline()'s own call-site-driven resync --
    // see its own comment for why this app doesn't subscribe to
    // Model::changed directly), rebinding only rows that changed. Each row
    // is one widget a right-click gesture and a drag source attach to, and
    // lays out its cells at fixed widths so columns align across rows.
    GtkWidget *m_mediaBrowserPanel = nullptr;
    GtkWidget *m_mediaBrowserList = nullptr;                   // a GtkListView (M4 D)
    GtkStringList *m_mediaIds = nullptr;                       // its items: asset ids, in bin order
    std::vector<std::pair<uint64_t, std::string>> m_mediaRows; // each row's id and what it shows
    std::set<uint64_t> m_mediaAwaitingThumbnail;
    std::unordered_map<uint64_t, GtkWidget *> m_mediaBoundThumbs; // bound rows' pictures, by asset
    bool m_mediaWaitingForHeight = false;
    timeline::TextureCache m_mediaTextures{256};
    // Two-button popover ("Remove from Project" / "Delete File...") for
    // whichever row was last right-clicked (m_contextMenuAssetId) --
    // reparented onto that row each time (see
    // onMediaBrowserRowRightClicked's own comment).
    GtkPopover *m_mediaBrowserContextMenu = nullptr;
    GtkWidget *m_removeAssetButton = nullptr;
    GtkWidget *m_deleteAssetFileButton = nullptr;
    core::AssetId m_contextMenuAssetId;
    GtkWidget *m_timeline = nullptr; // a UsTimelineView (ADR-008)
    // Audit A5: see onPlayheadOverlayDraw()'s own comment. Stacked on top
    // of m_timeline inside a GtkOverlay (buildUi()); not a target for
    // pointer events, so every click/drag/drop/tooltip controller stays
    // exactly where it already was, on m_timeline itself.
    GtkWidget *m_playheadOverlay = nullptr;
    // Enhancement #12: a separate, fixed-height widget stacked ABOVE
    // m_timeline in the layout (buildUi()), not overlapping it -- avoids
    // touching any of the row/y-coordinate math onTimelineClicked()/
    // onTrackDragBegin()/onTrackDragUpdate()/onTimelineRightClicked()
    // already do, since the ruler occupies its own vertical space rather
    // than the top of the track grid.
    GtkWidget *m_rulerArea = nullptr;
    // Doc 15 IP5's hosts (shell_hosts.cpp). All empty, null or false with
    // no drop-in, and then nothing below changes the window.
    std::vector<timeline::TimelineOverlayProvider *> m_timelineOverlays;
    std::vector<ImportHandler> m_importHandlers;
    core::Signal<> m_shellProjectChanged;
    core::Signal<> m_shellSelectionChanged;
    core::Signal<> m_shellPlayheadMoved;
    ShellSelection m_lastShellSelection;
    guint m_shellSelectionIdleId = 0;
    bool m_hasShellExtensions = false;
    AdwToolbarView *m_toolbarView = nullptr;
    GtkWidget *m_mainPaned = nullptr; // the toolbar view's content until an inspector wraps it
    GtkWidget *m_previewFrame = nullptr;
    AdwOverlaySplitView *m_inspectorSplit = nullptr;
    AdwViewStack *m_inspectorStack = nullptr;
    GtkOverlay *m_previewOverlay = nullptr;
    std::vector<GtkWidget *> m_previewOverlays;
    // The layout the timeline draws and hit-tests with: uniform rows plus
    // any drop-in lanes.
    timeline::RowLayout rowLayout() const;
    // A drop-in's timeline overlay claims a press before the timeline does.
    // The provider that claimed a press, or null; `m_overlayDrag` while it
    // drags (from onTrackDragBegin() to its end or cancel), with the press.
    timeline::TimelineOverlayProvider *overlayClaimsPress(double x, double y, int nPress);
    timeline::TimelineOverlayProvider *m_overlayDrag = nullptr;
    // Ends a provider's drag unfinished (Escape, a cancelled gesture, an
    // edit landing mid-drag).
    void cancelOverlayDrag();
    double m_overlayDragX = 0.0, m_overlayDragY = 0.0;
    // True while m_overlayDrag->dragged() runs: its own edits don't cancel it.
    bool m_overlayDragEditing = false;
    // Called after each timeline snapshot: a changed selection is posted to
    // selectionChanged from an idle (never from inside the draw).
    void noteSelectionForShell();
    static gboolean shellSelectionIdleTrampoline(gpointer userData);
    // Hands each path a handler claims to it, in order; returns the rest,
    // with `position` moved past what the handlers placed.
    std::vector<std::string> importWithHandlers(std::vector<std::string> paths, std::optional<core::TrackId> trackId,
                                                std::optional<core::FrameIndex> &position);
    // doc 06's Viewport: zoom and horizontal scroll for the timeline, the
    // ruler and the playhead overlay alike. m_timelineHAdjustment mirrors
    // it for the scrollbar under the timeline (in pixels).
    timeline::Viewport m_viewport;
    GtkAdjustment *m_timelineHAdjustment = nullptr;
    bool m_suppressHScrollSignal = false;
    double m_timelinePointerX = 0.0;
    double m_pinchLastScale = 1.0;
    GtkScale *m_seekScale = nullptr;
    GtkButton *m_playButton = nullptr;
    // Enhancement #4: the header bar's own title widget (what's actually
    // visible -- libadwaita's client-side header replaces the OS
    // titlebar text) -- updateWindowTitle() sets its title to the
    // project name (+ dirty mark) alongside gtk_window_set_title(),
    // which still drives the taskbar/Alt-Tab label.
    AdwWindowTitle *m_windowTitle = nullptr;
    GtkPopover *m_recentProjectsPopover = nullptr;
    GtkButton *m_undoButton = nullptr;
    GtkButton *m_redoButton = nullptr;
    GtkLabel *m_timecodeLabel = nullptr;
    GtkLabel *m_statusLabel = nullptr;
    GtkLabel *m_loopStatusLabel = nullptr;
    GtkScale *m_volumeScale = nullptr;
    GtkDropDown *m_previewScaleDropdown = nullptr;
    GtkPopover *m_trackContextMenu = nullptr;
    GtkWidget *m_deleteClipButton = nullptr;
    GtkWidget *m_splitAudioButton = nullptr;
    GtkWidget *m_closeGapButton = nullptr;
    GtkWidget *m_removeTrackButton = nullptr;
    GtkWidget *m_toggleLockButton = nullptr;
    GtkWidget *m_toggleHideButton = nullptr;
    GtkWidget *m_toggleMuteButton = nullptr;
    GtkWidget *m_editTrackNameButton = nullptr;
    GtkWidget *m_editClipNameButton = nullptr;
    GtkWidget *m_removeClipNameButton = nullptr;
    GtkWidget *m_removeTransitionButton = nullptr;
    GtkWidget *m_addTransitionButton = nullptr;
    GtkWidget *m_syncClipsButton = nullptr;
    core::ClipId m_contextMenuClip; // the clip right-clicked, if any
    GtkScale *m_trackVolumeScale = nullptr;
    // "value-changed" fires while merely repositioning the slider to the
    // right-clicked track's current volume (see onTimelineRightClicked) --
    // this suppresses turning that into a spurious SetTrackVolume command,
    // matching m_suppressSeekSignal's existing role for the seek scale.
    bool m_suppressTrackVolumeSignal = false;

    // Constructed first (before buildUi()): buildUi() reads
    // defaultPreviewScale() for the transport dropdown's initial
    // selection, and onShuttleForward/onAutosaveHeartbeat/
    // refreshRecentProjectsMenu all read from it too.
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<RenderProfileStore> m_renderProfiles;
    // The default profile, or High quality if that one's gone.
    core::RenderProfile defaultRenderProfile() const;
    // Settings > Toggles, cached here so they still work for the session
    // when the schema is missing (Settings' setters are then no-ops).
    bool m_snapWhileDragging = true;
    bool m_followPlayhead = true;
    bool m_showTimelineThumbnails = true;
    bool m_showWaveforms = true;
    bool m_showHoverPreview = true;
    void onSettingsToggleChanged(const std::string &key, bool active);
    // Settings > Locations: `key` is "project" or "export".
    void chooseDefaultFolder(const std::string &key, AdwActionRow *row);
    void setDefaultFolder(const std::string &key, const std::string &folder, AdwActionRow *row);
    // Where the Open and Save As dialogs start: the default project folder,
    // if set and present.
    void setInitialProjectFolder(GtkFileDialog *dialog) const;
    // ". It has effects from “x”, not installed: ..." when the project has
    // effects no loaded drop-in plays (IP2), else "".
    std::string unplayedEffectsNotice() const;
    // Where renders go: the default export folder, else the project's own
    // folder, else ~/Videos, else home.
    std::string exportFolder() const;
    // The file named by last-project-path, when reopening is on; called at
    // startup only when there's no recovery to offer.
    void reopenLastProjectIfWanted(const std::string &path);

    core::Model m_model = core::Model::createEmpty();
    core::UndoStack m_undoStack{m_model};
    std::unique_ptr<engine::Engine> m_engine;
    // The playback graph's length and rate, from the model rather than the
    // engine's mirror (ADR-003): right after an edit the engine thread may
    // not have rebuilt yet, but the tractor is always max(sequence length,
    // 1) long (EngineSync::verify()) at the sequence profile's rate.
    int sequenceFrames() const;
    double sequenceFps() const;
    std::unique_ptr<engine::WaveformCache> m_waveforms;
    std::unique_ptr<engine::ThumbnailCache> m_thumbnails;
    // The timeline's thumbnail strips (their own worker; see the constructor)
    // and the textures made from them.
    std::unique_ptr<engine::ThumbnailCache> m_timelineThumbnails;
    // doc 19 MT1: the window's worker pool and what runs on it. Results come
    // back through MainThreadDispatcher::post guarded by m_lifetime, so a
    // late one after the window is gone is dropped. prepareForShutdown()
    // cancels and joins them before MLT is closed.
    engine::MainThreadDispatcher::LifetimeToken m_lifetime = engine::MainThreadDispatcher::makeToken();
    std::unique_ptr<core::concurrency::ThreadPool> m_pool;
    std::unique_ptr<ImportQueue> m_importQueue;
    std::unique_ptr<SaveQueue> m_saveQueue;
    std::unique_ptr<ProjectLoader> m_projectLoader;
    // Bumped whenever the project is replaced (cancelProjectJobs()).
    uint64_t m_projectGeneration = 0;
    bool m_closeWhenSaved = false;
    timeline::TextureCache m_thumbnailTextures{600};
    timeline::TextureCache m_waveformTextures{64}; // timeline_renderer.h: one per visible waveform and zoom
    std::vector<ClipDisplay> m_clips;
    // doc 06's TimelineController: gesture state, drag preview and the
    // clip selection.
    timeline::TimelineController m_timelineController;
    // Which track row new imports/splits target; set by clicking a track's
    // row. A row index into model.sequence().tracks, not a TrackId.
    int m_activeTrack = 0;
    // What a right-click's context menu is currently open for: the track
    // row, the frame position clicked, and — if the click landed on a clip
    // or a gap — that clip's/gap's start frame (-1 if neither applies).
    // Which popover buttons are visible is decided from these at
    // right-click time (see onTimelineRightClicked).
    int m_contextMenuTrack = -1;
    int m_contextMenuFrame = -1;
    int m_contextMenuClipStartFrame = -1;
    int m_contextMenuGapStartFrame = -1;
    // Set alongside the above when the right-click landed inside a
    // dissolve transition's overlap region (invalid/default otherwise).
    core::TransitionId m_contextMenuTransitionId;
    // Set alongside the above when the right-click landed near the exact
    // boundary between two touching, not-yet-linked clips (invalid/
    // default otherwise) -- offers "Add Transition" there.
    core::ClipId m_contextMenuAddTransitionA, m_contextMenuAddTransitionB;

    // --- Inline track/clip name editing (double-click, or the context
    // menu's Edit Name) ---
    GtkPopover *m_inlineNameEditPopover = nullptr;
    GtkEntry *m_inlineNameEditEntry = nullptr;
    InlineEditKind m_inlineEditKind = InlineEditKind::None;
    int m_inlineEditTrackRow = -1;   // valid when m_inlineEditKind == Track
    core::ClipId m_inlineEditClipId; // valid when m_inlineEditKind == Clip
    // Set by Escape (onInlineNameEditKeyPressed) just before popping the
    // popover down, so the "closed" signal that follows knows to discard
    // the entry's text instead of committing it.
    bool m_inlineEditCancelled = false;
    // Why the single-key actions are off: the inline editor is open, or a
    // text field has focus. Either one keeps them off.
    bool m_inlineEditOpen = false;
    bool m_textHasFocus = false;
    bool m_transportActionsEnabled = true;
    // Drop-ins' actions with a shortcut a text field would type (addActions()):
    // switched with the editor's own single keys.
    std::vector<std::string> m_typingKeyActions;
    // Bumped by showStatus(): whether a drop-in's import handler said
    // something itself (then the shell's "Imported <path>" doesn't cover it).
    uint64_t m_statusCount = 0;
    // The header's Settings/Help group: drop-ins' buttons go before Settings.
    GtkWidget *m_appHeaderGroup = nullptr;
    GtkWidget *m_settingsButton = nullptr;

    bool m_suppressSeekSignal = false;

    // --- Autosave / recovery (doc 09) ---
    // Empty until the first successful Save/Save As/Open; drives both the
    // autosave filename (autosave::baseNameFor) and where a normal Save
    // writes.
    std::string m_currentProjectPath;
    // Fresh per launch (a UUID) -- makes two untitled sessions autosave to
    // different files instead of overwriting each other, per doc 09.
    std::string m_autosaveSessionId;
    // The autosaves this session wrote, taken back when a save or a clean
    // quit makes them stale (so only unsaved work is ever offered).
    std::unique_ptr<autosave::OwnAutosaves> m_ownAutosaves;
    void removeStaleAutosaves(const std::vector<std::string> &bases);
    gint64 m_lastEditMonotonicUsec = 0;
    // When the oldest edit not yet in an autosave happened; 0 = none
    // pending. Set by the first rebuild after an autosave, cleared by
    // performAutosave() and whenever the project is clean.
    gint64 m_unsavedSinceMonotonicUsec = 0;
    static constexpr guint kAutosaveHeartbeatSeconds = 10;
    guint m_autosaveHeartbeatId = 0;
    // Set after a successful "Recover" (audit A2): this session's own
    // future autosaves go to a filename keyed on m_autosaveSessionId (a
    // fresh UUID per launch), never the recovered file's, so nothing else
    // would ever clean the old one up. Deleted only once a manual Save
    // actually lands the recovered content somewhere durable -- not right
    // after recovery, when it would be the only copy of that work again if
    // the app crashed a second time before the user got to Save. Empty =
    // nothing pending. Cleared WITHOUT deleting anything by New Project/
    // Open/Reload (audit A1): switching away to a different project
    // before that Save happens must forget this pending cleanup, or a
    // later Save of the NEW project would delete the recovered
    // content's still-only copy out from under it. The files themselves
    // stay on disk either way -- a later launch, or offerRecoveryIfAny()
    // looping later in this one, can still find and offer them.
    std::string m_pendingAutosaveCleanupPath;
    std::string m_pendingAutosaveCleanupMetaPath;
    // Every .meta path offerRecoveryIfAny() has already asked about this
    // launch (recovering doesn't remove the file -- see the comment
    // above -- so without this the same one would just be found again).
    // Lets it loop through EVERY independently-orphaned autosave instead
    // of only ever surfacing whichever one findRecoverable() happens to
    // return first, so an older one with real content never goes silently
    // unmentioned just because a newer, emptier one also exists.
    std::set<std::string> m_offeredAutosaveMetaPaths;
    // Set by onCloseRequest()'s "Save" response, consumed by the very next
    // onSaveFinished() (success or not -- see that method's own comment)
    // so a save the user triggers manually in between never accidentally
    // closes the window.
    bool m_closeAfterSave = false;

    // Renders, one at a time (the Render button). prepareForShutdown()
    // stops it before MLT closes (post-M3 audit P2) and keeps what didn't
    // finish for the next launch (pending_renders).
    std::unique_ptr<RenderQueue> m_renderQueue;
    // Settings > Render > Render threads; read by the render backend on its
    // thread as each render starts, so a running render keeps its own.
    std::atomic<int> m_renderThreadsPercent{Settings::kDefaultRenderThreadsPercent};
    bool m_stopRenderConfirmed = false; // "Quit" was chosen while rendering
    // The Render button: a progress fill under its label, a queued-count
    // badge, and after a render, "Open Render" until clicked.
    GtkWidget *m_renderButton = nullptr;
    GtkLabel *m_renderLabel = nullptr;
    GtkProgressBar *m_renderProgress = nullptr;
    GtkLabel *m_renderBadge = nullptr;
    GtkPopover *m_renderMenu = nullptr;
    std::string m_lastRenderedPath; // non-empty: the button offers to open it
    // Every render goes through here. With missing media in use it asks
    // first ("Relink first / Render anyway") unless `missingConfirmed`.
    void queueRender(const core::RenderProfile &profile, const std::string &path, bool missingConfirmed = false);
    // `<project>-<profile>-YYYYMMDD-HHMMSS.mp4` in exportFolder(), unused.
    std::string autoRenderPath(const core::RenderProfile &profile) const;
    void askCancelOrQueueRender();
    void openLastRender();
    void updateRenderButton();
    void showRenderMenu();
    void onRenderStarted(const RenderJob &job);
    void onRenderProgress(double fraction);
    void onRenderDone(const RenderJob &job, bool ok, bool cancelled, const std::string &error);
    // Renders the last session didn't finish: offered once, at startup.
    void offerPendingRenders();
    void restartPendingRenders();

    void onAutosaveHeartbeat();
    void performAutosave();
    bool offerRecoveryIfAny(); // true when it asked
    void onWindowActiveChanged();
    static gboolean autosaveHeartbeatTrampoline(gpointer userData);
    static void windowActiveChangedTrampoline(GObject *object, GParamSpec *pspec, gpointer userData);
    static void focusWidgetChangedTrampoline(GObject *object, GParamSpec *pspec, gpointer userData);
    static void textFocusReleaseTrampoline(GtkGestureClick *gesture, int nPress, double x, double y, gpointer userData);
    static gboolean closeRequestTrampoline(GtkWindow *window, gpointer userData);
};

} // namespace ustudio::app
