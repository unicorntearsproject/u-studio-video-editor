#include "app/rack.h"

#include "app/catalog.h"
#include "app/preview_tools.h"
#include "app/shell_host.h"
#include "core/blocks.h"
#include "core/commands.h"
#include "core/descriptor.h"
#include "core/keyframes.h"
#include "core/log.h"
#include "core/transform_edit.h"
#include "core/commands/primitives.h"
#include "core/model/animation.h"
#include "core/model/transform.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <string_view>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ustudio::effects {

namespace {

// What the Rack edits: the selected clip, that clip's track, or the whole
// sequence (doc 15: "clip, several clips, track header ... or the
// sequence"; several clips at once is a later slice).
enum class Scope
{
    Clip,
    Track,
    Sequence,
    Block, // the adjustment block the FX lane selected (app/fx_lane.h)
};

// A new undo step when a control rests this long between changes; closer
// changes (a slider drag, typing) merge into one.
constexpr gint64 kGestureGapUs = 600'000;

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

const char *costLabel(CostBadge badge)
{
    switch (badge) {
    case CostBadge::Light:
        return "light";
    case CostBadge::Medium:
        return "medium";
    case CostBadge::Heavy:
        return "heavy";
    }
    return "";
}

const char *costClass(CostBadge badge)
{
    switch (badge) {
    case CostBadge::Light:
        return "success";
    case CostBadge::Medium:
        return "warning";
    case CostBadge::Heavy:
        return "error";
    }
    return "";
}

class Rack;

// One control bound to one effect parameter (or the mix), or to one of a
// clip's transform values (`transform` set; `effect` unused).
struct Control
{
    Rack *rack;
    core::EffectId effect;
    std::vector<core::EffectId> twins; // the same effect on the other selected clips
    std::string param;                 // empty: the mix
    ParamKind kind = ParamKind::Scalar;
    std::optional<DisplayMap> display;
    GtkWidget *widget = nullptr;         // spin, switch, colour button, drop-down, entry
    GtkAdjustment *adjustment = nullptr; // scalar, integer, mix
    std::vector<GtkAdjustment *> rect;   // x, y, w, h
    std::vector<std::string> choices;
    std::vector<std::string> extensions; // a file's, for its chooser
    uint64_t gesture = 0;
    gint64 lastChange = 0;
    // Keyframes (animatable numbers and the mix): previous, pin, next, and
    // the key-at-playhead's feel.
    bool animatable = false;
    GtkWidget *pin = nullptr, *previous = nullptr, *next = nullptr, *feel = nullptr;
    GtkWidget *arm = nullptr; // touch-record
    std::vector<core::Easing> feelEasings; // the feel drop-down's entries, in order
    std::optional<TransformField> transform;
    core::ClipId clip; // the transform's
};

// A card's mask controls (doc 15, "Mix and masks"; FX4).
struct MaskControl
{
    Rack *rack;
    core::EffectId effect;
    GtkWidget *shape = nullptr, *handles = nullptr, *invert = nullptr, *feather = nullptr;
    GtkAdjustment *featherAdjustment = nullptr;
    uint64_t gesture = 0;
    gint64 lastChange = 0;
};

// A card's own buttons.
struct CardAction
{
    Rack *rack;
    core::EffectId effect;
    std::vector<core::EffectId> twins;
    int move; // -1 up, +1 down, 0 remove
};

class Rack
{
  public:
    Rack(app::ShellHost &host, Catalog &catalog) : m_host(host), m_catalog(catalog) {}

    void install()
    {
        build();
        m_host.addHints({
            {"effects.rack-scope", "Effects", "Effects on",
             "The selected clip, its whole track, or the whole sequence (the finished picture)", nullptr, nullptr},
            {"effects.rack-add", "Effects", "Add effect", "Search every effect this install offers", "effects-browser",
             nullptr},
            {"effects.card-bypass", "Effects", "Effect on or off", "Off keeps the effect and its settings", nullptr,
             nullptr},
            {"effects.card-up", "Effects", "Move up", "Effects apply top to bottom", nullptr, nullptr},
            {"effects.card-down", "Effects", "Move down", "Effects apply top to bottom", nullptr, nullptr},
            {"effects.card-remove", "Effects", "Remove effect", nullptr, nullptr, nullptr},
            {"effects.card-mix", "Effects", "Mix",
             "How much of the effect shows: 0% is the picture without it, 100% the full effect", nullptr, nullptr},
            {"effects.key-pin", "Effects", "Pin",
             "A keyframe here at the playhead, or remove the one here. The first pin makes the value change over "
             "time; move the playhead and change the value to add the next",
             "effects-pin", nullptr},
            {"effects.key-previous", "Effects", "Previous keyframe", nullptr, nullptr, nullptr},
            {"effects.key-next", "Effects", "Next keyframe", nullptr, nullptr, nullptr},
            {"effects.key-feel", "Effects", "Feel",
             "How the value moves from this keyframe to the next: steady, smooth, easing in or out, bouncing, "
             "or holding still until the next",
             nullptr, nullptr},
            {"effects.key-arm", "Effects", "Touch-record",
             "Armed, moving this value records it as you go (play, then move it); when you let go, just enough "
             "keyframes are kept to follow what you did",
             nullptr, nullptr},
            {"effects.block-affects", "Effects", "Affects",
             "Which tracks the adjustment block changes: every track, or a track and those below it", nullptr, nullptr},
            {"effects.block-remove", "Effects", "Remove adjustment block", nullptr, nullptr, nullptr},
            {"effects.choose-file", "Effects", "Choose a file", nullptr, nullptr, nullptr},
            {"effects.mask", "Effects", "Mask",
             "Limits the effect to a rectangle or an ellipse of the picture; place it with Edit on the picture",
             nullptr, nullptr},
            {"effects.mask-invert", "Effects", "Invert mask", "The effect everywhere but inside the shape", nullptr,
             nullptr},
            {"effects.mask-feather", "Effects", "Soft edge", "How gradually the effect fades out at the shape's edge",
             nullptr, nullptr},
            {"effects.rack-menu", "Effects", "Effects menu", "Copy, paste, and save as a look", nullptr, nullptr},
            {"effects.transform", "Effects", "Transform",
             "Where the clip's picture sits: position, size and rotation. Pin a value to keyframe it, as with "
             "an effect's",
             nullptr, nullptr},
            {"effects.transform-x", "Effects", "X", "The picture's centre, in pixels from the frame's left edge",
             nullptr, nullptr},
            {"effects.transform-y", "Effects", "Y", "The picture's centre, in pixels from the frame's top edge",
             nullptr, nullptr},
            {"effects.transform-width", "Effects", "Width", "The picture's width on screen, in pixels", nullptr,
             nullptr},
            {"effects.transform-height", "Effects", "Height", "The picture's height on screen, in pixels", nullptr,
             nullptr},
            {"effects.transform-rotation", "Effects", "Rotation", "Degrees clockwise, about the picture's centre",
             nullptr, nullptr},
            {"effects.card-cost", "Effects", "Cost",
             "How much work the effect is for each frame, measured when it was checked", nullptr, nullptr},
        });
        // P pins (doc 15, "Keyboard summary"); E belongs to the Browser.
        static const std::vector<app::ActionSpec> actions = {
            {"effects-pin", "Pin the value at the playhead", "Effects", {"p"}, &onPinActionTrampoline},
            {"effects-copy", "Copy effects", "Effects", {"<Ctrl><Shift>c"}, &onCopyTrampoline},
            {"effects-paste", "Paste effects…", "Effects", {"<Ctrl><Shift>v"}, &onPasteTrampoline},
            {"effects-paste-append", "Paste effects after these", "Effects", {}, &onPasteAppendTrampoline},
            {"effects-paste-replace", "Paste effects instead of these", "Effects", {}, &onPasteReplaceTrampoline},
            {"effects-save-look", "Save effects as a look…", "Effects", {}, &onSaveLookTrampoline},
        };
        m_host.addActions(actions, this);
        m_host.setTooltip(m_scope, "effects.rack-scope");
        m_host.setTooltip(m_add, "effects.rack-add");
        m_host.setTooltip(m_menu, "effects.rack-menu");
        m_host.setTooltip(m_blockLane, "effects.block-affects");
        m_host.setTooltip(m_blockRemove, "effects.block-remove");
        m_host.setTooltip(m_compare, "effects.compare");
        m_host.addInspectorPage({"effects.rack", "Effects", "applications-graphics-symbolic", m_root});
        m_host.selectionChanged().connect([this] { refresh(); });
        m_host.projectChanged().connect([this] { refresh(); });
        m_host.playheadMoved().connect([this] { onPlayheadMoved(); });
        m_catalog.blockSelected.connect([this] { onBlockSelected(); });
        m_catalog.changed.connect([this] {
            m_structure.clear(); // names may have arrived
            refresh();
        });
        // A badge: rebuild only when that effect is on show.
        m_catalog.healthChanged.connect([this](const std::string &service) {
            std::optional<core::Model::EffectTarget> target = currentTarget();
            if (!target || !m_host.model().hasEffectTarget(*target))
                return;
            for (const core::Effect &e : m_host.model().effects(*target))
                if (e.service == service) {
                    m_structure.clear();
                    refresh();
                    return;
                }
        });
        refresh();
    }

    void onScopeChanged()
    {
        m_structure.clear();
        refresh();
    }

    void onControlChanged(Control &control)
    {
        if (m_updating)
            return;
        const gint64 now = g_get_monotonic_time();
        if (control.gesture == 0 || now - control.lastChange > kGestureGapUs)
            control.gesture = ++m_nextGesture;
        control.lastChange = now;
        const core::Model &model = m_host.model();
        if (control.transform) {
            m_lastControl = std::make_pair(control.effect, control.param);
            if (isArmed(control))
                record(control);
            else
                onTransformChanged(control);
            return;
        }
        if (!model.hasEffect(control.effect))
            return;
        if (!control.twins.empty()) {
            // Several clips: the same value on each, one undo step a gesture.
            std::vector<core::EffectId> all = control.twins;
            all.insert(all.begin(), control.effect);
            if (control.param.empty()) {
                m_host.execute(setMixOnAll(model, all, numberOf(control), control.gesture));
                return;
            }
            core::Param param = currentParam(model.effect(control.effect), control.param, control.kind);
            param.value = readControl(control);
            m_host.execute(setParamOnAll(all, param, control.gesture));
            return;
        }
        if (control.animatable)
            m_lastControl = std::make_pair(control.effect, control.param);
        if (control.animatable && isArmed(control)) {
            record(control);
            return;
        }
        // Animated: the change is the key at the playhead (set, or added).
        const std::vector<core::Keyframe> keys = keysOf(control);
        if (!keys.empty()) {
            applyKeys(control, withKeyAt(keys, frameOf(control), numberOf(control)), control.gesture);
            return;
        }
        if (control.param.empty()) {
            core::KeyframedValue mix = model.effect(control.effect).mix;
            mix.value = std::clamp(gtk_adjustment_get_value(control.adjustment) / 100.0, 0.0, 1.0);
            m_host.execute(std::make_unique<SetMix>(control.effect, mix, control.gesture));
            return;
        }
        core::Param param = currentParam(model.effect(control.effect), control.param, control.kind);
        param.value = readControl(control);
        m_host.execute(std::make_unique<SetParam>(control.effect, param, control.gesture));
    }

    // --- Touch-record (doc 15, "Keyframes that feel musical") --------------
    //
    // An armed control records what it's set to at each frame while it's
    // moved (playing or not), and a key at the playhead follows it live so
    // the picture does. When the gesture ends (no change for a moment) the
    // recording replaces the keys over its range, thinned (withRecording()),
    // in the same undo step. While it records, the playhead doesn't move
    // the control (updateValues()), or it would fight the hand.

    using ControlKey = std::pair<uint64_t, std::string>;
    struct Recording
    {
        ControlKey key;
        std::vector<std::pair<core::FrameIndex, double>> performed;
        uint64_t gesture = 0;
        double minimum = 0.0, maximum = 1.0;
        guint timer = 0;
        std::optional<TransformField> transform;
    };

    static ControlKey keyOf(const Control &control)
    {
        return {control.transform ? control.clip.value : control.effect.value, control.param};
    }

    bool isArmed(const Control &control) const
    {
        return m_armed.contains(keyOf(control));
    }

    bool isRecording(const Control &control) const
    {
        return m_recording && m_recording->key == keyOf(control);
    }

    void onArmToggled(Control &control)
    {
        const bool armed = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control.arm));
        if (armed)
            m_armed.insert(keyOf(control));
        else
            m_armed.erase(keyOf(control));
        showArmed(control, armed);
    }

    // Armed reads as a record button (red), not just a pressed one.
    static void showArmed(Control &control, bool armed)
    {
        if (armed)
            gtk_widget_add_css_class(control.arm, "destructive-action");
        else
            gtk_widget_remove_css_class(control.arm, "destructive-action");
    }

    void record(Control &control)
    {
        if (m_recording && m_recording->key != keyOf(control))
            finishRecording();
        if (!m_recording) {
            m_recording = Recording{keyOf(control), {}, control.gesture, 0.0, 1.0, 0, control.transform};
            const double scale = control.param.empty() ? 100.0 : 1.0; // the mix shows percent
            m_recording->minimum = gtk_adjustment_get_lower(control.adjustment) / scale;
            m_recording->maximum = gtk_adjustment_get_upper(control.adjustment) / scale;
            core::Log::debug("[effects] touch-record: started");
        }
        const core::FrameIndex at = frameOf(control);
        const double value = numberOf(control);
        m_recording->performed.emplace_back(at, value);
        applyKeys(control, withKeyAt(keysOf(control), at, value), m_recording->gesture);
        if (m_recording->timer)
            g_source_remove(m_recording->timer);
        m_recording->timer = g_timeout_add(static_cast<guint>(kGestureGapUs / 1000), &onRecordingIdleTrampoline, this);
    }

    void finishRecording()
    {
        if (!m_recording)
            return;
        Recording recording = std::move(*m_recording);
        m_recording.reset();
        if (recording.timer)
            g_source_remove(recording.timer);
        const core::Model &model = m_host.model();
        const double tolerance = recordingTolerance(recording.minimum, recording.maximum);
        if (recording.transform) {
            const core::ClipId clip{recording.key.first};
            if (!model.hasClip(clip))
                return;
            core::Transform t = model.clip(clip).transform.get();
            core::KeyframedValue &value = fieldOf(t, *recording.transform);
            value.keyframes = withRecording(value.keyframes, recording.performed, tolerance);
            m_host.execute(std::make_unique<core::SetClipTransform>(clip, t, recording.gesture));
            core::Log::debug("[effects] touch-record: " + std::to_string(recording.performed.size()) +
                             " transform values recorded");
            m_host.showStatus("Recorded: the keyframes follow what you did");
            return;
        }
        const core::EffectId id{recording.key.first};
        if (!model.hasEffect(id))
            return;
        const core::Effect &effect = model.effect(id);
        if (recording.key.second.empty()) {
            core::KeyframedValue mix = effect.mix;
            mix.keyframes = withRecording(mix.keyframes, recording.performed, tolerance);
            m_host.execute(std::make_unique<SetMix>(id, mix, recording.gesture));
        } else {
            auto param = std::find_if(effect.params.begin(), effect.params.end(),
                                      [&](const core::Param &p) { return p.name == recording.key.second; });
            if (param == effect.params.end())
                return;
            core::Param updated = *param;
            updated.keyframes = withRecording(updated.keyframes, recording.performed, tolerance);
            m_host.execute(std::make_unique<SetParam>(id, updated, recording.gesture));
        }
        core::Log::debug("[effects] touch-record: " + std::to_string(recording.performed.size()) + " values recorded");
        m_host.showStatus("Recorded: the keyframes follow what you did");
    }

    // --- Keyframes ---------------------------------------------------------

    // The playhead in the effect owner's frames: a clip's keys count from
    // its first frame on the timeline, a track's and the sequence's from 0.
    core::FrameIndex ownerFrame(core::EffectId effect) const
    {
        const core::Model &model = m_host.model();
        const core::FrameIndex frame = m_host.currentFrame();
        auto found = model.findEffect(effect);
        if (found && found->first.kind == core::Model::EffectTarget::Kind::Clip)
            return frame - model.clip(core::ClipId{found->first.id}).position;
        if (found && found->first.kind == core::Model::EffectTarget::Kind::AdjustmentBlock)
            return frame - model.adjustmentBlock(core::AdjustmentBlockId{found->first.id}).start;
        return frame;
    }

    core::FrameIndex ownerStart(core::EffectId effect) const
    {
        return m_host.currentFrame() - ownerFrame(effect);
    }

    // The playhead in the control's own frames: a transform's keys, like a
    // clip's effects', count from the clip's first frame.
    core::FrameIndex frameOf(const Control &control) const
    {
        if (control.transform) {
            const core::Model &model = m_host.model();
            return model.hasClip(control.clip) ? m_host.currentFrame() - model.clip(control.clip).position : 0;
        }
        return ownerFrame(control.effect);
    }

    core::FrameIndex startOf(const Control &control) const
    {
        return m_host.currentFrame() - frameOf(control);
    }

    // --- The clip's transform (ADR-018; keys through withTransformAt()) ----

    // The clip whose Transform card shows: one clip on show with a picture.
    std::optional<core::ClipId> transformClip(const core::Model::EffectTarget &target) const
    {
        const core::Model &model = m_host.model();
        if (multiple() || target.kind != core::Model::EffectTarget::Kind::Clip)
            return std::nullopt;
        const core::ClipId clip{target.id};
        if (!model.hasClip(clip) || !model.hasAsset(model.clip(clip).asset) ||
            !model.asset(model.clip(clip).asset).info.hasVideo)
            return std::nullopt;
        return clip;
    }

    // The clip's transform as an explicit placement: a Fit or Stretch
    // picture where it shows now (keys need one; transformProblem()).
    core::Transform placedTransform(core::ClipId clip) const
    {
        const core::Model &model = m_host.model();
        const core::Clip &c = model.clip(clip);
        const core::Transform &t = c.transform.get();
        if (t.bounds == core::Transform::Bounds::None)
            return t;
        const auto &info = model.asset(c.asset).info;
        return core::explicitTransform(t, info.width, info.height, model.sequence().profile);
    }

    // The card's note for a picture fitted or stretched to the frame.
    void showTransformNote()
    {
        const core::Model &model = m_host.model();
        const Control *any = nullptr;
        for (const std::unique_ptr<Control> &control : m_controls)
            if (control->transform)
                any = control.get();
        if (!any || !model.hasClip(any->clip))
            return;
        const core::Transform::Bounds bounds = model.clip(any->clip).transform.get().bounds;
        gtk_widget_set_visible(m_transformNote, bounds != core::Transform::Bounds::None);
        gtk_label_set_text(GTK_LABEL(m_transformNote),
                           bounds == core::Transform::Bounds::Stretch
                               ? "Stretched to the frame. Change a value to place it yourself."
                               : "Fitted to the frame. Change a value to place it yourself.");
    }

    // A value changed by hand (core/transform_edit.h: withValue()).
    void onTransformChanged(Control &control)
    {
        const core::Model &model = m_host.model();
        if (!model.hasClip(control.clip))
            return;
        const core::Transform next = withValue(model.clip(control.clip).transform.get(), placedTransform(control.clip),
                                               *control.transform, frameOf(control), numberOf(control));
        m_host.execute(std::make_unique<core::SetClipTransform>(control.clip, next, control.gesture));
    }

    // Position, size and rotation, each keyframeable like an effect's value.
    GtkWidget *buildTransformCard(core::ClipId clip)
    {
        const core::Model &model = m_host.model();
        const core::Profile &profile = model.sequence().profile;

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(card, "card");
        GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(inner, 10);
        gtk_widget_set_margin_end(inner, 10);
        gtk_widget_set_margin_top(inner, 8);
        gtk_widget_set_margin_bottom(inner, 8);
        gtk_box_append(GTK_BOX(card), inner);
        GtkWidget *title = gtk_label_new("Transform");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_add_css_class(title, "heading");
        m_host.setTooltip(title, "effects.transform");
        gtk_box_append(GTK_BOX(inner), title);
        // Updated with the values, not rebuilt: the first change places a
        // fitted picture, and a rebuild then would take the field being
        // typed in away from under the hand.
        m_transformNote = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(m_transformNote), TRUE);
        gtk_label_set_xalign(GTK_LABEL(m_transformNote), 0.0f);
        gtk_widget_add_css_class(m_transformNote, "dim-label");
        gtk_widget_add_css_class(m_transformNote, "caption");
        gtk_box_append(GTK_BOX(inner), m_transformNote);

        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        const double w = profile.width, h = profile.height;
        struct Row
        {
            TransformField field;
            const char *title, *name, *hint;
            double lower, upper, step;
            int digits;
        };
        const Row rows[] = {
            {TransformField::X, "X", "transform.x", "effects.transform-x", -4 * w, 5 * w, 1, 0},
            {TransformField::Y, "Y", "transform.y", "effects.transform-y", -4 * h, 5 * h, 1, 0},
            {TransformField::Width, "Width", "transform.width", "effects.transform-width", 1, 8 * w, 1, 0},
            {TransformField::Height, "Height", "transform.height", "effects.transform-height", 1, 8 * h, 1, 0},
            {TransformField::Rotation, "Rotation", "transform.rotation", "effects.transform-rotation", -3600, 3600, 0.5,
             1},
        };
        int row = 0;
        for (const Row &r : rows) {
            auto control = std::make_unique<Control>();
            control->rack = this;
            control->transform = r.field;
            control->clip = clip;
            control->param = r.name;
            control->animatable = true;
            control->adjustment = gtk_adjustment_new(0.0, r.lower, r.upper, r.step, r.step * 10, 0.0);
            GtkWidget *spin = gtk_spin_button_new(control->adjustment, r.step, static_cast<guint>(r.digits));
            // Sized for the value, not the range: sized for "40960" the
            // card pushed the inspector to twice its width.
            gtk_editable_set_width_chars(GTK_EDITABLE(spin), 7);
            gtk_accessible_update_property(GTK_ACCESSIBLE(spin), GTK_ACCESSIBLE_PROPERTY_LABEL, r.title, -1);
            m_host.setTooltip(spin, r.hint);
            control->widget = spin;
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            addRow(grid, row, r.title, spin);
            addKeyControls(grid, row, *control);
            row += 2;
            m_controls.push_back(std::move(control));
        }
        gtk_box_append(GTK_BOX(inner), grid);
        return card;
    }

    std::vector<core::Keyframe> keysOf(const Control &control) const
    {
        const core::Model &model = m_host.model();
        if (control.transform)
            return model.hasClip(control.clip)
                       ? fieldOf(model.clip(control.clip).transform.get(), *control.transform).keyframes
                       : std::vector<core::Keyframe>{};
        if (!model.hasEffect(control.effect))
            return {};
        const core::Effect &effect = model.effect(control.effect);
        if (control.param.empty())
            return effect.mix.keyframes;
        return currentParam(effect, control.param, control.kind).keyframes;
    }

    // The control's number in model units (the mix 0-1).
    double numberOf(const Control &control) const
    {
        if (control.transform)
            return gtk_adjustment_get_value(control.adjustment);
        if (control.param.empty())
            return std::clamp(gtk_adjustment_get_value(control.adjustment) / 100.0, 0.0, 1.0);
        return asNumber(readControl(control));
    }

    // Sets the control's keys (empty: not animated, at the value it showed).
    void applyKeys(Control &control, std::vector<core::Keyframe> keys, uint64_t gesture)
    {
        const core::Model &model = m_host.model();
        if (control.transform) {
            if (!model.hasClip(control.clip))
                return;
            // Keys need an explicit placement (transformProblem()): a Fit
            // or Stretch picture becomes one where it shows now.
            m_host.execute(std::make_unique<core::SetClipTransform>(
                control.clip,
                withKeys(placedTransform(control.clip), *control.transform, std::move(keys), numberOf(control)),
                gesture));
            return;
        }
        if (!model.hasEffect(control.effect))
            return;
        const core::Effect &effect = model.effect(control.effect);
        const double shown = numberOf(control);
        if (control.param.empty()) {
            core::KeyframedValue mix = effect.mix;
            if (keys.empty())
                mix.value = shown;
            mix.keyframes = std::move(keys);
            m_host.execute(std::make_unique<SetMix>(control.effect, mix, gesture));
            return;
        }
        core::Param param = currentParam(effect, control.param, control.kind);
        if (keys.empty())
            param.value = shown;
        param.keyframes = std::move(keys);
        m_host.execute(std::make_unique<SetParam>(control.effect, param, gesture));
    }

    // The pin: a key at the playhead, or none there. The first pin turns
    // animation on; removing the last turns it off at that value.
    void onKeyPin(Control &control)
    {
        m_lastControl = std::make_pair(control.effect, control.param);
        const std::vector<core::Keyframe> keys = keysOf(control);
        const core::FrameIndex at = frameOf(control);
        if (keyAt(keys, at))
            applyKeys(control, withoutKeyAt(keys, at), 0);
        else
            applyKeys(control, withKeyAt(keys, at, numberOf(control)), 0);
    }

    void onKeyStep(Control &control, bool forward)
    {
        const std::vector<core::Keyframe> keys = keysOf(control);
        const core::FrameIndex at = frameOf(control);
        const std::optional<core::FrameIndex> key = forward ? nextKey(keys, at) : previousKey(keys, at);
        if (key)
            m_host.seek(startOf(control) + *key);
    }

    void onKeyFeel(Control &control)
    {
        if (m_updating)
            return;
        const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(control.feel));
        if (index >= control.feelEasings.size())
            return;
        const std::vector<core::Keyframe> keys = keysOf(control);
        const core::FrameIndex at = frameOf(control);
        const core::Keyframe *key = keyAt(keys, at);
        if (!key || key->easing == control.feelEasings[index])
            return;
        applyKeys(control, withEasingAt(keys, at, control.feelEasings[index]), 0);
    }

    // P: pin the parameter last touched.
    void onPinAction()
    {
        for (const std::unique_ptr<Control> &control : m_controls)
            if (m_lastControl && control->animatable && control->effect == m_lastControl->first &&
                control->param == m_lastControl->second) {
                onKeyPin(*control);
                return;
            }
        m_host.showStatus("Change an effect's value first, then press P to pin it at the playhead");
    }

    // The pin lit when the playhead is on a key; previous/next only when
    // there's one that way; the feel only on a key.
    void showKeyState(Control &control)
    {
        if (!control.pin)
            return;
        const std::vector<core::Keyframe> keys = keysOf(control);
        const core::FrameIndex at = frameOf(control);
        const core::Keyframe *key = keyAt(keys, at);
        gtk_button_set_icon_name(GTK_BUTTON(control.pin), key ? "starred-symbolic" : "non-starred-symbolic");
        if (key)
            gtk_widget_add_css_class(control.pin, "accent");
        else
            gtk_widget_remove_css_class(control.pin, "accent");
        gtk_widget_set_sensitive(control.previous, previousKey(keys, at).has_value());
        gtk_widget_set_sensitive(control.next, nextKey(keys, at).has_value());
        gtk_widget_set_visible(control.feel, key != nullptr);
        gtk_widget_set_visible(GTK_WIDGET(g_object_get_data(G_OBJECT(control.feel), "label")), key != nullptr);
        if (key) {
            auto it = std::find(control.feelEasings.begin(), control.feelEasings.end(), key->easing);
            if (it != control.feelEasings.end())
                gtk_drop_down_set_selected(GTK_DROP_DOWN(control.feel),
                                           static_cast<guint>(it - control.feelEasings.begin()));
        }
    }

    void onCardAction(const CardAction &action)
    {
        const core::Model &model = m_host.model();
        auto found = model.findEffect(action.effect);
        if (!found)
            return;
        if (action.move == 0) {
            if (action.twins.empty()) {
                m_host.execute(std::make_unique<RemoveEffect>(action.effect));
                return;
            }
            std::vector<std::unique_ptr<core::Command>> removes;
            removes.push_back(std::make_unique<RemoveEffect>(action.effect));
            for (core::EffectId twin : action.twins)
                removes.push_back(std::make_unique<RemoveEffect>(twin));
            m_host.execute(std::make_unique<core::CompositeCommand>("Remove effect", std::move(removes)));
            return;
        }
        const size_t size = model.effects(found->first).size();
        const long index = static_cast<long>(found->second) + action.move;
        if (index < 0 || static_cast<size_t>(index) >= size)
            return;
        m_host.execute(std::make_unique<MoveEffect>(action.effect, static_cast<size_t>(index)));
    }

    void onBypass(core::EffectId effect, bool enabled)
    {
        if (m_updating)
            return;
        std::vector<core::EffectId> twins = twinsOf(effect).value_or(std::vector<core::EffectId>{});
        if (twins.empty()) {
            m_host.execute(std::make_unique<SetEffectEnabled>(effect, enabled));
            return;
        }
        std::vector<std::unique_ptr<core::Command>> commands;
        twins.insert(twins.begin(), effect);
        for (core::EffectId id : twins)
            if (m_host.model().hasEffect(id) && m_host.model().effect(id).enabled != enabled)
                commands.push_back(std::make_unique<SetEffectEnabled>(id, enabled));
        m_host.execute(
            std::make_unique<core::CompositeCommand>(enabled ? "Enable effect" : "Bypass effect", std::move(commands)));
    }

    // --- Copy, paste, looks, drops ------------------------------------------

    // Where a paste or a drop goes: every selected clip in clip scope, else
    // the Rack's target.
    std::vector<core::Model::EffectTarget> pasteTargets() const
    {
        std::vector<core::Model::EffectTarget> targets;
        if (static_cast<Scope>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_scope))) == Scope::Clip)
            for (core::ClipId id : selectedClips())
                targets.push_back(core::Model::EffectTarget::clip(id));
        if (targets.empty())
            if (std::optional<core::Model::EffectTarget> target = currentTarget())
                targets.push_back(*target);
        return targets;
    }

    // This drop-in's effects on the Rack's target (the first clip of several).
    std::vector<core::Effect> ownEffects() const
    {
        std::vector<core::Effect> effects;
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (target && m_host.model().hasEffectTarget(*target))
            for (const core::Effect &e : m_host.model().effects(*target))
                if (e.owner == kOwner)
                    effects.push_back(e);
        return effects;
    }

    void onCopy()
    {
        m_catalog.clipboard = ownEffects();
        m_host.showStatus(m_catalog.clipboard.empty()
                              ? "No effects here to copy"
                              : "Copied " + std::to_string(m_catalog.clipboard.size()) + " effects");
    }

    void paste(PasteMode mode)
    {
        gtk_popover_popdown(GTK_POPOVER(m_pastePopover));
        if (m_catalog.clipboard.empty()) {
            m_host.showStatus("Copy some effects first (Ctrl+Shift+C)");
            return;
        }
        m_host.execute(pasteEffects(m_host.model(), pasteTargets(), m_catalog.clipboard, mode,
                                    mode == PasteMode::Append ? "Paste effects" : "Replace effects"));
    }

    void onPaste()
    {
        gtk_popover_popup(GTK_POPOVER(m_pastePopover));
    }

    void onSaveLook()
    {
        if (ownEffects().empty()) {
            m_host.showStatus("Add some effects first, then save them as a look");
            return;
        }
        const std::string name = "Look " + std::to_string(m_host.model().project().looks.size() + 1);
        gtk_editable_set_text(GTK_EDITABLE(m_lookName), name.c_str());
        gtk_popover_popup(GTK_POPOVER(m_lookPopover));
        gtk_widget_grab_focus(m_lookName);
    }

    void onLookSave()
    {
        const std::string name = gtk_editable_get_text(GTK_EDITABLE(m_lookName));
        gtk_popover_popdown(GTK_POPOVER(m_lookPopover));
        if (name.empty())
            return;
        if (m_host.execute(std::make_unique<SaveLook>(core::Look{{}, name, ownEffects()})))
            m_host.showStatus("Saved look " + name + ": it's in the Add page's Looks");
    }

    // A Browser tile dropped on the Rack.
    bool onDrop(const std::string &payload)
    {
        static constexpr std::string_view kPrefix = "ustudio-effects:";
        if (!payload.starts_with(kPrefix))
            return false;
        const std::string item = payload.substr(kPrefix.size());
        const std::vector<core::Effect> effects = m_catalog.effectsFor(m_host.model(), item);
        if (effects.empty())
            return false;
        return m_host.execute(pasteEffects(m_host.model(), pasteTargets(), effects, PasteMode::Append,
                                           "Add " + m_catalog.nameOf(m_host.model(), item)));
    }

    void onSearchChanged()
    {
        fillAddList();
    }

    void onAddRow(GtkListBoxRow *row)
    {
        const char *service = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "service"));
        const EffectDescriptor *descriptor = service ? m_catalog.find(service) : nullptr;
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (!descriptor || !target)
            return;
        gtk_popover_popdown(GTK_POPOVER(m_addPopover));
        const size_t end = m_host.model().effects(*target).size();
        if (!m_host.execute(std::make_unique<AddEffect>(*target, makeEffect(*descriptor), end)))
            m_host.showStatus("Couldn't add " + descriptor->name);
    }

    void onSearchActivate()
    {
        fillAddList(); // search-changed is delayed; the list must match the text now
        if (GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(m_addList), 0))
            if (g_object_get_data(G_OBJECT(first), "service"))
                onAddRow(first);
    }

    void onAddShown()
    {
        gtk_editable_set_text(GTK_EDITABLE(m_search), "");
        fillAddList();
        gtk_widget_grab_focus(m_search);
    }

  private:
    // --- Target ------------------------------------------------------------

    std::vector<core::ClipId> selectedClips() const
    {
        std::vector<core::ClipId> clips;
        for (core::ClipId id : m_host.currentSelection().clips)
            if (m_host.model().hasClip(id))
                clips.push_back(id);
        return clips;
    }

    // With several clips selected (clip scope), the same effect on the other
    // clips: the same-numbered occurrence of its service among each clip's
    // own effects (the first softglow pairs with the first softglow), so a
    // look applied to clips that already differ still lines up. Empty with
    // one clip; nullopt when another clip lacks it (it isn't shared, so the
    // Rack doesn't show it).
    std::optional<std::vector<core::EffectId>> twinsOf(core::EffectId effect) const
    {
        const core::Model &model = m_host.model();
        const std::vector<core::ClipId> clips = selectedClips();
        if (clips.size() < 2 || static_cast<Scope>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_scope))) != Scope::Clip ||
            !model.hasEffect(effect))
            return std::vector<core::EffectId>{};
        const std::string &service = model.effect(effect).service;
        // Which occurrence of the service it is in the first clip.
        std::optional<size_t> occurrence;
        size_t seen = 0;
        for (const core::Effect &e : model.clip(clips.front()).effects)
            if (e.owner == kOwner && e.service == service) {
                if (e.id == effect)
                    occurrence = seen;
                ++seen;
            }
        if (!occurrence)
            return std::nullopt;
        std::vector<core::EffectId> twins;
        for (size_t c = 1; c < clips.size(); ++c) {
            size_t n = 0;
            std::optional<core::EffectId> twin;
            for (const core::Effect &e : model.clip(clips[c]).effects)
                if (e.owner == kOwner && e.service == service && n++ == *occurrence)
                    twin = e.id;
            if (!twin)
                return std::nullopt;
            twins.push_back(*twin);
        }
        return twins;
    }

    bool multiple() const
    {
        return selectedClips().size() > 1 &&
               static_cast<Scope>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_scope))) == Scope::Clip;
    }

    std::optional<core::Model::EffectTarget> currentTarget() const
    {
        const core::Model &model = m_host.model();
        const app::ShellSelection selection = m_host.currentSelection();
        std::optional<core::ClipId> clip;
        for (core::ClipId id : selection.clips)
            if (model.hasClip(id)) {
                clip = id;
                break;
            }
        const auto scope = static_cast<Scope>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_scope)));
        if (scope == Scope::Block) {
            if (m_catalog.selectedBlock && model.hasAdjustmentBlock(*m_catalog.selectedBlock))
                return core::Model::EffectTarget::adjustmentBlock(*m_catalog.selectedBlock);
            return core::Model::EffectTarget::sequence();
        }
        if (scope == Scope::Clip && clip)
            return core::Model::EffectTarget::clip(*clip);
        if (scope != Scope::Sequence) {
            if (clip)
                return core::Model::EffectTarget::track(model.clip(*clip).track);
            if (selection.track && model.hasTrack(*selection.track))
                return core::Model::EffectTarget::track(*selection.track);
        }
        return core::Model::EffectTarget::sequence();
    }

    std::string targetTitle(const core::Model::EffectTarget &target) const
    {
        const core::Model &model = m_host.model();
        switch (target.kind) {
        case core::Model::EffectTarget::Kind::Clip:
            if (multiple())
                return std::to_string(selectedClips().size()) + " clips: the effects they share";
            return "Clip: " + model.clip(core::ClipId{target.id}).name;
        case core::Model::EffectTarget::Kind::Track:
            return "Track: " + model.track(core::TrackId{target.id}).name;
        case core::Model::EffectTarget::Kind::Sequence:
            return "The whole sequence";
        case core::Model::EffectTarget::Kind::AdjustmentBlock: {
            const core::AdjustmentBlock &block = model.adjustmentBlock(core::AdjustmentBlockId{target.id});
            return "Adjustment block: " + std::to_string(block.length) + " frames from frame " +
                   std::to_string(block.start);
        }
        }
        return "";
    }

    // --- Values ------------------------------------------------------------

    const ParamDescriptor *paramDescriptor(const std::string &service, const std::string &name) const
    {
        const EffectDescriptor *descriptor = m_catalog.find(service);
        if (!descriptor)
            return nullptr;
        for (const ParamDescriptor &p : descriptor->params)
            if (p.id == name)
                return &p;
        return nullptr;
    }

    // The effect's parameter, or the descriptor's default when it doesn't
    // carry one yet (left to the service until set).
    core::Param currentParam(const core::Effect &effect, const std::string &name, ParamKind kind) const
    {
        for (const core::Param &p : effect.params)
            if (p.name == name)
                return p;
        core::Param param;
        param.name = name;
        const ParamDescriptor *p = paramDescriptor(effect.service, name);
        param.value = p ? p->defaultValue : parseValue(kind, "");
        return param;
    }

    static double asNumber(const core::Param::Value &value)
    {
        if (const double *d = std::get_if<double>(&value))
            return *d;
        if (const int64_t *i = std::get_if<int64_t>(&value))
            return static_cast<double>(*i);
        if (const bool *b = std::get_if<bool>(&value))
            return *b ? 1.0 : 0.0;
        return 0.0;
    }

    core::Param::Value readControl(const Control &control) const
    {
        switch (control.kind) {
        case ParamKind::Scalar: {
            const double shown = gtk_adjustment_get_value(control.adjustment);
            return control.display ? control.display->fromDisplay(shown) : shown;
        }
        case ParamKind::Integer:
            return static_cast<int64_t>(std::llround(gtk_adjustment_get_value(control.adjustment)));
        case ParamKind::Toggle:
            return static_cast<bool>(gtk_switch_get_active(GTK_SWITCH(control.widget)));
        case ParamKind::Color: {
            const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(control.widget));
            auto channel = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
            return core::Color{channel(rgba->red), channel(rgba->green), channel(rgba->blue), channel(rgba->alpha)};
        }
        case ParamKind::Choice: {
            const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(control.widget));
            return index < control.choices.size() ? control.choices[index] : std::string();
        }
        case ParamKind::Rect:
            return core::Rect{gtk_adjustment_get_value(control.rect[0]), gtk_adjustment_get_value(control.rect[1]),
                              gtk_adjustment_get_value(control.rect[2]), gtk_adjustment_get_value(control.rect[3])};
        case ParamKind::File:
        case ParamKind::Text:
            return std::string(gtk_editable_get_text(GTK_EDITABLE(control.widget)));
        }
        return 0.0;
    }

    void writeControl(Control &control, const core::Param::Value &value)
    {
        switch (control.kind) {
        case ParamKind::Scalar: {
            const double v = asNumber(value);
            gtk_adjustment_set_value(control.adjustment, control.display ? control.display->toDisplay(v) : v);
            break;
        }
        case ParamKind::Integer:
            gtk_adjustment_set_value(control.adjustment, asNumber(value));
            break;
        case ParamKind::Toggle:
            gtk_switch_set_active(GTK_SWITCH(control.widget), asNumber(value) != 0.0);
            break;
        case ParamKind::Color:
            if (const core::Color *c = std::get_if<core::Color>(&value)) {
                const GdkRGBA rgba{c->r / 255.0f, c->g / 255.0f, c->b / 255.0f, c->a / 255.0f};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(control.widget), &rgba);
            }
            break;
        case ParamKind::Choice:
            if (const std::string *s = std::get_if<std::string>(&value)) {
                auto it = std::find(control.choices.begin(), control.choices.end(), *s);
                if (it != control.choices.end())
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(control.widget),
                                               static_cast<guint>(it - control.choices.begin()));
            }
            break;
        case ParamKind::Rect:
            if (const core::Rect *r = std::get_if<core::Rect>(&value)) {
                gtk_adjustment_set_value(control.rect[0], r->x);
                gtk_adjustment_set_value(control.rect[1], r->y);
                gtk_adjustment_set_value(control.rect[2], r->w);
                gtk_adjustment_set_value(control.rect[3], r->h);
            }
            break;
        case ParamKind::File:
        case ParamKind::Text:
            if (const std::string *s = std::get_if<std::string>(&value);
                s && *s != gtk_editable_get_text(GTK_EDITABLE(control.widget)))
                gtk_editable_set_text(GTK_EDITABLE(control.widget), s->c_str());
            break;
        }
    }

    // --- Building ----------------------------------------------------------

    void build()
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 12);
        gtk_widget_set_margin_bottom(box, 12);

        GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        const char *scopes[] = {"Selected clip", "Its track", "Whole sequence", "Adjustment block", nullptr};
        m_scope = gtk_drop_down_new_from_strings(scopes);
        gtk_widget_set_hexpand(m_scope, TRUE);
        g_signal_connect(m_scope, "notify::selected", G_CALLBACK(&onScopeTrampoline), this);
        gtk_box_append(GTK_BOX(header), m_scope);

        m_add = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_add), "list-add-symbolic");
        gtk_menu_button_set_label(GTK_MENU_BUTTON(m_add), "Add");
        m_addPopover = gtk_popover_new();
        GtkWidget *addBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        m_search = gtk_search_entry_new();
        g_signal_connect(m_search, "search-changed", G_CALLBACK(&onSearchTrampoline), this);
        // Enter adds the best match.
        g_signal_connect(m_search, "activate", G_CALLBACK(&onSearchActivateTrampoline), this);
        gtk_box_append(GTK_BOX(addBox), m_search);
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
        gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 280);
        m_addList = gtk_list_box_new();
        g_signal_connect(m_addList, "row-activated", G_CALLBACK(&onAddRowTrampoline), this);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_addList);
        gtk_box_append(GTK_BOX(addBox), scroll);
        gtk_popover_set_child(GTK_POPOVER(m_addPopover), addBox);
        g_signal_connect(m_addPopover, "show", G_CALLBACK(&onAddShownTrampoline), this);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(m_add), m_addPopover);
        gtk_box_append(GTK_BOX(header), m_add);

        // Copy, paste, save as a look: the window's actions, so the menu
        // and the shortcuts are one thing.
        GMenu *menu = g_menu_new();
        g_menu_append(menu, "Copy effects", "win.effects-copy");
        g_menu_append(menu, "Paste after these", "win.effects-paste-append");
        g_menu_append(menu, "Paste instead of these", "win.effects-paste-replace");
        g_menu_append(menu, "Save as a look…", "win.effects-save-look");
        m_menu = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_menu), "view-more-symbolic");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(m_menu), G_MENU_MODEL(menu));
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_menu), GTK_ACCESSIBLE_PROPERTY_LABEL, "Effects menu", -1);
        g_object_unref(menu);
        gtk_box_append(GTK_BOX(header), m_menu);
        // Before and after over the picture (app/compare.h).
        GtkWidget *compare = gtk_button_new_from_icon_name("view-dual-symbolic");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(compare), "win.effects-compare");
        gtk_accessible_update_property(GTK_ACCESSIBLE(compare), GTK_ACCESSIBLE_PROPERTY_LABEL, "Compare", -1);
        m_compare = compare;
        gtk_box_append(GTK_BOX(header), compare);
        gtk_box_append(GTK_BOX(box), header);

        m_titleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        m_title = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(m_title), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(m_title), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(m_title, TRUE);
        gtk_widget_add_css_class(m_title, "heading");
        gtk_box_append(GTK_BOX(m_titleRow), m_title);
        gtk_box_append(GTK_BOX(box), m_titleRow);

        // An adjustment block's own settings: which tracks it affects (its
        // lane) and removing it. Shown in the block scope only.
        m_blockBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *affects = gtk_label_new("Affects");
        gtk_widget_add_css_class(affects, "dim-label");
        gtk_box_append(GTK_BOX(m_blockBar), affects);
        m_blockLane = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(nullptr)), nullptr);
        gtk_widget_set_hexpand(m_blockLane, TRUE);
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_blockLane), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Tracks the block affects", -1);
        g_signal_connect(m_blockLane, "notify::selected", G_CALLBACK(&onBlockLaneTrampoline), this);
        gtk_box_append(GTK_BOX(m_blockBar), m_blockLane);
        m_blockRemove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_blockRemove), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Remove adjustment block", -1);
        g_signal_connect(m_blockRemove, "clicked", G_CALLBACK(&onBlockRemoveTrampoline), this);
        gtk_box_append(GTK_BOX(m_blockBar), m_blockRemove);
        gtk_widget_set_visible(m_blockBar, FALSE);
        gtk_box_append(GTK_BOX(box), m_blockBar);

        // Ctrl+Shift+V: after or instead of the effects here (doc 15,
        // "Applying effects"). Parented to the title row, unparented when
        // the page goes.
        m_pastePopover = gtk_popover_new();
        GtkWidget *pasteBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *append = gtk_button_new_with_label("Paste after these");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(append), "win.effects-paste-append");
        gtk_box_append(GTK_BOX(pasteBox), append);
        GtkWidget *replace = gtk_button_new_with_label("Paste instead of these");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(replace), "win.effects-paste-replace");
        gtk_box_append(GTK_BOX(pasteBox), replace);
        gtk_popover_set_child(GTK_POPOVER(m_pastePopover), pasteBox);
        gtk_widget_set_parent(m_pastePopover, m_titleRow);

        m_lookPopover = gtk_popover_new();
        GtkWidget *lookBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        m_lookName = gtk_entry_new();
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_lookName), GTK_ACCESSIBLE_PROPERTY_LABEL, "Look name", -1);
        g_signal_connect(m_lookName, "activate", G_CALLBACK(&onLookSaveTrampoline), this);
        gtk_box_append(GTK_BOX(lookBox), m_lookName);
        GtkWidget *save = gtk_button_new_with_label("Save");
        gtk_accessible_update_property(GTK_ACCESSIBLE(save), GTK_ACCESSIBLE_PROPERTY_LABEL, "Save look", -1);
        gtk_widget_add_css_class(save, "suggested-action");
        g_signal_connect(save, "clicked", G_CALLBACK(&onLookSaveButtonTrampoline), this);
        gtk_box_append(GTK_BOX(lookBox), save);
        gtk_popover_set_child(GTK_POPOVER(m_lookPopover), lookBox);
        gtk_widget_set_parent(m_lookPopover, m_titleRow);

        m_empty = gtk_label_new("No effects yet. Add one to change how this looks or sounds.");
        gtk_label_set_wrap(GTK_LABEL(m_empty), TRUE);
        gtk_label_set_xalign(GTK_LABEL(m_empty), 0.0f);
        gtk_widget_add_css_class(m_empty, "dim-label");
        gtk_box_append(GTK_BOX(box), m_empty);

        m_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(box), m_cards);

        m_root = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(m_root), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_root), box);
        // A Browser tile dropped here goes on the Rack's clips.
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(&onDropTrampoline), this);
        gtk_widget_add_controller(m_root, GTK_EVENT_CONTROLLER(drop));
        g_signal_connect(m_root, "destroy", G_CALLBACK(&onDestroyTrampoline), this);
    }

    // Popovers parented by hand are unparented by hand.
    void onDestroy()
    {
        for (GtkWidget **popover : {&m_pastePopover, &m_lookPopover})
            if (*popover) {
                gtk_widget_unparent(*popover);
                *popover = nullptr;
            }
    }

    void fillAddList()
    {
        while (GtkWidget *child = gtk_widget_get_first_child(m_addList))
            gtk_list_box_remove(GTK_LIST_BOX(m_addList), child);
        if (!m_catalog.ready()) {
            GtkWidget *wait = gtk_label_new("Still finding the effects this install offers…");
            gtk_widget_add_css_class(wait, "dim-label");
            gtk_list_box_append(GTK_LIST_BOX(m_addList), wait);
            return;
        }
        const std::string query = lower(gtk_editable_get_text(GTK_EDITABLE(m_search)));
        // Best first: the name itself, then names starting with the query,
        // then any other match (name, category, service or tag).
        std::vector<std::pair<int, const EffectDescriptor *>> matches;
        for (const EffectDescriptor *d : m_catalog.offered()) {
            const std::string name = lower(d->name);
            std::string haystack = name + " " + lower(d->category + " " + d->service);
            for (const std::string &tag : d->tags)
                haystack += " " + lower(tag);
            int rank = 3;
            if (query.empty())
                rank = d->featured ? 0 : 1;
            else if (name == query)
                rank = 0;
            else if (name.starts_with(query))
                rank = 1;
            else if (haystack.find(query) != std::string::npos)
                rank = 2;
            if (rank < 3)
                matches.emplace_back(rank, d);
        }
        std::stable_sort(matches.begin(), matches.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        int shown = 0;
        for (const auto &[rank, d] : matches) {
            if (++shown > 300)
                break;
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *name = gtk_label_new(d->name.c_str());
            gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
            gtk_widget_set_hexpand(name, TRUE);
            gtk_box_append(GTK_BOX(line), name);
            GtkWidget *category = gtk_label_new(d->category.c_str());
            gtk_widget_add_css_class(category, "dim-label");
            gtk_widget_add_css_class(category, "caption");
            gtk_box_append(GTK_BOX(line), category);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), line);
            gtk_widget_set_tooltip_text(row, d->description.empty() ? nullptr : d->description.c_str());
            g_object_set_data_full(G_OBJECT(row), "service", g_strdup(d->service.c_str()), g_free);
            gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL, d->name.c_str(), -1);
            gtk_list_box_append(GTK_LIST_BOX(m_addList), row);
        }
        if (shown == 0) {
            GtkWidget *none = gtk_label_new("No effect matches");
            gtk_widget_add_css_class(none, "dim-label");
            gtk_list_box_append(GTK_LIST_BOX(m_addList), none);
        }
    }

    // What the cards show, minus values: rebuilt when this changes, else
    // only the values move (so a drag keeps its widget).
    std::string structureOf(const core::Model::EffectTarget &target) const
    {
        std::string key = std::to_string(static_cast<int>(target.kind)) + ":" + std::to_string(target.id);
        if (transformClip(target))
            key += "T"; // the Transform card

        // Several clips: what they share is part of what the cards show.
        if (multiple())
            for (core::ClipId clip : selectedClips()) {
                key += "/" + std::to_string(clip.value);
                for (const core::Effect &e : m_host.model().clip(clip).effects)
                    key += "." + e.service + (e.enabled ? "+" : "-");
            }
        for (const core::Effect &e : m_host.model().effects(target)) {
            // A mask's shape and invert are rows and a drop-down; its
            // geometry and soft edge aren't (they change during a drag).
            key += "|" + std::to_string(e.id.value) + e.service + (e.enabled ? "+" : "-") +
                   (e.mix.keyframes.empty() ? "" : "k") +
                   (e.mask ? "m" + e.mask->shape + (e.mask->invert ? "i" : "") : "");
            for (const core::Param &p : e.params)
                key += "," + p.name + (p.keyframes.empty() ? "" : "k");
        }
        return key;
    }

    void refresh()
    {
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (!target || !m_host.model().hasEffectTarget(*target))
            return;
        gtk_label_set_text(GTK_LABEL(m_title), targetTitle(*target).c_str());
        showBlockBar(*target);
        const std::string structure = structureOf(*target);
        if (structure == m_structure) {
            updateValues();
            return;
        }
        // From an idle: this may run inside a card's own signal handler
        // (remove, bypass), which must not destroy its widget under itself.
        if (!m_rebuildPending) {
            m_rebuildPending = true;
            g_idle_add(&onRebuildTrampoline, this);
        }
    }

    // --- Adjustment blocks ----------------------------------------------

    // The block bar: "Every track", then "<track> and below" for each row a
    // block can sit above (lane k = above row k).
    void showBlockBar(const core::Model::EffectTarget &target)
    {
        const bool block = target.kind == core::Model::EffectTarget::Kind::AdjustmentBlock;
        gtk_widget_set_visible(m_blockBar, block);
        if (!block)
            return;
        const core::Model &model = m_host.model();
        std::vector<std::string> names{"Every track"};
        const auto &tracks = model.sequence().tracks;
        for (size_t row = 1; row < tracks.size(); ++row)
            names.push_back(tracks[row].name + " and below");
        std::vector<const char *> strings;
        for (const std::string &name : names)
            strings.push_back(name.c_str());
        strings.push_back(nullptr);
        m_updating = true;
        gtk_drop_down_set_model(GTK_DROP_DOWN(m_blockLane), G_LIST_MODEL(gtk_string_list_new(strings.data())));
        const int lane = model.adjustmentBlock(core::AdjustmentBlockId{target.id}).lane;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(m_blockLane),
                                   static_cast<guint>(std::clamp<int>(lane, 0, static_cast<int>(names.size()) - 1)));
        m_updating = false;
    }

    void onBlockLane()
    {
        if (m_updating || !m_catalog.selectedBlock || !m_host.model().hasAdjustmentBlock(*m_catalog.selectedBlock))
            return;
        const core::AdjustmentBlock &block = m_host.model().adjustmentBlock(*m_catalog.selectedBlock);
        const int lane = static_cast<int>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_blockLane)));
        if (lane == block.lane)
            return;
        if (!m_host.execute(std::make_unique<SetAdjustmentBlockRange>(block.id, lane, block.start, block.length)))
            m_host.showStatus("Another block is already there on that lane.");
    }

    void onBlockRemove()
    {
        if (!m_catalog.selectedBlock || !m_host.model().hasAdjustmentBlock(*m_catalog.selectedBlock))
            return;
        if (m_host.execute(std::make_unique<RemoveAdjustmentBlock>(*m_catalog.selectedBlock))) {
            m_catalog.selectedBlock.reset();
            m_catalog.blockSelected.emit();
        }
    }

    void onBlockSelected()
    {
        const auto scope = static_cast<Scope>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_scope)));
        if (m_catalog.selectedBlock)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(m_scope), static_cast<guint>(Scope::Block));
        else if (scope == Scope::Block)
            gtk_drop_down_set_selected(GTK_DROP_DOWN(m_scope), static_cast<guint>(Scope::Clip));
        m_structure.clear();
        refresh();
    }

    void rebuildNow()
    {
        m_rebuildPending = false;
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (!target || !m_host.model().hasEffectTarget(*target))
            return;
        m_structure = structureOf(*target);
        rebuildCards(*target);
    }

    // --- Masks ------------------------------------------------------------

    // "Mask": none, rectangle or ellipse, the handles on the picture and
    // invert; "Soft edge" below when there is one. Returns the next row.
    int addMaskRows(GtkWidget *grid, int row, const core::Effect &effect)
    {
        auto mc = std::make_unique<MaskControl>();
        mc->rack = this;
        mc->effect = effect.id;
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        const char *shapes[] = {"None", "Rectangle", "Ellipse", nullptr};
        mc->shape = gtk_drop_down_new_from_strings(shapes);
        gtk_accessible_update_property(GTK_ACCESSIBLE(mc->shape), GTK_ACCESSIBLE_PROPERTY_LABEL, "Mask", -1);
        m_host.setTooltip(mc->shape, "effects.mask");
        const guint selected = !effect.mask ? 0 : effect.mask->shape == "ellipse" ? 2 : 1;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(mc->shape), selected);
        g_signal_connect(mc->shape, "notify::selected", G_CALLBACK(&onMaskShapeTrampoline), mc.get());
        gtk_widget_set_hexpand(mc->shape, TRUE);
        gtk_box_append(GTK_BOX(box), mc->shape);
        if (effect.mask) {
            mc->handles = gtk_toggle_button_new();
            gtk_button_set_icon_name(GTK_BUTTON(mc->handles), "edit-select-symbolic");
            gtk_widget_add_css_class(mc->handles, "flat");
            gtk_accessible_update_property(GTK_ACCESSIBLE(mc->handles), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           "Edit the mask on the picture", -1);
            m_host.setTooltip(mc->handles, "effects.rect-handles");
            g_signal_connect(mc->handles, "toggled", G_CALLBACK(&onMaskHandlesTrampoline), mc.get());
            gtk_box_append(GTK_BOX(box), mc->handles);
            mc->invert = gtk_toggle_button_new_with_label("Invert");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mc->invert), effect.mask->invert);
            m_host.setTooltip(mc->invert, "effects.mask-invert");
            g_signal_connect(mc->invert, "toggled", G_CALLBACK(&onMaskInvertTrampoline), mc.get());
            gtk_box_append(GTK_BOX(box), mc->invert);
        }
        addRow(grid, row++, "Mask", box);
        if (effect.mask) {
            mc->featherAdjustment = gtk_adjustment_new(effect.mask->feather.value, 0.0, 0.5, 0.01, 0.05, 0.0);
            mc->feather = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, mc->featherAdjustment);
            gtk_scale_set_draw_value(GTK_SCALE(mc->feather), FALSE);
            gtk_accessible_update_property(GTK_ACCESSIBLE(mc->feather), GTK_ACCESSIBLE_PROPERTY_LABEL, "Soft edge", -1);
            m_host.setTooltip(mc->feather, "effects.mask-feather");
            g_signal_connect(mc->featherAdjustment, "value-changed", G_CALLBACK(&onMaskFeatherTrampoline), mc.get());
            addRow(grid, row++, "Soft edge", mc->feather);
        }
        m_masks.push_back(std::move(mc));
        return row;
    }

    std::optional<core::EffectMask> maskOf(core::EffectId id) const
    {
        return m_host.model().hasEffect(id) ? m_host.model().effect(id).mask : std::nullopt;
    }

    // A new gesture after a rest, as the controls' (onControlChanged()).
    uint64_t maskGesture(MaskControl &mc)
    {
        const gint64 now = g_get_monotonic_time();
        if (mc.gesture == 0 || now - mc.lastChange > kGestureGapUs)
            mc.gesture = ++m_nextGesture;
        mc.lastChange = now;
        return mc.gesture;
    }

    static double geometry(const core::EffectMask &mask, const char *name, double fallback)
    {
        for (const core::Param &p : mask.params)
            if (p.name == name)
                if (const double *v = std::get_if<double>(&p.value))
                    return *v;
        return fallback;
    }

    static void setGeometry(core::EffectMask &mask, const char *name, double value)
    {
        for (core::Param &p : mask.params)
            if (p.name == name) {
                p.value = value;
                p.keyframes.clear();
                return;
            }
        mask.params.push_back({name, value, {}});
    }

    void onMaskShape(MaskControl &mc)
    {
        if (m_updating)
            return;
        const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(mc.shape));
        std::optional<core::EffectMask> mask = maskOf(mc.effect);
        if (selected == 0) {
            mask.reset();
        } else {
            if (!mask) {
                // A new mask: the middle half of the picture, a little soft.
                mask = core::EffectMask{};
                mask->params = {{"x", 0.5, {}}, {"y", 0.5, {}}, {"width", 0.5, {}}, {"height", 0.5, {}}};
                mask->feather = {0.05, {}};
            }
            mask->shape = selected == 2 ? "ellipse" : "rectangle";
        }
        m_host.execute(std::make_unique<SetEffectMask>(mc.effect, mask));
    }

    void onMaskInvert(MaskControl &mc)
    {
        std::optional<core::EffectMask> mask = maskOf(mc.effect);
        if (m_updating || !mask)
            return;
        mask->invert = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mc.invert));
        m_host.execute(std::make_unique<SetEffectMask>(mc.effect, mask));
    }

    void onMaskFeather(MaskControl &mc)
    {
        std::optional<core::EffectMask> mask = maskOf(mc.effect);
        if (m_updating || !mask)
            return;
        mask->feather = {gtk_adjustment_get_value(mc.featherAdjustment), {}};
        m_host.execute(std::make_unique<SetEffectMask>(mc.effect, mask, maskGesture(mc)));
    }

    // The mask's rectangle on the picture, in frame pixels both ways.
    void onMaskHandles(MaskControl &mc, bool on)
    {
        PreviewTools &tools = previewTools(m_host);
        const ControlKey key{mc.effect.value, "mask"};
        if (!on) {
            if (m_rectKey == key)
                tools.hideRect();
            m_rectKey.reset();
            return;
        }
        const std::optional<core::EffectMask> mask = maskOf(mc.effect);
        if (!mask)
            return;
        m_rectKey = key;
        const core::Profile &profile = m_host.model().sequence().profile;
        const double fw = profile.width, fh = profile.height;
        const double w = geometry(*mask, "width", 0.5), h = geometry(*mask, "height", 0.5);
        const core::Rect rect{(geometry(*mask, "x", 0.5) - w / 2) * fw, (geometry(*mask, "y", 0.5) - h / 2) * fh,
                              w * fw, h * fh};
        const core::EffectId effect = mc.effect;
        tools.showRect(rect, [this, effect, fw, fh](core::Rect r, uint64_t gesture) {
            std::optional<core::EffectMask> now = maskOf(effect);
            if (!now)
                return;
            setGeometry(*now, "x", (r.x + r.w / 2) / fw);
            setGeometry(*now, "y", (r.y + r.h / 2) / fh);
            setGeometry(*now, "width", r.w / fw);
            setGeometry(*now, "height", r.h / fh);
            // One undo step a drag; the gesture ids stay apart from the
            // controls' (the tools count their own from 1).
            m_host.execute(std::make_unique<SetEffectMask>(effect, now, (uint64_t{1} << 40) + gesture));
        });
    }

    // --- Files --------------------------------------------------------------

    void onChooseFile(Control &control)
    {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Choose a file");
        if (!control.extensions.empty()) {
            GtkFileFilter *filter = gtk_file_filter_new();
            std::string name;
            for (const std::string &extension : control.extensions) {
                gtk_file_filter_add_suffix(filter, extension.c_str());
                name += (name.empty() ? "" : ", ") + ("." + extension);
            }
            gtk_file_filter_set_name(filter, name.c_str());
            GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
            g_list_store_append(filters, filter);
            gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
            g_object_unref(filters);
            g_object_unref(filter);
        }
        m_chooseKey = keyOf(control);
        GtkRoot *root = gtk_widget_get_root(control.widget);
        gtk_file_dialog_open(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr, nullptr,
                             &onFileChosenTrampoline, this);
        g_object_unref(dialog);
    }

    void onFileChosen(GFile *file)
    {
        if (!file || !m_chooseKey)
            return;
        char *path = g_file_get_path(file);
        if (Control *control = findControl(*m_chooseKey); control && path)
            gtk_editable_set_text(GTK_EDITABLE(control->widget), path); // as typing it: one edit
        g_free(path);
    }

    // --- On the picture (app/preview_tools.h) ------------------------------

    Control *findControl(const ControlKey &key)
    {
        for (const std::unique_ptr<Control> &control : m_controls)
            if (keyOf(*control) == key)
                return control.get();
        return nullptr;
    }

    void onEyedropper(Control &control)
    {
        const ControlKey key = keyOf(control);
        previewTools(m_host).pickColour([this, key](core::Color colour) {
            // The Rack may have been rebuilt since: find the control again,
            // and set it as a hand would (keys, several clips, undo).
            if (Control *now = findControl(key)) {
                const GdkRGBA rgba{colour.r / 255.0f, colour.g / 255.0f, colour.b / 255.0f, 1.0f};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(now->widget), &rgba);
            }
        });
    }

    void onRectHandles(Control &control, bool on)
    {
        PreviewTools &tools = previewTools(m_host);
        if (!on) {
            if (m_rectKey == keyOf(control))
                tools.hideRect();
            m_rectKey.reset();
            return;
        }
        m_rectKey = keyOf(control);
        const ControlKey key = keyOf(control);
        const core::Rect *rect = nullptr;
        const core::Param::Value value = readControl(control);
        rect = std::get_if<core::Rect>(&value);
        tools.showRect(rect ? *rect : core::Rect{}, [this, key](core::Rect r, uint64_t) {
            if (Control *now = findControl(key))
                writeControlAsHand(*now, r);
        });
    }

    // Sets a rect control's numbers as typing them would (one undo step for
    // a drag: the changes come closer than the gesture gap).
    void writeControlAsHand(Control &control, const core::Rect &r)
    {
        const double values[4] = {r.x, r.y, r.w, r.h};
        for (size_t i = 0; i < 4 && i < control.rect.size(); ++i)
            gtk_adjustment_set_value(control.rect[i], values[i]);
    }

    void rebuildCards(const core::Model::EffectTarget &target)
    {
        // The handles belonged to a control about to go.
        if (m_rectKey) {
            previewTools(m_host).hideRect();
            m_rectKey.reset();
        }
        // Focus on a card that goes (a mask's drop-down, say) would pass to
        // the next focusable widget: the Transform card's X field, a text
        // entry, which turns the single-key shortcuts (E, P, T, C) off. The
        // scope drop-down takes it instead.
        GtkRoot *root = gtk_widget_get_root(m_cards);
        GtkWidget *focus = root ? gtk_root_get_focus(root) : nullptr;
        const bool focusGoes = focus && gtk_widget_is_ancestor(focus, m_cards);
        if (focusGoes)
            gtk_widget_grab_focus(m_scope);
        while (GtkWidget *child = gtk_widget_get_first_child(m_cards))
            gtk_box_remove(GTK_BOX(m_cards), child);
        m_controls.clear();
        m_transformNote = nullptr;
        m_actions.clear();
        m_masks.clear();
        // Several clips: only this drop-in's effects they all share, each
        // with its twins on the other clips.
        std::vector<std::pair<const core::Effect *, std::vector<core::EffectId>>> shown;
        for (const core::Effect &effect : m_host.model().effects(target)) {
            if (!multiple()) {
                shown.emplace_back(&effect, std::vector<core::EffectId>{});
                continue;
            }
            if (effect.owner != kOwner)
                continue;
            if (std::optional<std::vector<core::EffectId>> twins = twinsOf(effect.id))
                shown.emplace_back(&effect, std::move(*twins));
        }
        gtk_widget_set_visible(m_empty, shown.empty());
        gtk_label_set_text(GTK_LABEL(m_empty), multiple()
                                                   ? "These clips share no effects. Add one to add it to all of them."
                                                   : "No effects yet. Add one to change how this looks or sounds.");
        m_updating = true;
        if (std::optional<core::ClipId> clip = transformClip(target))
            gtk_box_append(GTK_BOX(m_cards), buildTransformCard(*clip));
        for (size_t i = 0; i < shown.size(); ++i)
            gtk_box_append(GTK_BOX(m_cards), buildCard(*shown[i].first, shown[i].second, i, shown.size()));
        m_updating = false;
        updateValues(); // animated values at the playhead, key states
    }

    GtkWidget *iconButton(const char *icon, const char *hint, core::EffectId effect,
                          const std::vector<core::EffectId> &twins, int move, bool sensitive)
    {
        GtkWidget *button = gtk_button_new_from_icon_name(icon);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_sensitive(button, sensitive);
        m_host.setTooltip(button, hint);
        m_actions.push_back(std::make_unique<CardAction>(CardAction{this, effect, twins, move}));
        g_signal_connect(button, "clicked", G_CALLBACK(&onCardActionTrampoline), m_actions.back().get());
        return button;
    }

    GtkWidget *buildCard(const core::Effect &effect, const std::vector<core::EffectId> &twins, size_t index,
                         size_t count)
    {
        const EffectDescriptor *descriptor = m_catalog.find(effect.service);
        const bool ours = effect.owner == kOwner;

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(card, "card");
        GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(inner, 10);
        gtk_widget_set_margin_end(inner, 10);
        gtk_widget_set_margin_top(inner, 8);
        gtk_widget_set_margin_bottom(inner, 8);
        gtk_box_append(GTK_BOX(card), inner);

        GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *bypass = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(bypass), effect.enabled);
        gtk_widget_set_valign(bypass, GTK_ALIGN_CENTER);
        gtk_widget_set_sensitive(bypass, ours);
        m_host.setTooltip(bypass, "effects.card-bypass");
        g_object_set_data(G_OBJECT(bypass), "rack", this);
        g_signal_connect(bypass, "notify::active", G_CALLBACK(&onBypassTrampoline),
                         GSIZE_TO_POINTER(static_cast<gsize>(effect.id.value)));
        gtk_box_append(GTK_BOX(header), bypass);

        const std::string name = !effect.displayName.empty() ? effect.displayName
                                 : descriptor                ? descriptor->name
                                                             : effect.service;
        GtkWidget *label = gtk_label_new(name.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "heading");
        if (!effect.enabled)
            gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(header), label);

        if (std::optional<HealthRecord> health = m_catalog.health(effect.service); health && health->usable()) {
            const CostBadge badge = costBadge(health->msPerFrame);
            GtkWidget *cost = gtk_label_new(costLabel(badge));
            gtk_widget_add_css_class(cost, "caption");
            gtk_widget_add_css_class(cost, costClass(badge));
            m_host.setTooltip(cost, "effects.card-cost");
            gtk_box_append(GTK_BOX(header), cost);
        }
        // Reordering several clips' stacks at once isn't offered (their other
        // effects may differ); one clip's is.
        const bool single = twins.empty();
        gtk_box_append(GTK_BOX(header),
                       iconButton("go-up-symbolic", "effects.card-up", effect.id, twins, -1, single && index > 0));
        gtk_box_append(GTK_BOX(header), iconButton("go-down-symbolic", "effects.card-down", effect.id, twins, +1,
                                                   single && index + 1 < count));
        gtk_box_append(GTK_BOX(header),
                       iconButton("user-trash-symbolic", "effects.card-remove", effect.id, twins, 0, true));
        gtk_box_append(GTK_BOX(inner), header);

        // Why it may not play (the extension skips these; doc 15, "Gating").
        std::string note;
        if (!ours)
            note = "Added by the " + effect.owner + " drop-in; its settings are its own.";
        else if (!descriptor && m_catalog.ready())
            note = "Not available on this computer: it plays without this effect.";
        else if (descriptor && !m_catalog.usable(effect.service))
            note = "Turned off: it failed the stability check on this computer.";
        if (!note.empty()) {
            GtkWidget *warning = gtk_label_new(note.c_str());
            gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
            gtk_label_set_xalign(GTK_LABEL(warning), 0.0f);
            gtk_widget_add_css_class(warning, "warning");
            gtk_box_append(GTK_BOX(inner), warning);
        }
        if (!ours)
            return card;

        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;
        // The mix: video effects only (MLT's mask pair composites pictures).
        if (!descriptor || descriptor->media == MediaKind::Video) {
            auto control = std::make_unique<Control>();
            control->rack = this;
            control->effect = effect.id;
            control->adjustment = gtk_adjustment_new(effect.mix.value * 100.0, 0.0, 100.0, 1.0, 10.0, 0.0);
            GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, control->adjustment);
            gtk_scale_set_digits(GTK_SCALE(scale), 0);
            gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
            gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
            gtk_widget_set_hexpand(scale, TRUE);
            m_host.setTooltip(scale, "effects.card-mix");
            control->widget = scale;
            control->twins = twins;
            // Keyframes are one clip's (they count from its start).
            control->animatable = twins.empty();
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            addRow(grid, row, "Mix", scale);
            if (control->animatable) {
                addKeyControls(grid, row, *control);
                row += 2;
            } else {
                ++row;
            }
            m_controls.push_back(std::move(control));
            // A mask: one clip's (its geometry is drawn on its picture).
            if (twins.empty())
                row = addMaskRows(grid, row, effect);
        }
        if (descriptor)
            for (const ParamDescriptor &p : descriptor->params) {
                if (p.hidden)
                    continue;
                const core::Param current = currentParam(effect, p.id, p.kind);
                GtkWidget *widget = buildControl(effect.id, p, current);
                if (!widget)
                    continue;
                m_controls.back()->twins = twins;
                if (!twins.empty())
                    m_controls.back()->animatable = false;
                addRow(grid, row, p.title, widget, p.description);
                if (m_controls.back()->animatable) {
                    addKeyControls(grid, row, *m_controls.back());
                    row += 2;
                } else {
                    ++row;
                }
            }
        gtk_box_append(GTK_BOX(inner), grid);
        return card;
    }

    void addRow(GtkWidget *grid, int row, const std::string &title, GtkWidget *widget,
                const std::string &description = {})
    {
        GtkWidget *label = gtk_label_new(title.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 8); // "Rotation", "Distance" whole
        gtk_label_set_max_width_chars(GTK_LABEL(label), 12);
        if (!description.empty())
            gtk_widget_set_tooltip_text(label, description.c_str());
        gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
        gtk_widget_set_hexpand(widget, TRUE);
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
    }

    // Previous key, pin, next key beside the value; the feel of the key at
    // the playhead on the row below (shown only there).
    void addKeyControls(GtkWidget *grid, int row, Control &control)
    {
        GtkWidget *keys = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(keys, "linked");
        auto button = [&](const char *icon, const char *hint, GCallback callback) {
            GtkWidget *b = gtk_button_new_from_icon_name(icon);
            gtk_widget_add_css_class(b, "flat");
            m_host.setTooltip(b, hint);
            g_signal_connect(b, "clicked", callback, &control);
            gtk_box_append(GTK_BOX(keys), b);
            return b;
        };
        control.previous = button("go-previous-symbolic", "effects.key-previous", G_CALLBACK(&onKeyPreviousTrampoline));
        control.pin = button("non-starred-symbolic", "effects.key-pin", G_CALLBACK(&onKeyPinTrampoline));
        control.next = button("go-next-symbolic", "effects.key-next", G_CALLBACK(&onKeyNextTrampoline));
        if (control.adjustment) {
            control.arm = gtk_toggle_button_new();
            gtk_button_set_icon_name(GTK_BUTTON(control.arm), "media-record-symbolic");
            gtk_widget_add_css_class(control.arm, "flat");
            gtk_accessible_update_property(GTK_ACCESSIBLE(control.arm), GTK_ACCESSIBLE_PROPERTY_LABEL, "Touch-record",
                                           -1);
            m_host.setTooltip(control.arm, "effects.key-arm");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control.arm), isArmed(control));
            showArmed(control, isArmed(control));
            g_signal_connect(control.arm, "toggled", G_CALLBACK(&onArmTrampoline), &control);
            gtk_box_append(GTK_BOX(keys), control.arm);
        }
        gtk_grid_attach(GTK_GRID(grid), keys, 2, row, 1, 1);

        std::vector<std::string> names;
        for (const Feel &feel : feels()) {
            names.push_back(feel.name);
            control.feelEasings.push_back(feel.easing);
        }
        for (int e = 0; e <= static_cast<int>(core::Easing::BounceInOut); ++e) {
            const auto easing = static_cast<core::Easing>(e);
            if (std::find(control.feelEasings.begin(), control.feelEasings.end(), easing) != control.feelEasings.end())
                continue;
            names.push_back(std::string("Curve: ") + core::easingName(easing));
            control.feelEasings.push_back(easing);
        }
        std::vector<const char *> strings;
        for (const std::string &n : names)
            strings.push_back(n.c_str());
        strings.push_back(nullptr);
        control.feel = gtk_drop_down_new_from_strings(strings.data());
        m_host.setTooltip(control.feel, "effects.key-feel");
        g_signal_connect(control.feel, "notify::selected", G_CALLBACK(&onKeyFeelTrampoline), &control);
        GtkWidget *feelLabel = gtk_label_new("Feel");
        gtk_label_set_xalign(GTK_LABEL(feelLabel), 1.0f);
        gtk_widget_add_css_class(feelLabel, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), feelLabel, 0, row + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), control.feel, 1, row + 1, 2, 1);
        g_object_set_data(G_OBJECT(control.feel), "label", feelLabel);
        showKeyState(control);
    }

    GtkWidget *buildControl(core::EffectId effect, const ParamDescriptor &p, const core::Param &current)
    {
        auto control = std::make_unique<Control>();
        control->rack = this;
        control->effect = effect;
        control->param = p.id;
        control->kind = p.kind;
        control->display = p.display;
        GtkWidget *widget = nullptr;
        switch (p.kind) {
        case ParamKind::Scalar: {
            // Without a maximum in the metadata (lift_gamma_gain's gains),
            // room for twice the default and twice what it's set to, so a
            // look's 1.08 isn't clamped to 1.
            double lo = p.minimum.value_or(0.0);
            double hi = p.maximum.value_or(
                std::max({1.0, lo + 1.0, 2.0 * asNumber(p.defaultValue), 2.0 * asNumber(current.value)}));
            if (p.display) {
                lo = p.display->toMin;
                hi = p.display->toMax;
            }
            const double span = hi - lo;
            control->adjustment = gtk_adjustment_new(lo, lo, hi, span / 100.0, span / 10.0, 0.0);
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, control->adjustment);
            gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
            gtk_widget_set_hexpand(scale, TRUE);
            gtk_box_append(GTK_BOX(box), scale);
            GtkWidget *spin = gtk_spin_button_new(control->adjustment, span / 100.0, span <= 10.0 ? 3 : 1);
            gtk_editable_set_width_chars(GTK_EDITABLE(spin), 6);
            gtk_box_append(GTK_BOX(box), spin);
            if (p.display && !p.display->unit.empty()) {
                GtkWidget *unit = gtk_label_new(p.display->unit.c_str());
                gtk_widget_add_css_class(unit, "dim-label");
                gtk_box_append(GTK_BOX(box), unit);
            }
            widget = box;
            control->animatable = p.animatable;
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            break;
        }
        case ParamKind::Integer: {
            const double lo = p.minimum.value_or(-1'000'000.0), hi = p.maximum.value_or(1'000'000.0);
            control->adjustment = gtk_adjustment_new(lo, lo, hi, 1.0, 10.0, 0.0);
            widget = gtk_spin_button_new(control->adjustment, 1.0, 0);
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            break;
        }
        case ParamKind::Toggle:
            widget = gtk_switch_new();
            gtk_widget_set_halign(widget, GTK_ALIGN_START);
            g_signal_connect(widget, "notify::active", G_CALLBACK(&onControlNotifyTrampoline), control.get());
            break;
        case ParamKind::Color: {
            control->widget = gtk_color_dialog_button_new(gtk_color_dialog_new());
            g_signal_connect(control->widget, "notify::rgba", G_CALLBACK(&onControlNotifyTrampoline), control.get());
            // The eyedropper: a colour from the picture (app/preview_tools.h).
            GtkWidget *pick = gtk_button_new_from_icon_name("color-select-symbolic");
            gtk_widget_add_css_class(pick, "flat");
            gtk_accessible_update_property(GTK_ACCESSIBLE(pick), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           "Pick a colour from the picture", -1);
            m_host.setTooltip(pick, "effects.eyedropper");
            g_signal_connect(pick, "clicked", G_CALLBACK(&onEyedropperTrampoline), control.get());
            widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            gtk_widget_set_halign(widget, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(widget), control->widget);
            gtk_box_append(GTK_BOX(widget), pick);
            break;
        }
        case ParamKind::Choice: {
            control->choices = p.choices;
            std::vector<const char *> strings;
            for (const std::string &c : p.choices)
                strings.push_back(c.c_str());
            strings.push_back(nullptr);
            widget = gtk_drop_down_new_from_strings(strings.data());
            g_signal_connect(widget, "notify::selected", G_CALLBACK(&onControlNotifyTrampoline), control.get());
            break;
        }
        case ParamKind::Rect: {
            widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
            for (int i = 0; i < 4; ++i) {
                GtkAdjustment *adjustment = gtk_adjustment_new(0.0, -100'000.0, 100'000.0, 1.0, 10.0, 0.0);
                control->rect.push_back(adjustment);
                GtkWidget *spin = gtk_spin_button_new(adjustment, 1.0, 0);
                gtk_editable_set_width_chars(GTK_EDITABLE(spin), 5);
                gtk_box_append(GTK_BOX(widget), spin);
                g_signal_connect(adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            }
            // Handles over the picture (app/preview_tools.h).
            GtkWidget *handles = gtk_toggle_button_new();
            gtk_button_set_icon_name(GTK_BUTTON(handles), "edit-select-symbolic");
            gtk_widget_add_css_class(handles, "flat");
            gtk_accessible_update_property(GTK_ACCESSIBLE(handles), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           "Edit on the picture", -1);
            m_host.setTooltip(handles, "effects.rect-handles");
            g_signal_connect(handles, "toggled", G_CALLBACK(&onRectHandlesTrampoline), control.get());
            gtk_box_append(GTK_BOX(widget), handles);
            break;
        }
        case ParamKind::File: {
            // The path, and a chooser for it (a LUT's .cube, a map).
            control->widget = gtk_entry_new();
            gtk_widget_set_hexpand(control->widget, TRUE);
            g_signal_connect(control->widget, "changed", G_CALLBACK(&onControlTrampoline), control.get());
            control->extensions = p.extensions;
            GtkWidget *choose = gtk_button_new_from_icon_name("document-open-symbolic");
            gtk_widget_add_css_class(choose, "flat");
            gtk_accessible_update_property(GTK_ACCESSIBLE(choose), GTK_ACCESSIBLE_PROPERTY_LABEL, "Choose a file",
                                           -1);
            m_host.setTooltip(choose, "effects.choose-file");
            g_signal_connect(choose, "clicked", G_CALLBACK(&onChooseFileTrampoline), control.get());
            widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            gtk_box_append(GTK_BOX(widget), control->widget);
            gtk_box_append(GTK_BOX(widget), choose);
            break;
        }
        case ParamKind::Text:
            widget = gtk_entry_new();
            gtk_widget_set_hexpand(widget, TRUE);
            g_signal_connect(widget, "changed", G_CALLBACK(&onControlTrampoline), control.get());
            break;
        }
        control->widget = control->widget ? control->widget : widget;
        writeControl(*control, current.value);
        m_controls.push_back(std::move(control));
        return widget;
    }

    void updateValues()
    {
        const core::Model &model = m_host.model();
        m_updating = true;
        if (m_transformNote)
            showTransformNote();
        for (const std::unique_ptr<Control> &control : m_controls) {
            if (control->transform) {
                if (!model.hasClip(control->clip) || isRecording(*control))
                    continue;
                // At the playhead, as the picture shows (a Fit or Stretch
                // picture: the placement a first change would make).
                const core::Transform shown = core::transformAt(placedTransform(control->clip), frameOf(*control));
                gtk_adjustment_set_value(control->adjustment, fieldOf(shown, *control->transform).value);
                showKeyState(*control);
                continue;
            }
            if (!model.hasEffect(control->effect) || isRecording(*control))
                continue;
            const core::Effect &effect = model.effect(control->effect);
            const std::vector<core::Keyframe> keys = keysOf(*control);
            if (!keys.empty()) {
                // Animated: the value at the playhead.
                const double value = core::easedValue(keys, static_cast<double>(frameOf(*control)));
                if (control->param.empty())
                    gtk_adjustment_set_value(control->adjustment, value * 100.0);
                else
                    writeControl(*control, value);
            } else if (control->param.empty()) {
                gtk_adjustment_set_value(control->adjustment, effect.mix.value * 100.0);
            } else {
                writeControl(*control, currentParam(effect, control->param, control->kind).value);
            }
            showKeyState(*control);
        }
        m_updating = false;
    }

    // Only animated values and key states move with the playhead.
    void onPlayheadMoved()
    {
        if (m_rebuildPending)
            return;
        updateValues();
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onSearchActivateTrampoline(GtkSearchEntry *, gpointer self)
    {
        static_cast<Rack *>(self)->onSearchActivate();
    }
    static void onCopyTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->onCopy();
    }
    static void onPasteTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->onPaste();
    }
    static void onPasteAppendTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->paste(PasteMode::Append);
    }
    static void onPasteReplaceTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->paste(PasteMode::Replace);
    }
    static void onSaveLookTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->onSaveLook();
    }
    static void onLookSaveTrampoline(GtkEntry *, gpointer self)
    {
        static_cast<Rack *>(self)->onLookSave();
    }
    static void onLookSaveButtonTrampoline(GtkButton *, gpointer self)
    {
        static_cast<Rack *>(self)->onLookSave();
    }
    static gboolean onDropTrampoline(GtkDropTarget *, const GValue *value, double, double, gpointer self)
    {
        const char *payload = G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : nullptr;
        return payload && static_cast<Rack *>(self)->onDrop(payload);
    }
    static void onDestroyTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Rack *>(self)->onDestroy();
    }
    static void onPinActionTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->onPinAction();
    }
    static void onMaskShapeTrampoline(GtkDropDown *, GParamSpec *, gpointer mc)
    {
        auto *m = static_cast<MaskControl *>(mc);
        m->rack->onMaskShape(*m);
    }
    static void onMaskInvertTrampoline(GtkToggleButton *, gpointer mc)
    {
        auto *m = static_cast<MaskControl *>(mc);
        m->rack->onMaskInvert(*m);
    }
    static void onMaskFeatherTrampoline(GtkAdjustment *, gpointer mc)
    {
        auto *m = static_cast<MaskControl *>(mc);
        m->rack->onMaskFeather(*m);
    }
    static void onMaskHandlesTrampoline(GtkToggleButton *button, gpointer mc)
    {
        auto *m = static_cast<MaskControl *>(mc);
        m->rack->onMaskHandles(*m, gtk_toggle_button_get_active(button));
    }
    static void onChooseFileTrampoline(GtkButton *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onChooseFile(*c);
    }
    static void onFileChosenTrampoline(GObject *dialog, GAsyncResult *result, gpointer self)
    {
        GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, nullptr);
        static_cast<Rack *>(self)->onFileChosen(file);
        if (file)
            g_object_unref(file);
    }
    static void onEyedropperTrampoline(GtkButton *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onEyedropper(*c);
    }
    static void onRectHandlesTrampoline(GtkToggleButton *button, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onRectHandles(*c, gtk_toggle_button_get_active(button));
    }
    static void onArmTrampoline(GtkToggleButton *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onArmToggled(*c);
    }
    static gboolean onRecordingIdleTrampoline(gpointer self)
    {
        auto *rack = static_cast<Rack *>(self);
        rack->m_recording->timer = 0; // this source ends here
        rack->finishRecording();
        return G_SOURCE_REMOVE;
    }
    static void onBlockLaneTrampoline(GtkDropDown *, GParamSpec *, gpointer self)
    {
        static_cast<Rack *>(self)->onBlockLane();
    }
    static void onBlockRemoveTrampoline(GtkButton *, gpointer self)
    {
        static_cast<Rack *>(self)->onBlockRemove();
    }
    static void onKeyPinTrampoline(GtkButton *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onKeyPin(*c);
    }
    static void onKeyPreviousTrampoline(GtkButton *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onKeyStep(*c, false);
    }
    static void onKeyNextTrampoline(GtkButton *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onKeyStep(*c, true);
    }
    static void onKeyFeelTrampoline(GObject *, GParamSpec *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onKeyFeel(*c);
    }
    static gboolean onRebuildTrampoline(gpointer self)
    {
        static_cast<Rack *>(self)->rebuildNow();
        return G_SOURCE_REMOVE;
    }
    static void onScopeTrampoline(GObject *, GParamSpec *, gpointer self)
    {
        static_cast<Rack *>(self)->onScopeChanged();
    }
    static void onSearchTrampoline(GtkSearchEntry *, gpointer self)
    {
        static_cast<Rack *>(self)->onSearchChanged();
    }
    static void onAddRowTrampoline(GtkListBox *, GtkListBoxRow *row, gpointer self)
    {
        static_cast<Rack *>(self)->onAddRow(row);
    }
    static void onAddShownTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Rack *>(self)->onAddShown();
    }
    static void onControlTrampoline(gpointer, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onControlChanged(*c);
    }
    static void onControlNotifyTrampoline(GObject *, GParamSpec *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onControlChanged(*c);
    }
    static void onCardActionTrampoline(GtkButton *, gpointer action)
    {
        auto *a = static_cast<CardAction *>(action);
        a->rack->onCardAction(*a);
    }
    static void onBypassTrampoline(GObject *widget, GParamSpec *, gpointer effect)
    {
        auto *rack = static_cast<Rack *>(g_object_get_data(widget, "rack"));
        rack->onBypass(core::EffectId{static_cast<uint64_t>(GPOINTER_TO_SIZE(effect))},
                       gtk_switch_get_active(GTK_SWITCH(widget)));
    }

    app::ShellHost &m_host;
    Catalog &m_catalog;
    GtkWidget *m_blockBar = nullptr, *m_blockLane = nullptr, *m_blockRemove = nullptr;
    GtkWidget *m_root = nullptr, *m_scope = nullptr, *m_add = nullptr, *m_addPopover = nullptr;
    GtkWidget *m_search = nullptr, *m_addList = nullptr, *m_title = nullptr, *m_empty = nullptr;
    GtkWidget *m_cards = nullptr, *m_menu = nullptr, *m_titleRow = nullptr, *m_pastePopover = nullptr;
    GtkWidget *m_lookPopover = nullptr, *m_lookName = nullptr, *m_compare = nullptr;
    std::vector<std::unique_ptr<Control>> m_controls;
    std::vector<std::unique_ptr<CardAction>> m_actions;
    std::string m_structure;
    bool m_updating = false;
    bool m_rebuildPending = false;
    // The parameter P pins: by effect and name, since pinning rebuilds the
    // cards (the value becomes animated).
    std::optional<std::pair<core::EffectId, std::string>> m_lastControl;
    GtkWidget *m_transformNote = nullptr; // the Transform card's, while it shows
    uint64_t m_nextGesture = 0;
    std::set<ControlKey> m_armed;
    std::optional<Recording> m_recording;
    std::optional<ControlKey> m_rectKey; // the rect control (or {effect, "mask"}) whose handles are shown
    std::vector<std::unique_ptr<MaskControl>> m_masks;
    std::optional<ControlKey> m_chooseKey; // the file control a chooser is open for
};

} // namespace

void addRack(app::ShellHost &host, Catalog &catalog)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<Rack>> racks;
    racks.push_back(std::make_unique<Rack>(host, catalog));
    racks.back()->install();
}

} // namespace ustudio::effects
