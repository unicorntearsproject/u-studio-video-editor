// BT.709 sources keep their colours in preview and export: the graph's
// black background (producer_colour) used to tag every composited frame
// BT.601, so red came out 233 and cyan's red 22 (2026-09-27).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/render/render_profile.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "platform/process.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>
#include <memory>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

struct Rgb
{
    int r, g, b;
};

Rgb pixelAt(Mlt::Producer &producer, int position, int x, int y)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = 1920, h = 1080;
    const uint8_t *px = frame->get_image(format, w, h) + (static_cast<size_t>(y) * 1920 + x) * 4;
    return {px[0], px[1], px[2]};
}

// Red on the left half, cyan on the right, BT.709 H.264 at 1080p.
fs::path makeBars(const fs::path &path)
{
    Model model = Model::createEmpty(); // 1080p30, colorspace 709
    EngineSync sync(model);
    Mlt::Profile &p = sync.profile();
    Mlt::Tractor tractor(p);
    Mlt::Producer red(p, "color:#ff0000"), cyan(p, "color:#00ffff");
    // RGBA, so the encoder's conversion to YUV uses the profile's BT.709
    // (a colour producer's own YUV is tagged 601: the bug under test).
    red.set("mlt_image_format", "rgba");
    cyan.set("mlt_image_format", "rgba");
    red.set_in_and_out(0, 29);
    cyan.set_in_and_out(0, 29);
    tractor.set_track(red, 0);
    tractor.set_track(cyan, 1);
    Mlt::Transition half(p, "composite");
    half.set("geometry", "960/0:960x1080");
    half.set("distort", 1);
    std::unique_ptr<Mlt::Field> field(tractor.field());
    field->plant_transition(half, 0, 1);
    Mlt::Consumer consumer(p, "avformat", utf8String(path).c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.set("crf", 1);
    consumer.set("an", 1);
    consumer.set("real_time", -1);
    consumer.connect(tractor);
    consumer.run();
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
    return path;
}

void checkColours(const Rgb &red, const Rgb &cyan)
{
    CAPTURE(red.r);
    CAPTURE(cyan.r);
    CHECK(red.r >= 248); // 233 with the 601 matrix
    CHECK(red.g <= 8);
    CHECK(cyan.r <= 8); // 22 with the 601 matrix
    CHECK(cyan.g >= 248);
    CHECK(cyan.b >= 248);
}

} // namespace

TEST_CASE("colour: a BT.709 source keeps its colours through the graph and in a render")
{
    static FactoryPolicy policy;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-colour-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    const fs::path bars = makeBars(dir / "bars709.mp4");
    {
        Mlt::Profile profile("atsc_1080p_30");
        Mlt::Producer alone(profile, utf8String(bars).c_str());
        checkColours(pixelAt(alone, 10, 480, 540), pixelAt(alone, 10, 1440, 540)); // the source is right
    }

    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = utf8String(bars);
    asset.status = Asset::Status::Ready;
    asset.info.hasVideo = true;
    asset.info.width = 1920;
    asset.info.height = 1080;
    asset.info.lengthInSequenceFrames = 30;
    model.insertClip(track, model.addAsset(asset), 0, 0, 29);
    {
        EngineSync sync(model);
        checkColours(pixelAt(sync.tractor(), 10, 480, 540), pixelAt(sync.tractor(), 10, 1440, 540));
    }

    const fs::path rendered = dir / "render.mp4";
    std::string error;
    REQUIRE(renderProject(model, utf8String(rendered), error));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, utf8String(rendered).c_str());
    REQUIRE(decoded.is_valid());
    checkColours(pixelAt(decoded, 10, 480, 540), pixelAt(decoded, 10, 1440, 540));
    fs::remove_all(dir);
}

TEST_CASE("colour: the project background survives save and load, and plays and exports as that colour")
{
    static FactoryPolicy policy;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-bg-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    auto near = [](const Rgb &px, uint32_t rgb, int tolerance) {
        CAPTURE(px.r);
        CAPTURE(px.g);
        CAPTURE(px.b);
        CHECK(std::abs(px.r - static_cast<int>(rgb >> 16 & 0xff)) <= tolerance);
        CHECK(std::abs(px.g - static_cast<int>(rgb >> 8 & 0xff)) <= tolerance);
        CHECK(std::abs(px.b - static_cast<int>(rgb & 0xff)) <= tolerance);
    };

    // A white clip at 30-59: frames 0-29 are the background alone.
    Model model = Model::createEmpty();
    const TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset white;
    white.path = "color:white";
    white.info.hasVideo = true;
    white.info.lengthInSequenceFrames = 10'000;
    model.insertClip(track, model.addAsset(white), 30, 0, 29);
    UndoStack undo(model);
    REQUIRE(undo.execute(std::make_unique<SetSequenceBackground>(0x3366cc)));
    CHECK_FALSE(undo.execute(std::make_unique<SetSequenceBackground>(0x3366cc))); // already that colour
    CHECK(undo.undoLabel() == "Set background colour");

    const fs::path project = dir / "background.ustudio";
    REQUIRE(saveProject(model, utf8String(project)).empty());
    auto loaded = loadProject(utf8String(project));
    REQUIRE(loaded.has_value());
    CHECK(loaded->sequence().background == 0x3366cc);

    {
        EngineSync sync(*loaded);
        INFO("editor");
        near(pixelAt(sync.tractor(), 5, 960, 540), 0x3366cc, 2);
        near(pixelAt(sync.tractor(), 45, 960, 540), 0xffffff, 2); // the clip covers it
        Mlt::Producer melt(sync.profile(), "xml", utf8String(project).c_str());
        REQUIRE(melt.is_valid());
        INFO("melt");
        near(pixelAt(melt, 5, 960, 540), 0x3366cc, 2); // melt plays the saved file the same

        // A new colour rebuilds the graph with it; undo restores it.
        undo.undo();
        CHECK(model.sequence().background == Sequence::kDefaultBackground);
        model.setSequenceBackground(0x808080); // a grey: the YUV path
        sync.setProject(model.snapshot());
        INFO("grey");
        near(pixelAt(sync.tractor(), 5, 960, 540), 0x808080, 2);
    }

    const fs::path rendered = dir / "background.mp4";
    std::string error;
    REQUIRE(renderProject(*loaded, utf8String(rendered), error));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, utf8String(rendered).c_str());
    REQUIRE(decoded.is_valid());
    near(pixelAt(decoded, 5, 960, 540), 0x3366cc, 6); // lossy H.264
    fs::remove_all(dir);
}

namespace {

// A rotated white rectangle on transparent, as a 1080p PNG: every edge is
// anti-aliased, with partial alpha (MLT's affine filter, avformat's png).
// A still with alpha played for 10 frames, encoded as `vcodec`/`pixFmt`
// (and any codec options) with the alpha kept.
fs::path makeAlphaVideo(const fs::path &still, const fs::path &path, const char *format, const char *vcodec,
                        const char *pixFmt, std::vector<std::pair<const char *, const char *>> options = {})
{
    Model model = Model::createEmpty();
    EngineSync sync(model);
    Mlt::Profile &p = sync.profile();
    Mlt::Producer picture(p, utf8String(still).c_str());
    picture.set_in_and_out(0, 9);
    Mlt::Consumer consumer(p, "avformat", utf8String(path).c_str());
    consumer.set("f", format);
    consumer.set("vcodec", vcodec);
    consumer.set("pix_fmt", pixFmt);
    // Asked for RGBA, the consumer converts to a yuva pix_fmt with the
    // alpha; left to pick from the pix_fmt it asks for yuv422 and drops it
    // for anything but rgba/argb/bgra (consumer_avformat.c, MLT 7.40).
    consumer.set("mlt_image_format", "rgba");
    for (const auto &[name, value] : options)
        consumer.set(name, value);
    consumer.set("an", 1);
    consumer.set("real_time", -1);
    consumer.connect(picture);
    consumer.run();
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
    return path;
}

fs::path makeAlphaStill(const fs::path &dir)
{
    Model model = Model::createEmpty();
    EngineSync sync(model);
    Mlt::Profile &p = sync.profile();
    Mlt::Producer white(p, "color:0xffffffff");
    white.set("mlt_image_format", "rgba");
    white.set_in_and_out(0, 0);
    Mlt::Filter affine(p, "affine");
    affine.set("use_normalized", 1);
    affine.set("transition.rect", "501 301 901 401 1");
    affine.set("transition.distort", 1);
    affine.set("transition.repeat_off", 1);
    affine.set("transition.mirror_off", 1);
    affine.set("transition.fix_rotate_x", 7);
    white.attach(affine);
    Mlt::Consumer consumer(p, "avformat", utf8String(dir / "still_%d.png").c_str());
    consumer.set("f", "image2");
    consumer.set("vcodec", "png");
    consumer.set("pix_fmt", "rgba");
    consumer.set("start_number", 1);
    consumer.set("an", 1);
    consumer.set("real_time", -1);
    consumer.connect(white);
    consumer.run();
    consumer.stop(); // joins the render-ahead thread (notes/render.md)
    return dir / "still_1.png";
}

// Over red, white keeps red at 255 at every coverage, and green equals
// blue: an edge pixel with less red, or a hue, is the 4:2:2 fringe.
struct FringeLimits
{
    int darkest;    // the lowest red allowed on an edge
    int skew;       // the largest |green - blue|
    double deficit; // the largest mean (255 - red) over the edges
};

void checkNoFringe(Mlt::Producer &producer, int position, const FringeLimits &limits)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgba;
    int w = 1920, h = 1080;
    const uint8_t *image = frame->get_image(format, w, h);
    int edges = 0, darkest = 255, skew = 0;
    double deficit = 0;
    for (size_t i = 0; i < size_t{1920} * 1080 * 4; i += 4) {
        const int r = image[i], g = image[i + 1], b = image[i + 2];
        if (g > 10 && g < 245) { // partly covered
            ++edges;
            darkest = std::min(darkest, r);
            deficit += 255 - r;
            skew = std::max(skew, std::abs(g - b));
        }
    }
    CAPTURE(edges);
    CHECK(edges > 500);
    CHECK(darkest >= limits.darkest);
    CHECK(skew <= limits.skew);
    CHECK(deficit / std::max(edges, 1) <= limits.deficit);
}

} // namespace

TEST_CASE("colour: a still's anti-aliased alpha edges over video have no dark or coloured fringe")
{
    static FactoryPolicy policy;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-fringe-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    const fs::path still = makeAlphaStill(dir);
    REQUIRE(fs::exists(still));

    Model model = Model::createEmpty();
    const TrackId upper = model.addTrack(Track::Kind::Video, 0, "V2");
    const TrackId lower = model.addTrack(Track::Kind::Video, 1, "V1");
    Asset red;
    red.path = "color:red";
    red.info.hasVideo = true;
    red.info.lengthInSequenceFrames = 10'000;
    model.insertClip(lower, model.addAsset(red), 0, 0, 59);
    Asset picture;
    picture.path = utf8String(still);
    picture.status = Asset::Status::Ready;
    picture.info.hasVideo = true;
    picture.info.isStillImage = true;
    picture.info.width = 1920;
    picture.info.height = 1080;
    const ClipId stillClip = model.insertClip(upper, model.addAsset(picture), 0, 0, 59);
    {
        EngineSync sync(model);
        // Without the pairing: darkest 55, skew 60, mean deficit 100.
        checkNoFringe(sync.tractor(), 30, {250, 4, 2.0});
    }

    const fs::path rendered = dir / "fringe.mp4";
    std::string error;
    const auto &presets = builtInRenderProfiles();
    const auto high = std::find_if(presets.begin(), presets.end(),
                                   [](const RenderProfile &p) { return p.name == kDefaultRenderProfileName; });
    REQUIRE(high != presets.end());
    REQUIRE(renderProject(model, utf8String(rendered), error, {}, nullptr, *high));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer decoded(profile, utf8String(rendered).c_str());
    REQUIRE(decoded.is_valid());
    // H.264's 4:2:0 shares one colour between four pixels, which darkens a
    // sharp red edge by itself (opaque white would too): 146, 19, 33 here,
    // against 49, 64, 59 without the pairing.
    checkNoFringe(decoded, 30, {110, 32, 45.0});

    // A still with its video turned off shows nothing (MLT's pixbuf ignores
    // video_index=-1, which turns a video's picture off).
    model.setClipEnabled(stillClip, false, true);
    {
        EngineSync sync(model);
        sync.tractor().seek(30);
        std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = 1920, h = 1080;
        const uint8_t *centre = frame->get_image(format, w, h) + (static_cast<size_t>(500) * 1920 + 950) * 4;
        CHECK(centre[1] < 30); // red, not the white rectangle
    }
    fs::remove_all(dir);
}

TEST_CASE("colour: videos with alpha are probed as such and composite without a fringe")
{
    static FactoryPolicy policy;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-alphav-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    struct Case
    {
        const char *name, *file, *format, *vcodec, *pixFmt;
        std::vector<std::pair<const char *, const char *>> options;
    };
    // What U-Stu Titles exports for OBS and other apps (T2d).
    const std::vector<Case> cases = {
        {"ProRes 4444", "p.mov", "mov", "prores_ks", "yuva444p10le", {{"profile", "4444"}}},
        {"VP9 alpha", "v.webm", "webm", "libvpx-vp9", "yuva420p", {{"vb", "4M"}}},
        {"QuickTime Animation", "q.mov", "mov", "qtrle", "argb", {}},
    };
    const fs::path still = makeAlphaStill(dir);
    {
        Model probeModel = Model::createEmpty();
        EngineSync probeSync(probeModel);
        CHECK_FALSE(probeSync.probeMedia(utf8String(makeBars(dir / "opaque.mp4"))).hasAlpha);
        CHECK(probeSync.probeMedia(utf8String(still)).hasAlpha); // a PNG with alpha, for proxies
    }
    for (const Case &c : cases) {
        INFO(std::string(c.name));
        const fs::path path = makeAlphaVideo(still, dir / c.file, c.format, c.vcodec, c.pixFmt, c.options);
        REQUIRE(fs::exists(path));
        Model model = Model::createEmpty();
        EngineSync::ProbedMedia probed;
        {
            EngineSync probeSync(model);
            probed = probeSync.probeMedia(utf8String(path));
        }
        CHECK(probed.hasAlpha);
        const TrackId upper = model.addTrack(Track::Kind::Video, 0, "V2");
        const TrackId lower = model.addTrack(Track::Kind::Video, 1, "V1");
        Asset red;
        red.path = "color:red";
        red.info.hasVideo = true;
        red.info.lengthInSequenceFrames = 10'000;
        model.insertClip(lower, model.addAsset(red), 0, 0, 9);
        Asset overlay;
        overlay.path = utf8String(path);
        overlay.status = Asset::Status::Ready;
        overlay.info.hasVideo = true;
        overlay.info.hasAlpha = probed.hasAlpha;
        overlay.info.width = 1920;
        overlay.info.height = 1080;
        overlay.info.lengthInSequenceFrames = 10;
        model.insertClip(upper, model.addAsset(overlay), 0, 0, 9);
        EngineSync sync(model);
        // VP9 keeps its alpha at 4:2:0 and lossy: an edge a shade darker
        // (249 measured), still no fringe (55 without the pairing).
        checkNoFringe(sync.tractor(), 5, {std::string(c.name) == "VP9 alpha" ? 240 : 250, 6, 3.0});
    }
    fs::remove_all(dir);
}
