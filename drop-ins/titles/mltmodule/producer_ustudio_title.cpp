// libmltustudio (ADR-012): one MLT producer, `ustudio_title`, that plays a
// .ustitle file through titlerender. MLT's framework C API only, and
// self-contained: the titles core, the renderer and the bits of src/core
// they use are linked in with hidden visibility, so the module works in any
// program that loads it (the editor, u-studio-render, melt) whatever that
// program exports.
//
// Properties:
//   resource        the .ustitle path (the constructor's argument)
//   length          the clip's length in frames; elastic timing fits the
//                   title to it (doc 16), so the hold stretches
//   field.<name>    the clip's value for {{name}}
//   video_index     -1: the picture is off (fully transparent frames), as
//                   the editor switches a clip's video off for media
//   background      "#rrggbb": a title with no background of its own is
//                   drawn on this colour (a flattened export, with no
//                   compositing)
//   timeline_start  where the title's frame 0 falls in the sequence, for
//                   {{timecode}} (0 when unset: the title's own time)
//
// Frames are straight RGBA (mlt_image_rgba) with the title's alpha, at the
// size the consumer asks for, drawn by the same function the titles app
// uses. Frame positions are the producer's own (a cut's frames carry their
// source position), so frame 0 is the clip's first frame when the clip
// starts at in = 0.

#include "core/animation.h"
#include "core/evaluate.h"
#include "core/title_xml.h"
#include "platform/clock.h"
#include "render/title_renderer.h"

#include <framework/mlt.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ustudio::titles;

// Per producer (producer->child). MLT may pull frames on several threads
// (a consumer with real_time > 1), so the cache has a lock; rendering
// happens outside it.
struct Title
{
    TitleDocument doc;
    std::mutex mutex;
    std::set<std::string> logged; // warnings already logged, once each

    // The last frame drawn and what it depended on: consecutive frames of
    // a hold with nothing animating are the same picture.
    // Whether frames differ in more than the layers' states (text in
    // animated units, a scramble, a typewriter's cursor, an animated
    // layer's own frames): then the moment itself is part of the cache key.
    bool textAnimates = false;
    // The text layers with dynamic fields ({{timecode}}, ...): what they
    // say now is part of the key, so {{clip_time}} redraws once a second
    // and {{date}} once a day.
    std::vector<size_t> dynamicLayers;
    std::vector<std::string> lastDynamicText;
    double lastTime = -1.0;
    std::vector<LayerState> lastStates;
    std::map<std::string, std::string> lastFields;
    int lastWidth = 0, lastHeight = 0;
    std::vector<uint8_t> lastImage;
};

Title *titleOf(mlt_producer producer)
{
    return static_cast<Title *>(producer->child);
}

std::map<std::string, std::string> fieldValues(mlt_properties properties)
{
    std::map<std::string, std::string> fields;
    const int count = mlt_properties_count(properties);
    for (int i = 0; i < count; ++i) {
        const char *name = mlt_properties_get_name(properties, i);
        if (name && std::strncmp(name, "field.", 6) == 0) {
            const char *value = mlt_properties_get_value(properties, i);
            fields.emplace(name + 6, value ? value : "");
        }
    }
    return fields;
}

void logOnce(mlt_producer producer, Title &title, const std::vector<std::string> &warnings)
{
    std::lock_guard lock(title.mutex);
    for (const std::string &warning : warnings)
        if (title.logged.insert(warning).second)
            mlt_log_warning(MLT_PRODUCER_SERVICE(producer), "%s\n", warning.c_str());
}

int getImage(mlt_frame frame, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int /*writable*/)
{
    auto producer = static_cast<mlt_producer>(mlt_frame_pop_service(frame));
    Title &title = *titleOf(producer);
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_profile profile = mlt_service_profile(MLT_PRODUCER_SERVICE(producer));
    if (*width <= 0)
        *width = profile->width;
    if (*height <= 0)
        *height = profile->height;
    *format = mlt_image_rgba;

    const double length = mlt_properties_get_int(properties, "length");
    // The position this producer gave the frame (getFrame()): a playlist
    // later sets the frame's position to its own, the sequence frame, so
    // mlt_frame_get_position() here is where the clip is, not how far in.
    const double at = static_cast<double>(mlt_frame_original_position(frame));
    const double time = titleFrame(title.doc, length, at, mlt_profile_fps(profile));
    const std::map<std::string, std::string> fields = fieldValues(properties);
    FieldClock clock;
    clock.clipFrame = at;
    clock.timelineFrame = mlt_properties_get_double(properties, "timeline_start") + at;
    clock.fps = mlt_profile_fps(profile);
    clock.localTime = ustudio::platform::localTime(std::time(nullptr));
    std::vector<std::string> dynamicText;
    for (size_t index : title.dynamicLayers)
        dynamicText.push_back(substituteFields(title.doc.layers[index].text, title.doc.fields, fields, clock));
    std::vector<LayerState> states;
    states.reserve(title.doc.layers.size());
    for (const Layer &layer : title.doc.layers)
        states.push_back(evaluateLayer(layer, title.doc.timing, time));

    const int size = *width * *height * 4; // mlt_image_rgba, as mlt_image_calculate_size() has it
    auto *image = static_cast<uint8_t *>(mlt_pool_alloc(size));
    bool cached = false;
    if (mlt_properties_get_int(properties, "video_index") == -1) {
        std::memset(image, 0, static_cast<size_t>(size));
        cached = true; // nothing to draw
    }
    if (!cached) {
        std::lock_guard lock(title.mutex);
        if (title.lastWidth == *width && title.lastHeight == *height && title.lastStates == states &&
            title.lastFields == fields && title.lastDynamicText == dynamicText &&
            (!title.textAnimates || title.lastTime == time)) {
            std::memcpy(image, title.lastImage.data(), title.lastImage.size());
            cached = true;
        }
    }
    if (!cached) {
        const TitleDocument *doc = &title.doc;
        TitleDocument flattened;
        if (const char *background = mlt_properties_get(properties, "background");
            background && title.doc.background.kind == FillKind::None) {
            if (auto colour = parseColor(background)) {
                flattened = title.doc;
                flattened.background.kind = FillKind::Solid;
                flattened.background.color = *colour;
                doc = &flattened;
            }
        }
        const RenderResult result = renderTitle(*doc, time, fields, *width, *height, &clock);
        toStraightRgba(result.frame, image);
        logOnce(producer, title, result.warnings);
        std::lock_guard lock(title.mutex);
        title.lastStates = std::move(states);
        title.lastTime = time;
        title.lastFields = fields;
        title.lastDynamicText = std::move(dynamicText);
        title.lastWidth = *width;
        title.lastHeight = *height;
        title.lastImage.assign(image, image + static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4);
    }
    *buffer = image;
    mlt_frame_set_image(frame, image, size, mlt_pool_release);

    mlt_properties frameProperties = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(frameProperties, "meta.media.width", *width);
    mlt_properties_set_int(frameProperties, "meta.media.height", *height);
    // sRGB, full range, like MLT's own RGBA producers (producer_colour.c).
    mlt_properties_set_int(frameProperties, "colorspace", mlt_colorspace_rgb);
    mlt_properties_set_int(frameProperties, "color_trc", mlt_color_trc_iec61966_2_1);
    mlt_properties_set_int(frameProperties, "color_primaries", mlt_color_pri_bt709);
    mlt_properties_set_int(frameProperties, "full_range", 1);
    return 0;
}

int getFrame(mlt_producer producer, mlt_frame_ptr frame, int /*index*/)
{
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    if (*frame) {
        mlt_frame_set_position(*frame, mlt_producer_position(producer));
        mlt_properties properties = MLT_FRAME_PROPERTIES(*frame);
        mlt_profile profile = mlt_service_profile(MLT_PRODUCER_SERVICE(producer));
        mlt_properties_set_int(properties, "progressive", 1);
        mlt_properties_set_double(properties, "aspect_ratio", mlt_profile_sar(profile));
        mlt_properties_set_int(properties, "meta.media.width", profile->width);
        mlt_properties_set_int(properties, "meta.media.height", profile->height);
        mlt_properties_set_int(properties, "format", mlt_image_rgba);
        mlt_frame_push_service(*frame, producer);
        mlt_frame_push_get_image(*frame, getImage);
    }
    mlt_producer_prepare_next(producer);
    return 0;
}

// MLT's destructors take void *.
void closeProperties(void *properties)
{
    mlt_properties_close(static_cast<mlt_properties>(properties));
}

void closeProducer(void *self)
{
    auto producer = static_cast<mlt_producer>(self);
    delete titleOf(producer);
    producer->child = nullptr;
    producer->close = nullptr;
    mlt_producer_close(producer);
    free(producer);
}

void *titleInit(mlt_profile profile, mlt_service_type, const char *, const void *arg)
{
    const char *path = static_cast<const char *>(arg);
    if (!path || !*path)
        return nullptr;
    auto read = readTitle(path);
    if (!read) {
        mlt_log_error(nullptr, "[ustudio_title] %s\n", read.error().c_str());
        return nullptr;
    }
    auto producer = static_cast<mlt_producer>(calloc(1, sizeof(mlt_producer_s)));
    if (!producer || mlt_producer_init(producer, nullptr) != 0) {
        free(producer);
        return nullptr;
    }
    auto *title = new Title;
    title->doc = std::move(read->document);
    for (size_t i = 0; i < title->doc.layers.size(); ++i)
        if (title->doc.layers[i].kind == LayerKind::Text && hasDynamicFields(title->doc.layers[i].text))
            title->dynamicLayers.push_back(i);
    for (const Layer &layer : title->doc.layers) {
        const Expansion expansion = expandBehaviors(layer, title->doc.timing);
        if (!layer.animators.empty() || !expansion.animators.empty() || expansion.cursor || expansion.scramble ||
            layer.kind == LayerKind::Lottie)
            title->textAnimates = true;
    }
    producer->child = title;
    producer->get_frame = getFrame;
    producer->close = closeProducer;
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_properties_set(properties, "resource", path);
    mlt_properties_set_double(properties, "aspect_ratio", mlt_profile_sar(profile));
    mlt_properties_set_int(properties, "meta.media.width", title->doc.width);
    mlt_properties_set_int(properties, "meta.media.height", title->doc.height);
    for (const std::string &warning : read->warnings)
        mlt_log_warning(MLT_PRODUCER_SERVICE(producer), "%s: %s\n", path, warning.c_str());
    // A fonts/ folder next to the title (doc 16, "Fonts").
    std::error_code ec;
    const std::filesystem::path fonts = std::filesystem::path(path).parent_path() / "fonts";
    if (std::filesystem::is_directory(fonts, ec))
        addFontDirectory(fonts.string());
    return producer;
}

// MLT's metadata for the service, built here rather than read from a YAML
// file, so the module is one file wherever it's installed. The same shape
// mlt_properties_parse_yaml() gives MLT's own services (checked against
// Repository::metadata() in titles-module).
mlt_properties metadata(mlt_service_type, const char *, void *)
{
    mlt_properties meta = mlt_properties_new();
    mlt_properties_set(meta, "schema_version", "7.0");
    mlt_properties_set(meta, "type", "producer");
    mlt_properties_set(meta, "identifier", "ustudio_title");
    mlt_properties_set(meta, "title", "U-Stu title");
    mlt_properties_set(meta, "version", "1");
    mlt_properties_set(meta, "license", "MIT");
    mlt_properties_set(meta, "language", "en");
    mlt_properties_set(meta, "description",
                       "Plays a .ustitle file (U-Stu Titles): animated text and shapes with alpha, fitted to the "
                       "clip's length (intro and outro keep their timing, the hold stretches).");
    mlt_properties_set(meta, "creator", "Unicorn Tears Project");

    mlt_properties parameters = mlt_properties_new();
    struct Parameter
    {
        const char *identifier, *title, *type, *description, *argument;
    };
    static constexpr Parameter kParameters[] = {
        {"resource", "File", "string", "The .ustitle file.", "yes"},
        {"length", "Length", "integer", "The clip's length in frames; the title's hold stretches to fit it.", "no"},
        {"field.*", "Field values", "string", "A value for each {{field}} in the title's text, e.g. field.name.", "no"},
        {"video_index", "Video", "integer", "-1: the picture is off (transparent frames).", "no"},
        {"background", "Background", "string", "#rrggbb: draw a title with no background of its own on this colour.",
         "no"},
    };
    int index = 0;
    for (const Parameter &p : kParameters) {
        mlt_properties parameter = mlt_properties_new();
        mlt_properties_set(parameter, "identifier", p.identifier);
        mlt_properties_set(parameter, "title", p.title);
        mlt_properties_set(parameter, "type", p.type);
        mlt_properties_set(parameter, "description", p.description);
        mlt_properties_set(parameter, "argument", p.argument);
        mlt_properties_set(parameter, "mutable", "yes");
        const std::string key = std::to_string(index++);
        mlt_properties_set_data(parameters, key.c_str(), parameter, 0, closeProperties, nullptr);
    }
    mlt_properties_set_data(meta, "parameters", parameters, 0, closeProperties, nullptr);
    return meta;
}

} // namespace

extern "C" __attribute__((visibility("default"))) void mlt_register(mlt_repository repository)
{
    // MLT_REGISTER without its C casts: the callbacks have MLT's own types.
    mlt_repository_register(repository, mlt_service_producer_type, "ustudio_title", titleInit);
    mlt_repository_register_metadata(repository, mlt_service_producer_type, "ustudio_title", metadata, nullptr);
}
