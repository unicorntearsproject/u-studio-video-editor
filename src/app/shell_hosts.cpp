// Doc 15 IP5: AppWindow as the ShellHost drop-ins build their UI on
// (shell_host.h). Split out of app_window.cpp to keep the hosts together;
// every host starts empty, and until a drop-in adds to it the window is
// built and behaves exactly as without this file.

#include "app_window.h"

#include "core/log.h"
#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

bool AppWindow::execute(std::unique_ptr<core::Command> command)
{
    if (!m_undoStack.execute(std::move(command)))
        return false;
    refreshTimeline();
    return true;
}

core::FrameIndex AppWindow::currentFrame() const
{
    return m_engine->currentFrame();
}

void AppWindow::seek(core::FrameIndex frame)
{
    const core::FrameIndex last = std::max<core::FrameIndex>(m_model.sequence().length() - 1, 0);
    m_engine->seek(static_cast<int>(std::clamp<core::FrameIndex>(frame, 0, last)));
}

ShellSelection AppWindow::currentSelection() const
{
    ShellSelection selection;
    for (core::ClipId clip : m_timelineController.selection().clips())
        if (m_model.hasClip(clip))
            selection.clips.push_back(clip);
    std::sort(selection.clips.begin(), selection.clips.end(), [this](core::ClipId a, core::ClipId b) {
        const core::Clip &x = m_model.clip(a), &y = m_model.clip(b);
        return x.position != y.position ? x.position < y.position : a.value < b.value;
    });
    if (m_activeTrack >= 0 && static_cast<size_t>(m_activeTrack) < m_model.sequence().tracks.size())
        selection.track = trackIdForRow(m_activeTrack);
    return selection;
}

void AppWindow::noteSelectionForShell()
{
    if (!m_hasShellExtensions || m_shellSelectionIdleId != 0 || currentSelection() == m_lastShellSelection)
        return;
    m_shellSelectionIdleId = g_idle_add(&AppWindow::shellSelectionIdleTrampoline, this);
}

void AppWindow::addInspectorPage(const InspectorPage &page)
{
    if (!m_inspectorSplit) {
        // The first page wraps the window's content in a split view with the
        // inspector on the right. Its toggle floats in the content's top
        // right corner rather than joining the header bar, which is already
        // as wide as the default 1100 px window allows (one more button
        // there widened the whole window, 2026-09-25).
        registerHints({{"inspector.toggle", "Inspector", "Show or hide the inspector",
                        "Pages that drop-ins add: effects, titles, …", "toggle-inspector", nullptr},
                       {"inspector.hide", "Inspector", "Hide the inspector",
                        "Gives its space back to the picture; the button in the picture's corner brings it back",
                        "toggle-inspector", nullptr}});
        m_inspectorSplit = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
        adw_overlay_split_view_set_sidebar_position(m_inspectorSplit, GTK_PACK_END);
        // Floating over the content, hidden until toggled: side by side it
        // needs the content's minimum (about 885 px) plus its own, wider
        // than the default window, and docking it only when there's room
        // takes an AdwBreakpoint, which drops the window's content-based
        // minimum size for everyone -- a decision of its own (doc 15's
        // REVIEW note), not part of adding the host.
        adw_overlay_split_view_set_collapsed(m_inspectorSplit, TRUE);
        adw_overlay_split_view_set_show_sidebar(m_inspectorSplit, FALSE);
        adw_overlay_split_view_set_min_sidebar_width(m_inspectorSplit, 240);
        adw_overlay_split_view_set_max_sidebar_width(m_inspectorSplit, 360);

        // Docked beside the content when the window has room (FX2, the
        // effects Rack: floating, it covers the picture and its shield takes
        // every drop meant for the preview). A window with breakpoints no
        // longer takes its minimum size from its content, so the minimum it
        // has now is pinned first (doc 15's REVIEW note on the inspector).
        const auto [minWidth, minHeight] = pinInspectorMinimum();
        // The content's minimum (about 885 px) plus the sidebar's widest.
        AdwBreakpoint *dock = adw_breakpoint_new(adw_breakpoint_condition_new_length(
            ADW_BREAKPOINT_CONDITION_MIN_WIDTH, std::max(1280, minWidth + 360), ADW_LENGTH_UNIT_PX));
        GValue collapsed = G_VALUE_INIT;
        g_value_init(&collapsed, G_TYPE_BOOLEAN);
        g_value_set_boolean(&collapsed, FALSE);
        adw_breakpoint_add_setter(dock, G_OBJECT(m_inspectorSplit), "collapsed", &collapsed);
        g_value_unset(&collapsed);
        adw_application_window_add_breakpoint(m_window, dock);

        GtkWidget *content = gtk_overlay_new();
        g_object_ref(m_mainPaned);
        adw_toolbar_view_set_content(m_toolbarView, GTK_WIDGET(m_inspectorSplit));
        gtk_overlay_set_child(GTK_OVERLAY(content), m_mainPaned);
        g_object_unref(m_mainPaned);
        adw_overlay_split_view_set_content(m_inspectorSplit, content);

        GtkWidget *toggle = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-right-symbolic");
        gtk_widget_add_css_class(toggle, "osd");
        gtk_widget_add_css_class(toggle, "circular");
        gtk_widget_set_halign(toggle, GTK_ALIGN_END);
        gtk_widget_set_valign(toggle, GTK_ALIGN_START);
        gtk_widget_set_margin_top(toggle, 8);
        gtk_widget_set_margin_end(toggle, 8);
        gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, "Inspector", -1);
        setTooltip(toggle, "inspector.toggle");
        g_object_bind_property(m_inspectorSplit, "show-sidebar", toggle, "active",
                               static_cast<GBindingFlags>(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
        gtk_overlay_add_overlay(GTK_OVERLAY(content), toggle);

        GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(sidebar, "inspector");
        m_inspectorStack = ADW_VIEW_STACK(adw_view_stack_new());
        gtk_widget_set_vexpand(GTK_WIDGET(m_inspectorStack), TRUE);
        GtkWidget *switcher = adw_view_switcher_new();
        adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_NARROW);
        adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), m_inspectorStack);
        gtk_widget_set_hexpand(switcher, TRUE);
        // The tabs and, at their end, a button that hides the pane: in the
        // narrow layout the pane covers the corner toggle.
        GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_add_css_class(tabs, "inspector-tabs");
        gtk_box_append(GTK_BOX(tabs), switcher);
        GtkWidget *hide = gtk_button_new_from_icon_name("sidebar-show-right-symbolic");
        gtk_widget_add_css_class(hide, "flat");
        gtk_widget_set_valign(hide, GTK_ALIGN_CENTER);
        gtk_accessible_update_property(GTK_ACCESSIBLE(hide), GTK_ACCESSIBLE_PROPERTY_LABEL, "Hide inspector", -1);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(hide), "win.toggle-inspector");
        setTooltip(hide, "inspector.hide");
        gtk_box_append(GTK_BOX(tabs), hide);
        gtk_box_append(GTK_BOX(sidebar), tabs);
        gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(m_inspectorStack));
        adw_overlay_split_view_set_sidebar(m_inspectorSplit, sidebar);

        // Remembered per user when docked (the narrow layout floats over the
        // picture, so it starts and goes back hidden there).
        g_signal_connect(m_inspectorSplit, "notify::collapsed", G_CALLBACK(&onInspectorCollapsedTrampoline), this);
        g_signal_connect(m_inspectorSplit, "notify::show-sidebar", G_CALLBACK(&onInspectorShownTrampoline), this);
    }
    adw_view_stack_add_titled_with_icon(m_inspectorStack, page.widget, page.id, page.title, page.iconName);
}

void AppWindow::toggleInspectorActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    if (!self->m_inspectorSplit)
        return; // no add-on adds an inspector page
    adw_overlay_split_view_set_show_sidebar(self->m_inspectorSplit,
                                            !adw_overlay_split_view_get_show_sidebar(self->m_inspectorSplit));
}

void AppWindow::onInspectorCollapsedTrampoline(GObject *, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    const bool docked = !adw_overlay_split_view_get_collapsed(self->m_inspectorSplit);
    adw_overlay_split_view_set_show_sidebar(self->m_inspectorSplit, docked && self->m_settings->showInspector());
}

void AppWindow::onInspectorShownTrampoline(GObject *, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    if (!adw_overlay_split_view_get_collapsed(self->m_inspectorSplit))
        self->m_settings->setShowInspector(adw_overlay_split_view_get_show_sidebar(self->m_inspectorSplit));
}

void AppWindow::showInspectorPage(const char *id)
{
    if (!m_inspectorSplit || !adw_view_stack_get_child_by_name(m_inspectorStack, id))
        return;
    adw_view_stack_set_visible_child_name(m_inspectorStack, id);
    adw_overlay_split_view_set_show_sidebar(m_inspectorSplit, TRUE);
}

void AppWindow::addActions(const std::vector<ActionSpec> &specs, gpointer target)
{
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(m_window));
    for (const ContributedAction &action : contributeActions(specs, target)) {
        GSimpleAction *simple = g_simple_action_new(action.spec.name, nullptr);
        g_signal_connect(simple, "activate", G_CALLBACK(action.spec.activated), action.target);
        g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(simple));
        if (app)
            setAccelsForAction(app, action.spec.name, action.spec.accels);
        // A shortcut with no Ctrl, Alt or Super would type in a text field.
        for (const char *accel : action.spec.accels) {
            guint key = 0;
            GdkModifierType mods{};
            if (gtk_accelerator_parse(accel, &key, &mods) &&
                !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
                m_typingKeyActions.push_back(action.spec.name);
                // Registered while a text field has focus: off until it leaves.
                g_simple_action_set_enabled(simple, m_transportActionsEnabled);
                break;
            }
        }
        g_object_unref(simple);
    }
}

void AppWindow::addHeaderButton(GtkWidget *button)
{
    // Shell extensions run after buildUi(); before it, this is a misuse.
    g_return_if_fail(m_appHeaderGroup && m_settingsButton);
    gtk_box_insert_child_after(GTK_BOX(m_appHeaderGroup), button, gtk_widget_get_prev_sibling(m_settingsButton));
    // A wider header widens the pinned minimum (drop-ins run in no fixed
    // order: a page's breakpoint may already be set).
    if (m_inspectorSplit)
        pinInspectorMinimum();
}

std::pair<int, int> AppWindow::pinInspectorMinimum()
{
    // With a breakpoint (the docking inspector) the window no longer takes
    // its minimum size from its content, so the content's own minimum is
    // pinned as a size request: measured on the content, since the window
    // itself then only reports the request.
    GtkWidget *content = adw_application_window_get_content(m_window);
    int minWidth = 0, minHeight = 0;
    gtk_widget_measure(content, GTK_ORIENTATION_HORIZONTAL, -1, &minWidth, nullptr, nullptr, nullptr);
    gtk_widget_measure(content, GTK_ORIENTATION_VERTICAL, minWidth, &minHeight, nullptr, nullptr, nullptr);
    gtk_widget_set_size_request(GTK_WIDGET(m_window), minWidth, minHeight);
    return {minWidth, minHeight};
}

void AppWindow::addHints(const std::vector<HintSpec> &hints)
{
    registerHints(hints);
}

void AppWindow::addPreviewOverlay(GtkWidget *overlay)
{
    if (!m_previewOverlay) {
        // The first overlay stacks the preview picture under a GtkOverlay,
        // the same size, inside the same frame.
        m_previewOverlay = GTK_OVERLAY(gtk_overlay_new());
        g_object_ref(m_preview);
        gtk_frame_set_child(GTK_FRAME(m_previewFrame), GTK_WIDGET(m_previewOverlay));
        gtk_overlay_set_child(m_previewOverlay, GTK_WIDGET(m_preview));
        g_object_unref(m_preview);
    }
    gtk_overlay_add_overlay(m_previewOverlay, overlay);
    m_previewOverlays.push_back(overlay);
}

PreviewMapping AppWindow::previewMapping() const
{
    const core::Profile &profile = m_model.sequence().profile;
    // The shown image's own aspect (a scaled preview keeps it); the
    // profile's until the first frame arrives.
    double aspect = displayAspectOf(profile);
    if (GdkPaintable *paintable = gtk_picture_get_paintable(m_preview)) {
        const double own = gdk_paintable_get_intrinsic_aspect_ratio(paintable);
        if (own > 0.0)
            aspect = own;
    }
    return mapPreview(gtk_widget_get_width(GTK_WIDGET(m_preview)), gtk_widget_get_height(GTK_WIDGET(m_preview)),
                      profile.width, profile.height, aspect);
}

void AppWindow::redrawPreviewOverlays()
{
    for (GtkWidget *overlay : m_previewOverlays)
        gtk_widget_queue_draw(overlay);
}

void AppWindow::addTimelineOverlay(timeline::TimelineOverlayProvider *provider)
{
    m_timelineOverlays.push_back(provider);
    refreshTimeline(); // its lanes change the timeline's height
}

void AppWindow::redrawTimeline()
{
    refreshTimeline();
}

timeline::TimelineOverlayProvider *AppWindow::overlayClaimsPress(double x, double y, int nPress)
{
    if (m_timelineOverlays.empty())
        return nullptr;
    const timeline::RowLayout layout = rowLayout();
    for (timeline::TimelineOverlayProvider *overlay : m_timelineOverlays) {
        if (overlay->pressed(m_model, m_viewport, layout, x, y, nPress)) {
            gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
            return overlay;
        }
    }
    return nullptr;
}

std::string AppWindow::projectFolder() const
{
    std::error_code ec;
    if (!m_currentProjectPath.empty())
        return core::utf8String(core::pathFromUtf8(m_currentProjectPath).parent_path());
    const std::string folder = m_settings->defaultProjectFolder();
    if (!folder.empty() && std::filesystem::is_directory(core::pathFromUtf8(folder), ec))
        return folder;
    return {};
}

void AppWindow::assetChangedOnDisk(core::AssetId id)
{
    if (!m_model.hasAsset(id))
        return;
    const core::Asset &asset = m_model.asset(id);
    const std::string fingerprint = core::fileFingerprint(asset.path);
    const auto status = fingerprint.empty() ? core::Asset::Status::Missing : core::Asset::Status::Ready;
    if (fingerprint == asset.fileFingerprint && status == asset.status)
        return;
    // A new fingerprint changes the bin, so the next build is a full one
    // (EngineSync::setProject()), and drop-in producers are made afresh.
    m_model.setAssetSource(id, asset.path, fingerprint, status);
    m_engine->publish(m_model.snapshot());
    refreshTimeline();
    refreshMediaBrowser();
    refreshMissingBanner();
}

void AppWindow::addImportHandler(ImportHandler handler)
{
    for (const std::string &extension : handler.extensions) {
        for (const ImportHandler &existing : m_importHandlers) {
            if (std::find(existing.extensions.begin(), existing.extensions.end(), extension) !=
                existing.extensions.end()) {
                Log::warn("[import] refused a handler for ." + extension + ": another drop-in has it");
                return;
            }
        }
    }
    m_importHandlers.push_back(std::move(handler));
}

std::vector<std::string> AppWindow::importWithHandlers(std::vector<std::string> paths,
                                                       std::optional<core::TrackId> trackId,
                                                       std::optional<core::FrameIndex> &position)
{
    std::vector<std::string> rest;
    for (std::string &path : paths) {
        std::string extension = core::utf8String(core::pathFromUtf8(path).extension());
        if (!extension.empty())
            extension.erase(0, 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto handler = std::find_if(m_importHandlers.begin(), m_importHandlers.end(), [&](const ImportHandler &h) {
            return std::find(h.extensions.begin(), h.extensions.end(), extension) != h.extensions.end();
        });
        if (extension.empty() || handler == m_importHandlers.end()) {
            rest.push_back(std::move(path));
            continue;
        }
        const uint64_t statusBefore = m_statusCount;
        auto result = handler->import(path, trackId, position);
        if (!result) {
            showStatus(result.error());
            continue;
        }
        if (*result && position)
            position = **result;
        // The handler's own report (captions: how many, what was skipped)
        // stays; the plain one only when it said nothing.
        if (m_statusCount == statusBefore)
            showStatus("Imported " + path);
    }
    return rest;
}

// --- GTK signal trampolines ---

gboolean AppWindow::shellSelectionIdleTrampoline(gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_shellSelectionIdleId = 0;
    ShellSelection now = self->currentSelection();
    if (now != self->m_lastShellSelection) {
        self->m_lastShellSelection = std::move(now);
        self->m_shellSelectionChanged.emit();
    }
    return G_SOURCE_REMOVE;
}

} // namespace ustudio::app
