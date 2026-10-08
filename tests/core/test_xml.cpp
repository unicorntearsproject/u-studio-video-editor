#include "doctest.h"

#include "core/xml/reader.h"
#include "core/xml/writer.h"

#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <random>
#include <sstream>

using namespace ustudio::core;

namespace {

namespace fs = std::filesystem;

// A unique path per test case and process under the scratch build directory -- never
// touches real project files, cleaned up at the end of each test.
struct TempProjectFile
{
    fs::path path;

    explicit TempProjectFile(const std::string &name)
        : path(fs::temp_directory_path() / ("ustudio-xml-test-" + std::to_string(::getpid()) + "-" + name + ".ustudio"))
    {}
    ~TempProjectFile()
    {
        std::remove(path.string().c_str());
    }
};

AssetId addTestAsset(Model &model, const std::string &path, FrameIndex lengthInFrames = 100'000)
{
    Asset asset;
    asset.path = path;
    asset.displayName = "clip";
    asset.folder = "/b-roll";
    asset.fileFingerprint = "12345:6789";
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.width = 1920;
    asset.info.height = 1080;
    asset.info.fps = {30, 1};
    asset.info.sampleRate = 48000;
    asset.info.audioChannels = 2;
    asset.info.lengthInSequenceFrames = lengthInFrames;
    asset.info.videoCodec = "h264";
    asset.info.audioCodec = "aac";
    asset.info.container = "mp4";
    asset.status = Asset::Status::Ready;
    return model.addAsset(asset);
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeWholeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

} // namespace

TEST_CASE("XML round-trip: empty project")
{
    TempProjectFile file("empty");
    Model model = Model::createEmpty();

    std::string error = saveProject(model, file.path.string());
    CHECK(error.empty());

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: tracks, clips, and a still-image-style asset")
{
    TempProjectFile file("basic");
    Model model = Model::createEmpty();

    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addTestAsset(model, "/home/user/videos/clip.mp4");

    model.insertClip(video, asset, 0, 0, 99);
    model.insertClip(video, asset, 150, 10, 59);    // gap between the two clips
    model.setTrackFlags(video, false, false, true); // locked
    model.setTrackFlags(audio, true, false, false); // muted; left with no clips -- Model has no
                                                    // mutator yet to flip videoEnabled=false for a
                                                    // clip (invariant 8), that's SplitAudio's job
                                                    // once it lands; this still exercises track-level
                                                    // (kind/flags/name) round-trip on an empty track

    std::string error = saveProject(model, file.path.string());
    REQUIRE(error.empty());

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->check().empty());
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: a dissolve transition's overlapping clips survive exactly")
{
    TempProjectFile file("transition");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "/home/user/videos/clip.mp4");

    ClipId a = model.insertClip(track, asset, 0, 0, 49);
    ClipId b = model.insertClip(track, asset, 50, 10, 59); // 10 frames of head handle
    model.addTransition(track, a, b, 6, 4);                // a and b now overlap by 10

    std::string error = saveProject(model, file.path.string());
    REQUIRE(error.empty());

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->check().empty());
    CHECK(*loaded == model);
    // Not just ==: confirm the overlap itself round-tripped, not just
    // happened to compare equal on some other field.
    CHECK(loaded->clip(a).out == model.clip(a).out);
    CHECK(loaded->clip(b).position == model.clip(b).position);
    CHECK(loaded->clip(a).end() > loaded->clip(b).position);
}

TEST_CASE("XML round-trip: relative asset path under the project directory")
{
    TempProjectFile file("relpath");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");

    // Under the same directory as the project file -- writer.cpp should
    // store this relative to that directory, reader.cpp should resolve it
    // back to the exact same absolute path.
    fs::path assetPath = file.path.parent_path() / "media" / "clip.mp4";
    AssetId asset = addTestAsset(model, assetPath.string());
    model.insertClip(track, asset, 0, 0, 49);

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->asset(asset).path == assetPath.string());
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: generator-shorthand resource is preserved verbatim")
{
    TempProjectFile file("generator");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "color:red");
    model.insertClip(track, asset, 0, 0, 49);

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->asset(asset).path == "color:red");
}

TEST_CASE("XML round-trip: markers and settings")
{
    TempProjectFile file("markers");
    Model model = Model::createEmpty();
    Sequence &seq = model.mutableSequence();
    seq.markers.push_back(Marker{MarkerId{1}, 42, "a \"quoted\" marker\nwith a newline", 3});
    seq.markers.push_back(Marker{MarkerId{2}, 100, "second", 1});

    // Settings live on Project, not Sequence -- go through a fresh Model
    // built directly so both markers and settings are present together.
    Project project = model.project();
    project.settings["preview_scale"] = "0.5";
    project.settings["theme"] = "dark";
    Model withSettings(project);

    REQUIRE(saveProject(withSettings, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(*loaded == withSettings);
    REQUIRE(loaded->sequence().markers.size() == 2);
    CHECK(loaded->sequence().markers[0].text == "a \"quoted\" marker\nwith a newline");
}

TEST_CASE("XML round-trip: track visual order survives even though MLT order differs")
{
    TempProjectFile file("order");
    Model model = Model::createEmpty();
    // Visual (model) order: topVideo, bottomVideo, audio -- MLT order is
    // audio, bottomVideo, topVideo, so this specifically exercises the
    // ustudio:visual_index property, not just accidental agreement.
    model.addTrack(Track::Kind::Video, 0, "V-top");
    model.addTrack(Track::Kind::Video, 1, "V-bottom");
    model.addTrack(Track::Kind::Audio, 2, "A1");

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->sequence().tracks.size() == 3);
    CHECK(loaded->sequence().tracks[0].name == "V-top");
    CHECK(loaded->sequence().tracks[1].name == "V-bottom");
    CHECK(loaded->sequence().tracks[2].name == "A1");
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: refuses a file with no ustudio:format_version")
{
    TempProjectFile file("not-ustudio");
    {
        std::ofstream out(file.path);
        out << "<mlt><tractor><track producer=\"black\"/></tractor></mlt>";
    }

    auto loaded = loadProject(file.path.string());
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("XML round-trip: refuses a <profile> with frame_rate_den=\"0\" (audit C3)")
{
    TempProjectFile file("bad-fps");
    REQUIRE(saveProject(Model::createEmpty(), file.path.string()).empty());

    std::string xml = readWholeFile(file.path);
    size_t pos = xml.find("frame_rate_den=\"1\"");
    REQUIRE(pos != std::string::npos);
    xml.replace(pos, std::string("frame_rate_den=\"1\"").size(), "frame_rate_den=\"0\"");
    writeWholeFile(file.path, xml);

    auto loaded = loadProject(file.path.string());
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("XML round-trip: a missing/wrong ustudio:next_id is corrected instead of causing an id collision "
          "(audit C3)")
{
    TempProjectFile file("bad-next-id");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "color:red");
    model.insertClip(track, asset, 0, 0, 49);
    REQUIRE(saveProject(model, file.path.string()).empty());

    // Roll ustudio:next_id back down to "1" -- well below every id already
    // in use (track/asset/clip ids are all > 1 by this point) -- simulating
    // either a missing property (defaults to "1") or a hand-edited one.
    std::string xml = readWholeFile(file.path);
    size_t nameAt = xml.find("name=\"ustudio:next_id\"");
    REQUIRE(nameAt != std::string::npos);
    size_t contentStart = xml.find('>', nameAt) + 1;
    size_t contentEnd = xml.find('<', contentStart);
    xml.replace(contentStart, contentEnd - contentStart, "1");
    writeWholeFile(file.path, xml);

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->check().empty());

    // A fresh id allocated after loading must not collide with anything
    // already in the file -- exactly what a corrected nextId guarantees.
    AssetId newAsset = loaded->addAsset(Asset{});
    CHECK(newAsset != asset);
    CHECK(loaded->check().empty());
}

TEST_CASE("XML round-trip: refuses a file whose clip span is out of range for its asset (audit C3)")
{
    TempProjectFile file("bad-span");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "/home/user/videos/clip.mp4", 100);
    model.insertClip(track, asset, 0, 0, 49); // in-range: [0, 49] of a 100-frame asset
    REQUIRE(saveProject(model, file.path.string()).empty());

    // Push the clip's "out" attribute past the asset's recorded length --
    // Model::check() names this "an invalid in/out range" / "out-of-range
    // source span" (invariant 3), which loadProject() must now refuse
    // rather than silently hand back a Model check() already knows is bad.
    std::string xml = readWholeFile(file.path);
    size_t pos = xml.find("out=\"49\"");
    REQUIRE(pos != std::string::npos);
    xml.replace(pos, std::string("out=\"49\"").size(), "out=\"999\"");
    writeWholeFile(file.path, xml);

    auto loaded = loadProject(file.path.string());
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("XML round-trip: save does not touch the target file on a bad path")
{
    Model model = Model::createEmpty();
    std::string error = saveProject(model, "/nonexistent-directory-for-this-test/project.ustudio");
    CHECK_FALSE(error.empty());
}

// Same random-edit generator as the other property tests, this time
// checked against a save/open round trip every 50 edits -- the
// combination doc 12's M1 acceptance criteria actually asks for
// ("save -> quit -> open restores the timeline identically").
TEST_CASE("XML property: save/open round trip stays exact across 300 random edits")
{
    TempProjectFile file("property");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "/media/generated.mp4", 1'000'000);

    std::mt19937 rng(99);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;

    for (int i = 0; i < 300; ++i) {
        std::uniform_int_distribution<int> pickAction(0, liveClips.empty() ? 0 : 2);
        int action = pickAction(rng);

        if (action == 0 || liveClips.empty()) {
            FrameIndex length = 10 + static_cast<FrameIndex>(rng() % 90);
            ClipId clip = model.insertClip(track, asset, nextFreePosition, 0, length - 1);
            nextFreePosition += length;
            liveClips.push_back(clip);
        } else if (action == 1) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            size_t index = pickClip(rng);
            model.removeClip(liveClips[index]);
            liveClips.erase(liveClips.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            const Clip &current = model.clip(clip);
            if (current.length() > 2) {
                FrameIndex at = current.position + 1 + static_cast<FrameIndex>(rng() % (current.length() - 2));
                liveClips.push_back(model.splitClip(clip, at));
            }
        }

        if (i % 50 != 49)
            continue;

        REQUIRE(saveProject(model, file.path.string()).empty());
        auto loaded = loadProject(file.path.string());
        REQUIRE(loaded.has_value());
        INFO("iteration ", i);
        REQUIRE(*loaded == model);
        REQUIRE(loaded->check().empty());
    }
}

// Format 4 (2026-09-24) moved the render structure and the dissolve
// metadata; every project saved before that is format 3. Hand-written in
// the old writer's exact shape -- one playlist per track holding the model
// clips, the dissolve as a bare ustudio:-only <transition> child of the
// sequence tractor -- so the owner's existing projects keep opening.
TEST_CASE("XML: a format-3 project with a dissolve still opens")
{
    TempProjectFile file("format3");
    Model expected = Model::createEmpty();
    TrackId track = expected.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(expected, "/home/user/videos/clip.mp4");
    ClipId a = expected.insertClip(track, asset, 0, 0, 49);
    ClipId b = expected.insertClip(track, asset, 50, 10, 59);
    TransitionId t = expected.addTransition(track, a, b, 6, 4);
    const Clip &clipA = expected.clip(a);
    const Clip &clipB = expected.clip(b);

    auto entry = [](const Clip &clip) {
        return "<entry producer=\"asset" + std::to_string(clip.asset.value) + "\" in=\"" + std::to_string(clip.in) +
               "\" out=\"" + std::to_string(clip.out) + "\"><property name=\"ustudio:clip_id\">" +
               std::to_string(clip.id.value) + "</property><property name=\"ustudio:position\">" +
               std::to_string(clip.position) + "</property></entry>";
    };
    std::string xml =
        "<?xml version=\"1.0\"?>\n<mlt LC_NUMERIC=\"C\" producer=\"main_bin\" root=\"/tmp\">"
        "<profile width=\"1920\" height=\"1080\" frame_rate_num=\"30\" frame_rate_den=\"1\" sample_aspect_num=\"1\" "
        "sample_aspect_den=\"1\" display_aspect_num=\"16\" display_aspect_den=\"9\" progressive=\"1\" "
        "colorspace=\"709\"/>"
        "<producer id=\"asset" +
        std::to_string(asset.value) +
        "\" in=\"0\" out=\"99999\">"
        "<property name=\"resource\">/home/user/videos/clip.mp4</property>"
        "<property name=\"ustudio:asset_id\">" +
        std::to_string(asset.value) +
        "</property>"
        "<property name=\"ustudio:has_video\">1</property><property name=\"ustudio:has_audio\">1</property>"
        "<property name=\"ustudio:length_in_sequence_frames\">100000</property></producer>"
        "<playlist id=\"track_" +
        std::to_string(track.value) +
        "\">"
        "<property name=\"ustudio:track_id\">" +
        std::to_string(track.value) +
        "</property>"
        "<property name=\"ustudio:visual_index\">0</property><property name=\"ustudio:kind\">video</property>" +
        entry(clipA) + entry(clipB) +
        "</playlist>"
        "<tractor id=\"seq1\" in=\"0\" out=\"59\">"
        "<property name=\"ustudio:format_version\">3</property>"
        "<property name=\"ustudio:next_id\">100</property>"
        "<track producer=\"track_" +
        std::to_string(track.value) +
        "\"/>"
        "<transition><property name=\"ustudio:transition_id\">" +
        std::to_string(t.value) +
        "</property>"
        "<property name=\"ustudio:transition_track\">" +
        std::to_string(track.value) +
        "</property>"
        "<property name=\"ustudio:transition_a\">" +
        std::to_string(a.value) +
        "</property>"
        "<property name=\"ustudio:transition_b\">" +
        std::to_string(b.value) +
        "</property>"
        "<property name=\"ustudio:transition_extend_a\">6</property>"
        "<property name=\"ustudio:transition_extend_b\">4</property>"
        "<property name=\"ustudio:transition_service\">luma</property></transition>"
        "</tractor></mlt>\n";
    writeWholeFile(file.path, xml);

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->check().empty());
    REQUIRE(loaded->sequence().transitions.size() == 1);
    CHECK(loaded->sequence().transitions[0].length == 10);
    CHECK(loaded->clip(a).position == clipA.position);
    CHECK(loaded->clip(a).out == clipA.out);
    CHECK(loaded->clip(b).position == clipB.position);
    CHECK(loaded->clip(b).in == clipB.in);
}

// --- Format 5 (IP2, doc 15): effects and the rest of IP1's data ---------------

namespace {

std::string readFile(const fs::path &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Effect richEffect(const std::string &service)
{
    Effect effect;
    effect.service = service;
    effect.displayName = "Rich " + service;
    effect.owner = "effects";
    Param level;
    level.name = "level";
    level.value = 0.25;
    level.keyframes = {{-5, 0.0, Easing::SmoothNatural}, {10, 0.5, Easing::BounceInOut}, {40, 1.0, Easing::Discrete}};
    Param colour;
    colour.name = "color";
    colour.value = Color{255, 43, 214, 128};
    Param rect;
    rect.name = "rect";
    rect.value = Rect{0.1, 0.2, 0.5, 0.25};
    Param text;
    text.name = "text";
    text.value = std::string("a;b=c \"quoted\"");
    Param count;
    count.name = "count";
    count.value = int64_t{7};
    Param flag;
    flag.name = "invert";
    flag.value = true;
    effect.params = {level, colour, rect, text, count, flag};
    effect.mix = {0.75, {{0, 0.0, Easing::CubicIn}, {20, 1.0, Easing::Linear}}};
    return effect;
}

} // namespace

TEST_CASE("XML format 5: effects, masks, blocks, looks, source params and recipes round-trip exactly")
{
    TempProjectFile file("format5");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "color:red");
    ClipId a = model.insertClip(track, asset, 0, 100, 199);
    ClipId b = model.insertClip(track, asset, 100, 500, 599);
    TransitionId dissolve = model.addTransition(track, a, b, 5, 5);
    Param softness;
    softness.name = "softness";
    softness.value = 0.3;
    model.setTransitionRecipe(dissolve, "wipe-left", {softness});

    Effect clipEffect = richEffect("brightness");
    clipEffect.mask =
        EffectMask{"ellipse", {softness}, {0.1, {{0, 0.0, Easing::Linear}, {9, 0.4, Easing::QuadraticOut}}}, true};
    model.addEffect(Model::EffectTarget::clip(a), clipEffect, 0);
    Effect disabled = richEffect("volume");
    disabled.enabled = false;
    model.addEffect(Model::EffectTarget::clip(a), disabled, 1);
    model.addEffect(Model::EffectTarget::track(track), richEffect("volume"), 0);
    model.addEffect(Model::EffectTarget::sequence(), richEffect("brightness"), 0);
    AdjustmentBlock block;
    block.lane = 1;
    block.start = 30;
    block.length = 90;
    block.fadeOut = FadeSpec{10};
    block.effects = {richEffect("sepia")};
    model.addAdjustmentBlock(block);
    model.addLook(Look{{}, "Warm & \"grainy\"", {richEffect("sepia"), richEffect("brightness")}});
    Param title;
    title.name = "markup";
    title.value = std::string("<b>Hello</b>");
    title.keyframes = {{0, 0.0, Easing::Linear}};
    model.setClipSourceParams(b, {title});
    REQUIRE(model.check().empty());

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->project() == model.project());

    // Saved again, byte for byte the same file.
    const std::string first = readFile(file.path);
    REQUIRE(saveProject(*loaded, file.path.string()).empty());
    CHECK(readFile(file.path) == first);
}

TEST_CASE("XML format 5: effects play in melt too -- native filters on every cut, animated per cut")
{
    TempProjectFile file("format5-native");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "color:red");
    ClipId a = model.insertClip(track, asset, 0, 100, 199);
    ClipId b = model.insertClip(track, asset, 100, 500, 599);
    model.addTransition(track, a, b, 5, 5); // a's last 10 frames play in the dissolve
    Effect fade;
    fade.service = "brightness";
    Param level;
    level.name = "level";
    level.value = 1.0;
    level.keyframes = {{0, 0.0, Easing::Linear}, {100, 1.0, Easing::CubicIn}};
    fade.params = {level};
    model.addEffect(Model::EffectTarget::clip(a), fade, 0);
    REQUIRE(saveProject(model, file.path.string()).empty());
    const std::string text = readFile(file.path);

    CHECK(text.find("<property name=\"ustudio:format_version\">6</property>") != std::string::npos); // 6: transforms
    // The record (model) copy: the whole animation, as written.
    CHECK(text.find("<property name=\"level\">0=0;100g=1</property>") != std::string::npos);
    // a's exclusive cut (its first 95 frames): the value at its last frame
    // interpolated there.
    CHECK(text.find("<property name=\"level\">0=0;94=0.94</property>") != std::string::npos);
    // a's tail inside the dissolve (frames 95-104 of a, extended by 5):
    // starts at the interpolated value, ends past a's last keyframe.
    CHECK(text.find("<property name=\"level\">0=0.95;5g=1</property>") != std::string::npos);
}

TEST_CASE("XML format 5: an effect's mix is written as MLT's mask pair, which the reader skips")
{
    TempProjectFile file("format5-mix");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    ClipId clip = model.insertClip(track, addTestAsset(model, "color:red"), 0, 0, 49);
    Effect half;
    half.service = "brightness";
    half.mix = {0.5, {}};
    model.addEffect(Model::EffectTarget::clip(clip), half, 0);
    Effect ramp;
    ramp.service = "sepia";
    ramp.mix = {1.0, {{0, 0.0, Easing::Linear}, {49, 1.0, Easing::Linear}}};
    model.addEffect(Model::EffectTarget::track(track), ramp, 0);
    REQUIRE(saveProject(model, file.path.string()).empty());
    const std::string text = readFile(file.path);

    // core::nativeFilters(): mask_start runs the effect, mask_apply
    // composites it back at the mix -- never with mask_apply's default
    // transition, qtblend (ADR-007).
    CHECK(text.find("<property name=\"mlt_service\">mask_start</property>") != std::string::npos);
    CHECK(text.find("<property name=\"filter\">brightness</property>") != std::string::npos);
    CHECK(text.find("<property name=\"transition\">frei0r.cairoblend</property>") != std::string::npos);
    CHECK(text.find("<property name=\"transition.0\">0.5</property>") != std::string::npos);
    CHECK(text.find("qtblend") == std::string::npos);
    // A keyframed mix animates brightness's alpha between the pair.
    CHECK(text.find("<property name=\"alpha\">0=0;49=1</property>") != std::string::npos);

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->project() == model.project()); // one effect each, not three
}

TEST_CASE("XML format 4 still loads (migration), and saves as format 5")
{
    const fs::path fixture = fs::path(TEST_DATA_DIR) / "format4.ustudio";
    auto loaded = loadProject(fixture.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->sequence().tracks.size() == 2);
    CHECK(loaded->sequence().clips.size() == 3);
    REQUIRE(loaded->sequence().transitions.size() == 1);
    CHECK(loaded->sequence().transitions[0].recipe.empty());
    CHECK(loaded->sequence().markers.size() == 1);
    CHECK(loaded->sequence().effects.empty());

    TempProjectFile file("format4-migrated");
    REQUIRE(saveProject(*loaded, file.path.string()).empty());
    auto again = loadProject(file.path.string());
    REQUIRE(again.has_value());
    CHECK(again->project() == loaded->project());
}

TEST_CASE("xml: the sequence background is saved as #rrggbb; anything else reads as black")
{
    CHECK(backgroundHex(0x3366cc) == "#3366cc");
    CHECK(backgroundResource(0x3366cc) == "0x3366ccff");
    CHECK(parseBackgroundHex("#3366CC") == 0x3366ccu);
    CHECK_FALSE(parseBackgroundHex("3366cc").has_value());
    CHECK_FALSE(parseBackgroundHex("#3366c").has_value());
    CHECK_FALSE(parseBackgroundHex("#3366cg").has_value());
    CHECK_FALSE(parseBackgroundHex("#3366cc; rm").has_value());
}

TEST_CASE("XML format 7 only where an older build would lose data: transform keyframes")
{
    TempProjectFile file("format7");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    ClipId clip = model.insertClip(track, addTestAsset(model, "color:red"), 0, 0, 99);
    Transform placed;
    placed.bounds = Transform::Bounds::None;
    placed.x.value = 960;
    placed.y.value = 540;
    placed.width.value = 960;
    placed.height.value = 540;
    model.setClipTransform(clip, placed);
    REQUIRE(saveProject(model, file.path.string()).empty());
    // Static values: format 6 still, so older builds open it.
    CHECK(readFile(file.path).find("<property name=\"ustudio:format_version\">6</property>") != std::string::npos);

    // Keys: an older build would keep the value and drop them, and lose
    // them for good on its next save, so it must refuse the file (7).
    placed.x.keyframes = {{0, 480, Easing::Linear}, {50, 1440, Easing::CubicOut}};
    model.setClipTransform(clip, placed);
    REQUIRE(saveProject(model, file.path.string()).empty());
    CHECK(readFile(file.path).find("<property name=\"ustudio:format_version\">7</property>") != std::string::npos);
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->clip(clip).transform.get() == placed);
}

// doc 09, "Versioning": every format a past U-Stu wrote still opens. The
// fixtures are real files from each format's own writer
// (data/generate_format_fixture.cpp.txt says how); 6 and 7 also come from
// today's writer.
TEST_CASE("XML: every past project format still opens, with its content, and re-saves")
{
    for (int version : {3, 4, 5, 6}) {
        INFO("format " << version);
        const fs::path fixture = fs::path(TEST_DATA_DIR) / ("format" + std::to_string(version) + ".ustudio");
        auto loaded = loadProjectFile(fixture.string());
        REQUIRE(loaded.has_value());
        const Sequence &seq = loaded->sequence();
        CHECK(seq.tracks.size() == 2);
        CHECK(seq.clips.size() == 3);
        CHECK(seq.transitions.size() == 1);
        if (version >= 5) {
            CHECK(seq.markers.size() == 1);
            bool keyed = false;
            for (const auto &[id, clip] : seq.clips)
                for (const Effect &effect : clip.effects)
                    keyed = keyed || (effect.service == "brightness" && !effect.params.empty() &&
                                      effect.params.front().keyframes.size() == 2);
            CHECK(keyed);
        }
        if (version == 6) {
            bool placed = false;
            for (const auto &[id, clip] : seq.clips)
                placed = placed || (clip.transform.get().bounds == Transform::Bounds::None &&
                                    clip.transform.get().rotation.value == 15 && clip.transform.get().flipH);
            CHECK(placed);
        }
        TempProjectFile file("format-resave");
        REQUIRE(saveProject(*loaded, file.path.string()).empty());
        auto again = loadProjectFile(file.path.string());
        REQUIRE(again.has_value());
        CHECK(again->project() == loaded->project());
    }
    CHECK(oldestReadableProjectFormat() == 3);
    CHECK(newestReadableProjectFormat() == 7);
}

TEST_CASE("XML: a project that can't open says why, by kind; the version that saved it is recorded")
{
    TempProjectFile file("load-errors");
    Model model = Model::createEmpty();
    model.insertClip(model.addTrack(Track::Kind::Video, 0, "V1"), addTestAsset(model, "color:red"), 0, 0, 9);
    REQUIRE(saveProject(model, file.path.string()).empty());
    std::string text = readFile(file.path);
    CHECK(text.find("<property name=\"ustudio:saved_by\">" USTUDIO_VERSION "</property>") != std::string::npos);

    // A newer format: refused, naming the version that wrote it.
    const std::string current = "<property name=\"ustudio:format_version\">6</property>";
    REQUIRE(text.find(current) != std::string::npos);
    text.replace(text.find(current), current.size(), "<property name=\"ustudio:format_version\">99</property>");
    const std::string saver = "<property name=\"ustudio:saved_by\">" USTUDIO_VERSION "</property>";
    text.replace(text.find(saver), saver.size(), "<property name=\"ustudio:saved_by\">9.1.0</property>");
    {
        std::ofstream out(file.path, std::ios::trunc);
        out << text;
    }
    auto newer = loadProjectFile(file.path.string());
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().kind == ProjectLoadError::Kind::TooNew);
    CHECK(newer.error().formatVersion == 99);
    CHECK(newer.error().savedBy == "9.1.0");
    CHECK(loadProject(file.path.string()).error().find("saved by 9.1.0") != std::string::npos);

    auto missing = loadProjectFile(file.path.string() + ".gone");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind == ProjectLoadError::Kind::Missing);

    // The first prototype's INI projects: named for what they are.
    {
        std::ofstream out(file.path, std::ios::trunc);
        out << "[Project]\nTrackCount=1\n\n[Track0]\nClipCount=0\n";
    }
    auto prototype = loadProjectFile(file.path.string());
    REQUIRE_FALSE(prototype.has_value());
    CHECK(prototype.error().kind == ProjectLoadError::Kind::NotAProject);
    CHECK(prototype.error().message.find("prototype") != std::string::npos);
}

TEST_CASE("XML: an effect from an add-on this build doesn't have is kept, not dropped")
{
    TempProjectFile file("unknown-effect");
    Model model = Model::createEmpty();
    const ClipId clip =
        model.insertClip(model.addTrack(Track::Kind::Video, 0, "V1"), addTestAsset(model, "color:red"), 0, 0, 9);
    Effect fx;
    fx.service = "acme.glow"; // no such MLT service here
    Param radius;
    radius.name = "radius";
    radius.value = 4.5;
    Param look;
    look.name = "look";
    look.value = std::string("warm");
    fx.params = {radius, look};
    model.addEffect(Model::EffectTarget::clip(clip), fx, 0);
    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProjectFile(file.path.string());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->clip(clip).effects.size() == 1);
    CHECK(loaded->clip(clip).effects.front().service == "acme.glow");
    CHECK(loaded->clip(clip).effects.front().params == fx.params);
    TempProjectFile again("unknown-effect-2");
    REQUIRE(saveProject(*loaded, again.path.string()).empty());
    CHECK(loadProjectFile(again.path.string())->project() == loaded->project());
}
