#include "writer.h"

#include "core/model/effect_native.h"
#include "core/model/transform.h"
#include "core/model/transition_native.h"
#include "core/log.h"
#include "core/media/utf8_path.h"

#include "core/model/audio_level.h"
#include "core/model/mlt_order.h"
#include "core/model/track_segments.h"
#include "core/xml/effect_io.h"

#include <libxml/tree.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

// libxml2's BAD_CAST is a C-style cast (used throughout this file to
// satisfy its const xmlChar* API from our const char*/std::string data).
// Unlike the GTK macro-noise case (docs/plans/v2/11), a pragma push/pop
// actually works here: it wraps our own function bodies directly, not an
// #include block whose macros get invoked from call sites elsewhere.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::core {

namespace {

namespace fs = std::filesystem;

// doc 09; 3 adds ustudio:position (below) and transitions; 4 splits each
// track into a render playlist (what the tractor plays, matching EngineSync
// exactly) and a record playlist (the model, what the reader reads); 5 adds
// effects as <filter>s, clip source parameters, transition recipes,
// adjustment blocks and looks (IP2, doc 15; core/xml/effect_io.h).
// 6: clip transforms (ADR-018). 7: what an older build would lose, written
// only when a file holds it (xml_detail::requireFormatVersion()): transform
// keyframes. Otherwise 6, so older builds still open the file.
constexpr int kFormatVersion = 6;

// Every effect in `effects` as a <filter> under `parent` (effect_io.h);
// `cutIn` for a clip's render cut.
void writeEffects(xmlNodePtr parent, const std::vector<Effect> &effects, FrameIndex offset, FrameIndex length,
                  bool withModel, std::optional<FrameIndex> cutIn = std::nullopt)
{
    for (const Effect &effect : effects)
        xml_detail::writeEffectFilter(parent, effect, offset, length, withModel, cutIn);
}

xmlNodePtr addProperty(xmlNodePtr parent, const std::string &name, const std::string &value)
{
    xmlNodePtr prop = xmlNewTextChild(parent, nullptr, BAD_CAST "property", BAD_CAST value.c_str());
    xmlNewProp(prop, BAD_CAST "name", BAD_CAST name.c_str());
    return prop;
}

// std::to_string(double) formats via the process's C locale (LC_NUMERIC),
// which can be non-"C" (GTK/GLib i18n init calls setlocale(LC_ALL, "")) --
// exactly what this file's own LC_NUMERIC="C" root attribute exists to
// keep out of the saved values themselves. std::to_chars is specified
// locale-independent, and reader.cpp's toDouble() (std::from_chars) is its
// exact inverse.
std::string doubleToString(double value)
{
    std::array<char, 64> buf{};
    auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    return std::string(buf.data(), result.ptr);
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string markersToJson(const std::vector<Marker> &markers)
{
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < markers.size(); ++i) {
        const Marker &m = markers[i];
        if (i > 0)
            out << ",";
        out << "{\"id\":" << m.id.value << ",\"at\":" << m.at << ",\"text\":\"" << jsonEscape(m.text)
            << "\",\"color\":" << static_cast<int>(m.color) << "}";
    }
    out << "]";
    return out.str();
}

std::string settingsToJson(const std::map<std::string, std::string> &settings)
{
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto &[key, value] : settings) {
        if (!first)
            out << ",";
        first = false;
        out << "\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"";
    }
    out << "}";
    return out.str();
}

// "service:argument" for an MLT generator shorthand, split into its two
// halves; nullopt for a filesystem path. Same shape test EngineSync's
// verify() uses: a colon before any '/' (an absolute or relative file path
// has no colon before its first slash).
std::optional<std::pair<std::string, std::string>> splitGeneratorShorthand(const std::string &path)
{
    size_t colon = path.find(':');
    if (colon == std::string::npos || colon == 0 || path.find('/') < colon)
        return std::nullopt;
    return std::make_pair(path.substr(0, colon), path.substr(colon + 1));
}

// Relative to `projectDir` when the asset lives under it (doc 09), else
// left absolute.
std::string relativizePath(const std::string &assetPath, const fs::path &projectDir)
{
    if (assetPath.empty() || assetPath.front() != '/')
        return assetPath; // not an absolute filesystem path (e.g. a color:/noise:/tone: generator) -- leave as-is

    std::error_code ec;
    fs::path relative = fs::relative(assetPath, projectDir, ec);
    if (ec || relative.empty() || relative.native().starts_with(".."))
        return assetPath;
    return relative.string();
}

// What MLT needs to open an asset's media. A generator shorthand
// ("color:red", "tone:") only loads through MLT's xml producer as an
// explicit mlt_service plus its argument as the resource -- written as a
// bare "resource" it loads as black/silence (confirmed with a standalone
// repro against MLT 7.40, 2026-09-24; a real media path loads fine either
// way). The original path is kept in ustudio:path, which the reader prefers.
void writeMediaSource(xmlNodePtr producer, const Asset &asset, const fs::path &projectDir)
{
    if (std::optional<std::pair<std::string, std::string>> generator = splitGeneratorShorthand(asset.path)) {
        addProperty(producer, "mlt_service", generator->first);
        addProperty(producer, "resource", generator->second);
        addProperty(producer, "ustudio:path", asset.path);
        // A colour's own YUV is tagged BT.601, which MLT composites unconverted
        // into our 709 graph; as RGBA it's converted with the profile's
        // matrix (EngineSync does the same; docs/developer/notes/engine-sync.md).
        if (generator->first == "color" || generator->first == "colour")
            addProperty(producer, "mlt_image_format", "rgba");
    } else {
        addProperty(producer, "resource", relativizePath(asset.path, projectDir));
    }
}

void writeAssetProducer(xmlNodePtr mlt, const Asset &asset, const fs::path &projectDir, const std::string &nodeId)
{
    xmlNodePtr producer = xmlNewChild(mlt, nullptr, BAD_CAST "producer", nullptr);
    xmlNewProp(producer, BAD_CAST "id", BAD_CAST nodeId.c_str());
    xmlNewProp(producer, BAD_CAST "in", BAD_CAST "0");
    FrameIndex out = asset.info.lengthInSequenceFrames > 0 ? asset.info.lengthInSequenceFrames - 1 : 0;
    xmlNewProp(producer, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());

    writeMediaSource(producer, asset, projectDir);

    addProperty(producer, "ustudio:asset_id", std::to_string(asset.id.value));
    addProperty(producer, "ustudio:display_name", asset.displayName);
    addProperty(producer, "ustudio:folder", asset.folder);
    addProperty(producer, "ustudio:fingerprint", asset.fileFingerprint);
    addProperty(producer, "ustudio:proxy", asset.proxyPath);
    // Missing is what this run found on disk, not part of the project
    // (doc 07; the next open checks again).
    const Asset::Status status = asset.status == Asset::Status::Missing ? Asset::Status::Ready : asset.status;
    addProperty(producer, "ustudio:status", std::to_string(static_cast<int>(status)));

    const MediaInfo &info = asset.info;
    addProperty(producer, "ustudio:has_video", info.hasVideo ? "1" : "0");
    addProperty(producer, "ustudio:has_audio", info.hasAudio ? "1" : "0");
    addProperty(producer, "ustudio:width", std::to_string(info.width));
    addProperty(producer, "ustudio:height", std::to_string(info.height));
    addProperty(producer, "ustudio:fps_num", std::to_string(info.fps.num));
    addProperty(producer, "ustudio:fps_den", std::to_string(info.fps.den));
    addProperty(producer, "ustudio:sar_num", std::to_string(info.sar.num));
    addProperty(producer, "ustudio:sar_den", std::to_string(info.sar.den));
    addProperty(producer, "ustudio:audio_channels", std::to_string(info.audioChannels));
    addProperty(producer, "ustudio:sample_rate", std::to_string(info.sampleRate));
    addProperty(producer, "ustudio:native_duration", doubleToString(info.nativeDurationSeconds));
    addProperty(producer, "ustudio:video_codec", info.videoCodec);
    addProperty(producer, "ustudio:audio_codec", info.audioCodec);
    addProperty(producer, "ustudio:container", info.container);
    addProperty(producer, "ustudio:is_image_sequence", info.isImageSequence ? "1" : "0");
    if (info.isImageSequence)
        addProperty(producer, "ustudio:sequence_begin", std::to_string(info.sequenceBegin));
    addProperty(producer, "ustudio:is_still_image", info.isStillImage ? "1" : "0");
    if (info.hasAlpha)
        addProperty(producer, "ustudio:has_alpha", "1");
    // Read back directly (reader.cpp), not derived from the node's "out"
    // attribute below -- out=length-1 collapses both "boundless/unknown"
    // (length<=0) and "exactly 1 frame" to the same out="0", which is
    // ambiguous to invert.
    addProperty(producer, "ustudio:length_in_sequence_frames", std::to_string(info.lengthInSequenceFrames));
}

void writeClipEntry(xmlNodePtr playlist, const Clip &clip, const std::string &producerId)
{
    xmlNodePtr entry = xmlNewChild(playlist, nullptr, BAD_CAST "entry", nullptr);
    xmlNewProp(entry, BAD_CAST "producer", BAD_CAST producerId.c_str());
    xmlNewProp(entry, BAD_CAST "in", BAD_CAST std::to_string(clip.in).c_str());
    xmlNewProp(entry, BAD_CAST "out", BAD_CAST std::to_string(clip.out).c_str());

    addProperty(entry, "ustudio:clip_id", std::to_string(clip.id.value));
    // The track's own position, not derived from this entry's place in
    // the record playlist (blank/entry cumulative length): a dissolve
    // legitimately overlaps two clips, which a flat playlist can't
    // represent -- the reader reads this directly. The record playlist is
    // never played; writeRenderPlaylist() below writes what is.
    addProperty(entry, "ustudio:position", std::to_string(clip.position));
    addProperty(entry, "ustudio:name", clip.name);
    addProperty(entry, "ustudio:speed", doubleToString(clip.speed));
    addProperty(entry, "ustudio:video_enabled", clip.videoEnabled ? "1" : "0");
    addProperty(entry, "ustudio:audio_enabled", clip.audioEnabled ? "1" : "0");
    if (clip.fadeIn)
        addProperty(entry, "ustudio:fade_in", std::to_string(clip.fadeIn->length));
    if (clip.fadeOut)
        addProperty(entry, "ustudio:fade_out", std::to_string(clip.fadeOut->length));
    xml_detail::writeParams(entry, "ustudio:source_param.", clip.sourceParams);
    writeEffects(entry, clip.effects, 0, clip.length(), true);
    // ADR-018: the clip's transform, when it isn't the default; a keyed
    // value's keys go in "<name>.keyframes".
    if (const Transform &t = clip.transform.get(); !(t == Transform{})) {
        const char *bounds = t.bounds == Transform::Bounds::None      ? "none"
                             : t.bounds == Transform::Bounds::Stretch ? "stretch"
                                                                      : "fit";
        addProperty(entry, "ustudio:transform.bounds", bounds);
        const std::pair<const char *, const KeyframedValue *> values[] = {{"x", &t.x},
                                                                          {"y", &t.y},
                                                                          {"width", &t.width},
                                                                          {"height", &t.height},
                                                                          {"rotation", &t.rotation},
                                                                          {"crop_left", &t.cropLeft},
                                                                          {"crop_top", &t.cropTop},
                                                                          {"crop_right", &t.cropRight},
                                                                          {"crop_bottom", &t.cropBottom}};
        for (const auto &[name, value] : values) {
            if (value->value != 0.0)
                addProperty(entry, std::string("ustudio:transform.") + name, doubleToString(value->value));
            if (!value->keyframes.empty()) {
                addProperty(entry, std::string("ustudio:transform.") + name + ".keyframes",
                            xml_detail::encodeKeyframes(value->keyframes));
                xml_detail::requireFormatVersion(7); // an older build keeps the value, drops the keys
            }
        }
        if (t.flipH)
            addProperty(entry, "ustudio:transform.flip_h", "1");
        if (t.flipV)
            addProperty(entry, "ustudio:transform.flip_v", "1");
    }
}

void writeRecordPlaylist(xmlNodePtr mlt, const Model &model, const Track &track, const std::string &playlistId,
                         size_t visualIndex, const std::unordered_map<uint64_t, std::string> &producerIdByAsset)
{
    xmlNodePtr playlist = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(playlist, BAD_CAST "id", BAD_CAST playlistId.c_str());

    addProperty(playlist, "ustudio:track_id", std::to_string(track.id.value));
    // The model's Sequence::tracks order (visual, top-to-bottom) is not
    // recoverable from the MLT tractor's own bottom-to-top / audio-first
    // <track> ordering alone, and Model::operator== is order-sensitive on
    // it -- this is what the reader sorts by to reconstruct it exactly.
    addProperty(playlist, "ustudio:visual_index", std::to_string(visualIndex));
    addProperty(playlist, "ustudio:kind", track.kind == Track::Kind::Audio ? "audio" : "video");
    addProperty(playlist, "ustudio:name", track.name);
    addProperty(playlist, "ustudio:muted", track.muted ? "1" : "0");
    addProperty(playlist, "ustudio:hidden", track.hidden ? "1" : "0");
    addProperty(playlist, "ustudio:locked", track.locked ? "1" : "0");
    addProperty(playlist, "ustudio:volume", doubleToString(track.volume));
    writeEffects(playlist, track.effects, 0, std::max<FrameIndex>(model.sequence().length(), 1), true);

    FrameIndex cursor = 0;
    for (ClipId clipId : track.clips) {
        const Clip &clip = model.clip(clipId);
        // No <blank> when a dissolve transition makes this clip start
        // before the previous one ends (clip.position <= cursor) -- there
        // is no gap to fill; each clip's real position round-trips via
        // its own ustudio:position property regardless (see
        // writeClipEntry), not from this cumulative cursor.
        if (clip.position > cursor) {
            xmlNodePtr blank = xmlNewChild(playlist, nullptr, BAD_CAST "blank", nullptr);
            xmlNewProp(blank, BAD_CAST "length", BAD_CAST std::to_string(clip.position - cursor).c_str());
        }
        writeClipEntry(playlist, clip, producerIdByAsset.at(clip.asset.value));
        cursor = std::max(cursor, clip.end());
    }
}

// One cut of an asset inside a render playlist: an <entry> with the
// segment's own source range.
// With the clip's effects as native filters, animated for this cut
// (it starts in - clip.in frames into the clip).
xmlNodePtr writeRenderCut(xmlNodePtr playlist, const std::string &producerId, FrameIndex in, FrameIndex out,
                          const Clip &clip, const Model &model)
{
    xmlNodePtr entry = xmlNewChild(playlist, nullptr, BAD_CAST "entry", nullptr);
    xmlNewProp(entry, BAD_CAST "producer", BAD_CAST producerId.c_str());
    xmlNewProp(entry, BAD_CAST "in", BAD_CAST std::to_string(in).c_str());
    xmlNewProp(entry, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());
    writeEffects(entry, clip.effects, in - clip.in, out - in + 1, false, in);
    // After the effects, as EngineSync attaches them (ADR-018): the same
    // filters from the same function, so melt plays what the editor does.
    const MediaInfo &info = model.asset(clip.asset).info;
    for (const NativeFilter &native :
         transformFilters(clip.transform.get(), info.width, info.height, model.sequence().profile, 1.0, 1.0,
                          in - clip.in, out - in + 1)) {
        xmlNodePtr filter = xmlNewChild(entry, nullptr, BAD_CAST "filter", nullptr);
        xmlNewProp(filter, BAD_CAST "in", BAD_CAST std::to_string(in).c_str());
        xmlNewProp(filter, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());
        addProperty(filter, "mlt_service", native.service);
        for (const auto &[name, value] : native.properties)
            addProperty(filter, name, value);
    }
    return entry;
}

// A transition's filters on one of its cuts (a dip's brightness ramp),
// after everything writeRenderCut() put there, as EngineSync attaches them.
void writeTransitionFilters(xmlNodePtr entry, const std::vector<NativeFilter> &filters, FrameIndex in, FrameIndex out)
{
    for (const NativeFilter &native : filters) {
        xmlNodePtr filter = xmlNewChild(entry, nullptr, BAD_CAST "filter", nullptr);
        xmlNewProp(filter, BAD_CAST "in", BAD_CAST std::to_string(in).c_str());
        xmlNewProp(filter, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());
        addProperty(filter, "mlt_service", native.service);
        for (const auto &[name, value] : native.properties)
            addProperty(filter, name, value);
    }
}

// A wipe's map for melt: generated beside the project (ustudio-wipes/) and
// named relative to it, as media is; "" for an unknown name or a folder we
// can't write, which then renders as the plain dissolve, as it plays.
std::string lumaMapResource(const std::string &name, const fs::path &projectDir)
{
    const std::vector<std::string> &names = lumaMapNames();
    if (std::find(names.begin(), names.end(), name) == names.end())
        return {};
    const fs::path file = lumaMapPath(projectDir / "ustudio-wipes", name);
    if (!writeLumaMap(name, file)) {
        Log::warn("[xml] couldn't write the wipe map " + utf8String(file));
        return {};
    }
    return relativizePath(utf8String(file), projectDir);
}

// The producer a clip's cuts come from, mirroring EngineSync's
// masterProducerFor(): the asset's own <producer> when both streams are
// on, else a variant <producer> of the same media with video_index and/or
// audio_index = -1 ("off", per avformat's YAML). Set on the producer, not
// the entry: MLT ignores both on a cut (see masterProducerFor()). Variants
// carry no ustudio:asset_id, so the reader skips them. Emitted on first use,
// which writeRenderPlaylist() arranges to happen before any playlist
// references them.
class ProducerVariants
{
  public:
    ProducerVariants(xmlNodePtr mlt, const Model &model, const fs::path &projectDir,
                     const std::unordered_map<uint64_t, std::string> &producerIdByAsset)
        : m_mlt(mlt), m_model(model), m_projectDir(projectDir), m_producerIdByAsset(producerIdByAsset)
    {}

    std::string idFor(const Clip &clip)
    {
        const std::string &base = m_producerIdByAsset.at(clip.asset.value);
        if (clip.videoEnabled && clip.audioEnabled)
            return base;
        std::string id = base + (clip.videoEnabled ? "" : "_novideo") + (clip.audioEnabled ? "" : "_noaudio");
        if (m_written.insert(id).second) {
            const Asset &asset = m_model.asset(clip.asset);
            xmlNodePtr producer = xmlNewChild(m_mlt, nullptr, BAD_CAST "producer", nullptr);
            xmlNewProp(producer, BAD_CAST "id", BAD_CAST id.c_str());
            writeMediaSource(producer, asset, m_projectDir);
            if (!clip.videoEnabled)
                addProperty(producer, "video_index", "-1");
            if (!clip.audioEnabled)
                addProperty(producer, "audio_index", "-1");
        }
        return id;
    }

    const fs::path &projectDir() const
    {
        return m_projectDir;
    }

  private:
    xmlNodePtr m_mlt;
    const Model &m_model;
    fs::path m_projectDir;
    const std::unordered_map<uint64_t, std::string> &m_producerIdByAsset;
    std::set<std::string> m_written;
};

// The MLT form of one dissolve, mirroring EngineSync's
// buildTransitionSubTractor(): a 2-track tractor, `a`'s tail on track 0
// and `b`'s head on track 1, joined by the dissolve service (luma) and an
// audio crossfade (mix, start=-1), both with explicit in/out over the
// sub-tractor's local range (see that function for why both matter). A
// recipe's services come from the same core::nativeTransition(). The
// transition's model record rides on the video <transition> as ustudio:
// properties, which is where the reader looks for it.
std::string writeDissolveTractor(xmlNodePtr mlt, const Model &model, const TrackSegment &segment,
                                 ProducerVariants &producers, const fs::path &projectDir)
{
    const Transition &t = model.transition(segment.transition);
    const Clip &clipA = model.clip(segment.a);
    const Clip &clipB = model.clip(segment.b);
    std::string id = "dissolve_" + std::to_string(t.id.value);

    xmlNodePtr tailA = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(tailA, BAD_CAST "id", BAD_CAST(id + "_a").c_str());
    const NativeTransition native = nativeTransition(t);
    writeTransitionFilters(
        writeRenderCut(tailA, producers.idFor(clipA), clipA.out - t.length + 1, clipA.out, clipA, model),
        native.tailFilters, clipA.out - t.length + 1, clipA.out);

    xmlNodePtr headB = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(headB, BAD_CAST "id", BAD_CAST(id + "_b").c_str());
    writeTransitionFilters(
        writeRenderCut(headB, producers.idFor(clipB), clipB.in, clipB.in + t.length - 1, clipB, model),
        native.headFilters, clipB.in, clipB.in + t.length - 1);

    xmlNodePtr tractor = xmlNewChild(mlt, nullptr, BAD_CAST "tractor", nullptr);
    xmlNewProp(tractor, BAD_CAST "id", BAD_CAST id.c_str());
    xmlNewProp(tractor, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(tractor, BAD_CAST "out", BAD_CAST std::to_string(t.length - 1).c_str());
    xmlNodePtr trackA = xmlNewChild(tractor, nullptr, BAD_CAST "track", nullptr);
    xmlNewProp(trackA, BAD_CAST "producer", BAD_CAST(id + "_a").c_str());
    xmlNodePtr trackB = xmlNewChild(tractor, nullptr, BAD_CAST "track", nullptr);
    xmlNewProp(trackB, BAD_CAST "producer", BAD_CAST(id + "_b").c_str());

    xmlNodePtr luma = xmlNewChild(tractor, nullptr, BAD_CAST "transition", nullptr);
    xmlNewProp(luma, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(luma, BAD_CAST "out", BAD_CAST std::to_string(t.length - 1).c_str());
    addProperty(luma, "mlt_service", native.video.service);
    addProperty(luma, "a_track", "0");
    addProperty(luma, "b_track", "1");
    for (const auto &[name, value] : native.video.properties)
        addProperty(luma, name, value);
    if (!native.luma.empty() && native.video.service == "luma")
        if (std::string map = lumaMapResource(native.luma, projectDir); !map.empty())
            addProperty(luma, "resource", map);
    addProperty(luma, "ustudio:transition_id", std::to_string(t.id.value));
    addProperty(luma, "ustudio:transition_track", std::to_string(t.track.value));
    addProperty(luma, "ustudio:transition_a", std::to_string(t.a.value));
    addProperty(luma, "ustudio:transition_b", std::to_string(t.b.value));
    addProperty(luma, "ustudio:transition_extend_a", std::to_string(t.extendA));
    addProperty(luma, "ustudio:transition_extend_b", std::to_string(t.extendB));
    addProperty(luma, "ustudio:transition_service", t.service);
    if (!t.recipe.empty())
        addProperty(luma, "ustudio:transition_recipe", t.recipe);
    xml_detail::writeParams(luma, "ustudio:transition_param.", t.params);

    xmlNodePtr mix = xmlNewChild(tractor, nullptr, BAD_CAST "transition", nullptr);
    xmlNewProp(mix, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(mix, BAD_CAST "out", BAD_CAST std::to_string(t.length - 1).c_str());
    addProperty(mix, "mlt_service", native.audio.service);
    addProperty(mix, "a_track", "0");
    addProperty(mix, "b_track", "1");
    for (const auto &[name, value] : native.audio.properties)
        addProperty(mix, name, value);
    return id;
}

// The playlist the sequence tractor actually plays for one track: the same
// segments EngineSync::rebuildTrackPlaylist() appends (core::
// planTrackSegments(), shared), and the same track-level volume filter. No
// ustudio: properties -- the model lives in the record playlist.
void writeRenderPlaylist(xmlNodePtr mlt, const Model &model, const Track &track, const std::string &playlistId,
                         ProducerVariants &producers)
{
    // Everything this playlist references is written before it: MLT's xml
    // producer resolves ids as it parses, so forward references fail.
    std::vector<TrackSegment> segments = planTrackSegments(model, track);
    std::unordered_map<uint64_t, std::string> dissolveIdByTransition;
    std::unordered_map<uint64_t, std::string> producerIdByClip;
    for (const TrackSegment &segment : segments) {
        if (segment.kind == TrackSegment::Kind::Transition)
            dissolveIdByTransition.emplace(segment.transition.value,
                                           writeDissolveTractor(mlt, model, segment, producers, producers.projectDir()));
        else
            producerIdByClip.emplace(segment.clip.value, producers.idFor(model.clip(segment.clip)));
    }

    xmlNodePtr playlist = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(playlist, BAD_CAST "id", BAD_CAST playlistId.c_str());
    // Track mute/hide, as EngineSync sets it: MLT's per-track "hide".
    int hide = (track.hidden ? 1 : 0) | (track.muted ? 2 : 0);
    if (hide != 0)
        addProperty(playlist, "hide", std::to_string(hide));

    FrameIndex cursor = 0;
    for (const TrackSegment &segment : segments) {
        if (segment.start > cursor) {
            xmlNodePtr blank = xmlNewChild(playlist, nullptr, BAD_CAST "blank", nullptr);
            xmlNewProp(blank, BAD_CAST "length", BAD_CAST std::to_string(segment.start - cursor).c_str());
        }
        if (segment.kind == TrackSegment::Kind::Clip) {
            writeRenderCut(playlist, producerIdByClip.at(segment.clip.value), segment.in, segment.out,
                           model.clip(segment.clip), model);
        } else {
            xmlNodePtr entry = xmlNewChild(playlist, nullptr, BAD_CAST "entry", nullptr);
            xmlNewProp(entry, BAD_CAST "producer",
                       BAD_CAST dissolveIdByTransition.at(segment.transition.value).c_str());
            xmlNewProp(entry, BAD_CAST "in", BAD_CAST "0");
            xmlNewProp(entry, BAD_CAST "out", BAD_CAST std::to_string(segment.length - 1).c_str());
        }
        cursor = segment.start + segment.length;
    }

    if (track.volume != 1.0) {
        xmlNodePtr filter = xmlNewChild(playlist, nullptr, BAD_CAST "filter", nullptr);
        addProperty(filter, "mlt_service", "volume");
        addProperty(filter, "level", doubleToString(linearToDecibels(track.volume)));
    }
    writeEffects(playlist, track.effects, 0, std::max<FrameIndex>(model.sequence().length(), 1), false);
}

// Adjustment blocks (played from FX4) and looks, each a never-played
// playlist holding the record, like the record playlists.
void writeAdjustmentBlocksAndLooks(xmlNodePtr mlt, const Project &project, const Sequence &seq)
{
    for (const AdjustmentBlock &block : seq.adjustmentBlocks) {
        xmlNodePtr playlist = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
        xmlNewProp(playlist, BAD_CAST "id", BAD_CAST("adjustment_" + std::to_string(block.id.value)).c_str());
        addProperty(playlist, "ustudio:adjustment_block_id", std::to_string(block.id.value));
        addProperty(playlist, "ustudio:lane", std::to_string(block.lane));
        addProperty(playlist, "ustudio:start", std::to_string(block.start));
        addProperty(playlist, "ustudio:length", std::to_string(block.length));
        if (block.fadeIn)
            addProperty(playlist, "ustudio:fade_in", std::to_string(block.fadeIn->length));
        if (block.fadeOut)
            addProperty(playlist, "ustudio:fade_out", std::to_string(block.fadeOut->length));
        writeEffects(playlist, block.effects, 0, block.length, true);
    }
    for (const Look &look : project.looks) {
        xmlNodePtr playlist = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
        xmlNewProp(playlist, BAD_CAST "id", BAD_CAST("look_" + std::to_string(look.id.value)).c_str());
        addProperty(playlist, "ustudio:look_id", std::to_string(look.id.value));
        addProperty(playlist, "ustudio:look_name", look.name);
        writeEffects(playlist, look.effects, 0, 1, true);
    }
}

} // namespace

std::string saveProject(const Model &model, const std::string &path)
{
    // Files inside the project's folder are saved relative to it
    // (xml_detail::ProjectFolderScope); the render graph keeps them absolute.
    std::error_code folderEc;
    const xml_detail::ProjectFolderScope folder(std::filesystem::absolute(std::filesystem::path(path), folderEc).parent_path());
    const Project &project = model.project();
    const Sequence &seq = model.sequence();
    fs::path targetPath(path);
    fs::path projectDir = targetPath.parent_path();

    const xml_detail::FormatVersionScope formatVersion(kFormatVersion);
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr mlt = xmlNewNode(nullptr, BAD_CAST "mlt");
    xmlDocSetRootElement(doc, mlt);
    xmlNewProp(mlt, BAD_CAST "LC_NUMERIC", BAD_CAST "C");
    xmlNewProp(mlt, BAD_CAST "producer", BAD_CAST "main_bin");
    xmlNewProp(mlt, BAD_CAST "root", BAD_CAST projectDir.string().c_str());

    xmlNodePtr profileNode = xmlNewChild(mlt, nullptr, BAD_CAST "profile", nullptr);
    xmlNewProp(profileNode, BAD_CAST "description", BAD_CAST "ustudio");
    xmlNewProp(profileNode, BAD_CAST "width", BAD_CAST std::to_string(seq.profile.width).c_str());
    xmlNewProp(profileNode, BAD_CAST "height", BAD_CAST std::to_string(seq.profile.height).c_str());
    xmlNewProp(profileNode, BAD_CAST "frame_rate_num", BAD_CAST std::to_string(seq.profile.fps.num).c_str());
    xmlNewProp(profileNode, BAD_CAST "frame_rate_den", BAD_CAST std::to_string(seq.profile.fps.den).c_str());
    xmlNewProp(profileNode, BAD_CAST "sample_aspect_num", BAD_CAST std::to_string(seq.profile.sar.num).c_str());
    xmlNewProp(profileNode, BAD_CAST "sample_aspect_den", BAD_CAST std::to_string(seq.profile.sar.den).c_str());
    xmlNewProp(profileNode, BAD_CAST "display_aspect_num", BAD_CAST std::to_string(seq.profile.dar.num).c_str());
    xmlNewProp(profileNode, BAD_CAST "display_aspect_den", BAD_CAST std::to_string(seq.profile.dar.den).c_str());
    xmlNewProp(profileNode, BAD_CAST "progressive", BAD_CAST(seq.profile.progressive ? "1" : "0"));
    xmlNewProp(profileNode, BAD_CAST "colorspace", BAD_CAST std::to_string(seq.profile.colorspace).c_str());

    std::unordered_map<uint64_t, std::string> producerIdByAsset;
    xmlNodePtr mainBin = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(mainBin, BAD_CAST "id", BAD_CAST "main_bin");

    for (const Asset &asset : project.bin) {
        std::string nodeId = "asset" + std::to_string(asset.id.value);
        producerIdByAsset.emplace(asset.id.value, nodeId);
        writeAssetProducer(mlt, asset, projectDir, nodeId);

        FrameIndex out = asset.info.lengthInSequenceFrames > 0 ? asset.info.lengthInSequenceFrames - 1 : 0;
        xmlNodePtr entry = xmlNewChild(mainBin, nullptr, BAD_CAST "entry", nullptr);
        xmlNewProp(entry, BAD_CAST "producer", BAD_CAST nodeId.c_str());
        xmlNewProp(entry, BAD_CAST "in", BAD_CAST "0");
        xmlNewProp(entry, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());
    }

    std::unordered_map<uint64_t, size_t> visualIndexByTrack;
    for (size_t i = 0; i < seq.tracks.size(); ++i)
        visualIndexByTrack.emplace(seq.tracks[i].id.value, i);

    std::vector<TrackId> order = mltTrackOrder(seq);
    std::unordered_map<uint64_t, std::string> playlistIdByTrack;
    ProducerVariants producers(mlt, model, projectDir, producerIdByAsset);
    for (TrackId trackId : order) {
        const Track &track = model.track(trackId);
        std::string playlistId = "track_" + std::to_string(trackId.value);
        playlistIdByTrack.emplace(trackId.value, playlistId);
        // Record first, render second: the record playlist is never
        // referenced by the tractor, so MLT loads it standalone and ignores
        // it (same as main_bin), while the reader only reads it.
        writeRecordPlaylist(mlt, model, track, "record_" + std::to_string(trackId.value),
                            visualIndexByTrack.at(trackId.value), producerIdByAsset);
        writeRenderPlaylist(mlt, model, track, playlistId, producers);
    }

    writeAdjustmentBlocksAndLooks(mlt, project, seq);

    xmlNodePtr blackProducer = xmlNewChild(mlt, nullptr, BAD_CAST "producer", nullptr);
    xmlNewProp(blackProducer, BAD_CAST "id", BAD_CAST "black");
    FrameIndex sequenceLength = std::max<FrameIndex>(seq.length(), 1);
    xmlNewProp(blackProducer, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(blackProducer, BAD_CAST "out", BAD_CAST std::to_string(sequenceLength - 1).c_str());
    // As writeMediaSource(): a bare "color:…" resource loads as black
    // whatever the colour (it was right for black only by accident).
    addProperty(blackProducer, "mlt_service", "color");
    addProperty(blackProducer, "resource", backgroundResource(seq.background));
    // melt can't run EngineSync's re-tag filter on the background; as RGBA
    // it converts with the profile's matrix, the same pixels (see above).
    addProperty(blackProducer, "mlt_image_format", "rgba");

    // One layer's tracks, composites and mixes, as EngineSync::rebuildAll()
    // plants them, so `melt`/u-studio-render (no editor) sees the same
    // graph our own playback does. See EngineSync's class comment for why
    // every track has both, rather than doc 05's video/audio-split graph.
    auto writeLayer = [&](xmlNodePtr layer, const std::string &bottom, const std::vector<TrackId> &trackIds) {
        xmlNodePtr bottomTrack = xmlNewChild(layer, nullptr, BAD_CAST "track", nullptr);
        xmlNewProp(bottomTrack, BAD_CAST "producer", BAD_CAST bottom.c_str());
        for (TrackId trackId : trackIds) {
            xmlNodePtr trackNode = xmlNewChild(layer, nullptr, BAD_CAST "track", nullptr);
            xmlNewProp(trackNode, BAD_CAST "producer", BAD_CAST playlistIdByTrack.at(trackId.value).c_str());
        }
        for (size_t index = 1; index <= trackIds.size(); ++index) {
            xmlNodePtr composite = xmlNewChild(layer, nullptr, BAD_CAST "transition", nullptr);
            addProperty(composite, "mlt_service", "composite");
            addProperty(composite, "a_track", "0"); // onto the background, as EngineSync (alpha; ADR-018)
            addProperty(composite, "fill", "1");    // as EngineSync: the code's default is 0
            addProperty(composite, "b_track", std::to_string(index));

            xmlNodePtr mix = xmlNewChild(layer, nullptr, BAD_CAST "transition", nullptr);
            addProperty(mix, "mlt_service", "mix");
            addProperty(mix, "a_track", std::to_string(index - 1));
            addProperty(mix, "b_track", std::to_string(index));
            addProperty(mix, "start", "1");
            addProperty(mix, "sum", "1");
            addProperty(mix, "always_active", "1");
        }
    };
    // A lane's adjustment blocks on its layer, each for its own frames (the
    // engine's effects drop-in does the same through decorateLane()).
    auto writeLaneBlocks = [&](xmlNodePtr layer, int lane) {
        for (const AdjustmentBlock &block : seq.adjustmentBlocks)
            if (block.lane == lane)
                writeEffects(layer, blockEffects(block), 0, block.length, false, block.start); // fades folded in, as the engine plays it
    };

    // FX4: adjustment lanes below the first row are sub-tractors, deepest
    // first (MLT's xml producer needs a producer before its use), each the
    // next layer's track 0 (core::adjustmentLayers(), shared with
    // EngineSync).
    const AdjustmentLayers layers = adjustmentLayers(seq);
    std::string bottom = "black";
    for (size_t i = layers.lanes.size(); i-- > 0;) {
        const std::string id = "lane_" + std::to_string(layers.lanes[i]);
        xmlNodePtr group = xmlNewChild(mlt, nullptr, BAD_CAST "tractor", nullptr);
        xmlNewProp(group, BAD_CAST "id", BAD_CAST id.c_str());
        xmlNewProp(group, BAD_CAST "in", BAD_CAST "0");
        xmlNewProp(group, BAD_CAST "out", BAD_CAST std::to_string(sequenceLength - 1).c_str());
        addProperty(group, "ustudio.lane", std::to_string(layers.lanes[i]));
        writeLayer(group, bottom, layers.layerTracks[i + 1]);
        writeLaneBlocks(group, layers.lanes[i]);
        bottom = id;
    }

    xmlNodePtr tractor = xmlNewChild(mlt, nullptr, BAD_CAST "tractor", nullptr);
    xmlNewProp(tractor, BAD_CAST "id", BAD_CAST("seq" + std::to_string(seq.id.value)).c_str());
    xmlNewProp(tractor, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(tractor, BAD_CAST "out", BAD_CAST std::to_string(sequenceLength - 1).c_str());

    xmlNodePtr versionNode = addProperty(tractor, "ustudio:format_version", std::to_string(kFormatVersion));
    // Read back only to tell a user of an older build which version to
    // update to (reader.h, ProjectLoadError::savedBy).
    addProperty(tractor, "ustudio:saved_by", USTUDIO_VERSION);
    addProperty(tractor, "ustudio:sequence_id", std::to_string(seq.id.value));
    addProperty(tractor, "ustudio:sequence_name", seq.name);
    addProperty(tractor, "ustudio:active_sequence", std::to_string(project.activeSequence.value));
    addProperty(tractor, "ustudio:next_id", std::to_string(project.nextId));
    addProperty(tractor, "ustudio:mlt_profile_name", seq.profile.mltName);
    addProperty(tractor, "ustudio:markers", markersToJson(seq.markers));
    addProperty(tractor, "ustudio:background", backgroundHex(seq.background));
    addProperty(tractor, "ustudio:settings", settingsToJson(project.settings));

    writeLayer(tractor, bottom, layers.layerTracks[0]);
    // Master effects on the output (Sequence::effects).
    writeEffects(tractor, seq.effects, 0, sequenceLength, true);
    // Then lane 0's blocks, after them as in EngineSync (decorateTractor(),
    // then decorateLane()).
    writeLaneBlocks(tractor, 0);

    // Set last: every record has had its say (requireFormatVersion()).
    xmlNodeSetContent(versionNode, BAD_CAST std::to_string(formatVersion.needed()).c_str());
    std::string tmpPath = path + ".tmp";
    int written = xmlSaveFormatFileEnc(tmpPath.c_str(), doc, "UTF-8", 1);
    xmlFreeDoc(doc);
    if (written < 0)
        return "failed to write " + tmpPath;

    int fd = ::open(tmpPath.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }

    std::error_code ec;
    fs::rename(tmpPath, targetPath, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        return "failed to replace " + path + " (" + ec.message() + ")";
    }

    return {};
}

} // namespace ustudio::core

#pragma GCC diagnostic pop
