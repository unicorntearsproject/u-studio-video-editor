#include "reader.h"

#include "core/xml/effect_io.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <tuple>
#include <unordered_map>

// See writer.cpp's identical comment: BAD_CAST is libxml2's C-style cast,
// used throughout this file; the pragma push/pop works here because it
// wraps our own function bodies, not an #include block.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::core {

namespace {

namespace fs = std::filesystem;

constexpr int kFormatVersion = 7; // the newest writer.cpp may write
// Oldest version this reader still opens. Format 3 differs from 4 only in
// where the render structure and dissolve metadata live (see writer.cpp);
// the record playlists and every ustudio: property it reads are identical.
// 5 only adds (effects, source parameters, recipes, adjustment blocks,
// looks), which an older file simply doesn't have.
constexpr int kOldestReadableFormatVersion = 3;

std::string attr(xmlNodePtr node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    std::string result = value ? reinterpret_cast<const char *>(value) : "";
    if (value)
        xmlFree(value);
    return result;
}

std::optional<std::string> getProperty(xmlNodePtr node, const std::string &name)
{
    for (xmlNodePtr child = node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE || xmlStrcmp(child->name, BAD_CAST "property") != 0)
            continue;
        xmlChar *propName = xmlGetProp(child, BAD_CAST "name");
        if (!propName)
            continue;
        bool matches = name == reinterpret_cast<const char *>(propName);
        xmlFree(propName);
        if (!matches)
            continue;

        xmlChar *content = xmlNodeGetContent(child);
        std::string value = content ? reinterpret_cast<const char *>(content) : "";
        if (content)
            xmlFree(content);
        return value;
    }
    return std::nullopt;
}

std::string prop(xmlNodePtr node, const std::string &name, const std::string &fallback = "")
{
    return getProperty(node, name).value_or(fallback);
}

int64_t toI64(const std::string &s)
{
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}

uint64_t toU64(const std::string &s)
{
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

// std::strtod consults the process's C locale (LC_NUMERIC), which can be
// non-"C" if anything in the process calls setlocale(LC_ALL, "") (GTK/GLib
// i18n init does) -- exactly the class of bug the writer's own
// LC_NUMERIC="C" root attribute is there to avoid on the write side.
// std::from_chars for floating point is specified to be locale-independent
// (always "C"-like), so it's the correct parse here, not g_ascii_strtod
// (which would pull GLib into core/, against the layer boundary).
double toDouble(const std::string &s)
{
    double value = 0.0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    return result.ec == std::errc{} ? value : 0.0;
}

bool toBool(const std::string &s)
{
    return s == "1";
}

xmlNodePtr firstChildNamed(xmlNodePtr parent, const char *name)
{
    for (xmlNodePtr child = parent->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, BAD_CAST name) == 0)
            return child;
    }
    return nullptr;
}

// Inverse of writer.cpp's relativizePath(): a stored resource that starts
// with '/' is already absolute; one shaped like "service:arg" (a colon
// before any slash -- color:/noise:/tone: generators) is left as-is,
// since it was never a filesystem path; anything else is a path relative
// to the project file's directory.
std::string resolveResource(const std::string &stored, const fs::path &projectDir)
{
    if (stored.empty() || stored.front() == '/')
        return stored;

    size_t colon = stored.find(':');
    size_t slash = stored.find('/');
    bool looksLikeShorthand = colon != std::string::npos && (slash == std::string::npos || colon < slash);
    if (looksLikeShorthand)
        return stored;

    std::error_code ec;
    fs::path resolved = (projectDir / stored).lexically_normal();
    return ec ? stored : resolved.string();
}

// Matches writer.cpp's markersToJson()/settingsToJson() exactly -- not a
// general JSON parser, just the inverse of that specific hand-rolled
// encoding (a flat array of {id,at,text,color} objects; a flat
// string-to-string object).
std::string jsonUnescape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            switch (next) {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            default:
                out += next;
            }
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Extracts the content of a JSON string literal starting at `pos`
// (pointing at the opening quote); returns the unescaped value and
// advances `pos` past the closing quote.
std::string readJsonString(const std::string &json, size_t &pos)
{
    ++pos; // skip opening quote
    std::string raw;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            raw += json[pos];
            raw += json[pos + 1];
            pos += 2;
        } else {
            raw += json[pos];
            ++pos;
        }
    }
    // Untrusted input (CLAUDE.md): an unterminated string in a hand-edited
    // or corrupted project file means the loop above ran off the end
    // without finding the closing quote, leaving pos == json.size()
    // already. Unconditionally advancing past that would push pos PAST
    // size() -- not the one-past-the-end position std::string::operator[]
    // tolerates, a true out-of-bounds index the very next dereference
    // anywhere in this file would read (audit C3).
    if (pos < json.size())
        ++pos; // skip closing quote
    return jsonUnescape(raw);
}

std::vector<Marker> parseMarkersJson(const std::string &json)
{
    std::vector<Marker> markers;
    size_t pos = 0;
    while (pos < json.size() && json[pos] != '[')
        ++pos;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
            ++pos;
        if (pos >= json.size() || json[pos] == ']')
            break;
        if (json[pos] != '{') {
            ++pos;
            continue;
        }
        ++pos; // skip '{'
        Marker marker;
        while (pos < json.size() && json[pos] != '}') {
            while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
                ++pos;
            if (pos >= json.size() || json[pos] == '}')
                break;
            std::string key = readJsonString(json, pos);
            while (pos < json.size() && json[pos] != ':')
                ++pos;
            // Same "malformed input, colon never found" case as
            // readJsonString above -- guard both the skip and the read
            // that follows it (audit C3).
            if (pos < json.size())
                ++pos; // skip ':'
            while (pos < json.size() && json[pos] == ' ')
                ++pos;

            std::string value;
            if (pos < json.size() && json[pos] == '"') {
                value = readJsonString(json, pos);
            } else {
                size_t start = pos;
                while (pos < json.size() && json[pos] != ',' && json[pos] != '}')
                    ++pos;
                value = json.substr(start, pos - start);
            }

            if (key == "id")
                marker.id = MarkerId{toU64(value)};
            else if (key == "at")
                marker.at = toI64(value);
            else if (key == "text")
                marker.text = value;
            else if (key == "color")
                marker.color = static_cast<uint8_t>(toI64(value));
        }
        ++pos; // skip '}'
        markers.push_back(std::move(marker));
    }
    return markers;
}

std::map<std::string, std::string> parseSettingsJson(const std::string &json)
{
    std::map<std::string, std::string> settings;
    size_t pos = 0;
    while (pos < json.size() && json[pos] != '{')
        ++pos;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
            ++pos;
        if (pos >= json.size() || json[pos] == '}')
            break;
        std::string key = readJsonString(json, pos);
        while (pos < json.size() && json[pos] != ':')
            ++pos;
        ++pos;
        while (pos < json.size() && json[pos] == ' ')
            ++pos;
        std::string value = readJsonString(json, pos);
        settings.emplace(std::move(key), std::move(value));
    }
    return settings;
}

} // namespace

int oldestReadableProjectFormat()
{
    return kOldestReadableFormatVersion;
}

int newestReadableProjectFormat()
{
    return kFormatVersion;
}

std::expected<Model, std::string> loadProject(const std::string &path)
{
    std::expected<Model, ProjectLoadError> loaded = loadProjectFile(path);
    if (!loaded)
        return std::unexpected(loaded.error().message);
    return std::move(*loaded);
}

std::expected<Model, ProjectLoadError> loadProjectFile(const std::string &path)
{
    using Kind = ProjectLoadError::Kind;
    auto fail = [](Kind kind, std::string message, int formatVersion = 0, std::string savedBy = {}) {
        return std::unexpected(ProjectLoadError{kind, std::move(message), formatVersion, std::move(savedBy)});
    };
    std::error_code existsEc;
    if (!fs::exists(fs::path(path), existsEc))
        return fail(Kind::Missing, path + ": no such file");
    {
        // The first prototype's projects (before 2026-09-17) were INI files
        // starting "[Project]"; this version doesn't open them (owner,
        // 2026-10-08). Said plainly, not as "failed to parse".
        std::ifstream head(fs::path(path), std::ios::binary);
        if (!head)
            return fail(Kind::Unreadable, path + ": can't be read (permissions?)");
        std::string start(64, '\0');
        head.read(start.data(), static_cast<std::streamsize>(start.size()));
        start.resize(static_cast<size_t>(head.gcount()));
        const size_t first = start.find_first_not_of(" \r\n");
        if (first != std::string::npos && start.compare(first, 9, "[Project]") == 0)
            return fail(Kind::NotAProject, path +
                                               ": a project from the first prototype editor, which this version can't "
                                               "open");
    }
    // Files named relative to the project (a LUT in its luts folder) are
    // read against the folder it's in now (xml_detail::ProjectFolderScope).
    std::error_code folderEc;
    const xml_detail::ProjectFolderScope folder(
        std::filesystem::absolute(std::filesystem::path(path), folderEc).parent_path());
    xmlDocPtr doc = xmlReadFile(path.c_str(), nullptr, XML_PARSE_NOBLANKS);
    if (!doc)
        return fail(Kind::Unreadable, "failed to parse " + path + " (not XML)");

    xmlNodePtr mlt = xmlDocGetRootElement(doc);
    if (!mlt || xmlStrcmp(mlt->name, BAD_CAST "mlt") != 0) {
        xmlFreeDoc(doc);
        return fail(Kind::NotAProject, path + ": not an MLT XML file");
    }

    // The sequence tractor is the one carrying ustudio:format_version. From
    // format 4, each dissolve's own sub-tractor is also a top-level
    // <tractor>, written before it, so "first <tractor>" is not enough.
    // Falls back to the first one so a foreign file still gets the specific
    // "not a ustudio project" error below.
    xmlNodePtr tractor = nullptr;
    for (xmlNodePtr node = mlt->children; node && !tractor; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST "tractor") == 0 &&
            getProperty(node, "ustudio:format_version"))
            tractor = node;
    }
    if (!tractor)
        tractor = firstChildNamed(mlt, "tractor");
    if (!tractor) {
        xmlFreeDoc(doc);
        return fail(Kind::NotAProject, path + ": no <tractor> element");
    }

    std::optional<std::string> formatVersionStr = getProperty(tractor, "ustudio:format_version");
    if (!formatVersionStr) {
        xmlFreeDoc(doc);
        return fail(Kind::NotAProject, path + ": not a ustudio project (no ustudio:format_version)");
    }
    int formatVersion = static_cast<int>(toI64(*formatVersionStr));
    if (formatVersion < kOldestReadableFormatVersion || formatVersion > kFormatVersion) {
        const std::string savedBy = prop(tractor, "ustudio:saved_by");
        xmlFreeDoc(doc);
        return fail(formatVersion > kFormatVersion ? Kind::TooNew : Kind::TooOld,
                    path + ": ustudio:format_version " + std::to_string(formatVersion) +
                        (savedBy.empty() ? std::string() : " (saved by " + savedBy + ")") + "; this build reads " +
                        std::to_string(kOldestReadableFormatVersion) + " to " + std::to_string(kFormatVersion),
                    formatVersion, savedBy);
    }

    fs::path projectDir = fs::path(path).parent_path();

    Sequence seq;
    seq.id = SequenceId{toU64(prop(tractor, "ustudio:sequence_id", "1"))};
    seq.name = prop(tractor, "ustudio:sequence_name");
    seq.markers = parseMarkersJson(prop(tractor, "ustudio:markers", "[]"));
    // Absent before 0.54 (black); anything unreadable is black too.
    seq.background =
        parseBackgroundHex(prop(tractor, "ustudio:background", "#000000")).value_or(Sequence::kDefaultBackground);

    if (xmlNodePtr profileNode = firstChildNamed(mlt, "profile")) {
        seq.profile.width = static_cast<int>(toI64(attr(profileNode, "width")));
        seq.profile.height = static_cast<int>(toI64(attr(profileNode, "height")));
        seq.profile.fps.num = static_cast<int32_t>(toI64(attr(profileNode, "frame_rate_num")));
        seq.profile.fps.den = static_cast<int32_t>(toI64(attr(profileNode, "frame_rate_den")));
        seq.profile.sar.num = static_cast<int32_t>(toI64(attr(profileNode, "sample_aspect_num")));
        seq.profile.sar.den = static_cast<int32_t>(toI64(attr(profileNode, "sample_aspect_den")));
        seq.profile.dar.num = static_cast<int32_t>(toI64(attr(profileNode, "display_aspect_num")));
        seq.profile.dar.den = static_cast<int32_t>(toI64(attr(profileNode, "display_aspect_den")));
        seq.profile.progressive = attr(profileNode, "progressive") == "1";
        seq.profile.colorspace = static_cast<int>(toI64(attr(profileNode, "colorspace")));
    }
    seq.profile.mltName = prop(tractor, "ustudio:mlt_profile_name");

    // Untrusted input (CLAUDE.md, audit C3): a hand-edited or foreign-tool
    // <profile> with frame_rate_den="0" (or a negative/zero numerator)
    // would later reach Mlt::Profile::set_frame_rate() and every fps-based
    // FrameIndex<->time conversion in the app; fail the load cleanly here
    // instead of dividing by zero somewhere downstream.
    if (seq.profile.fps.num <= 0 || seq.profile.fps.den <= 0) {
        xmlFreeDoc(doc);
        return fail(Kind::Invalid, path + ": invalid <profile> frame rate (frame_rate_num/frame_rate_den must be "
                                          "positive)");
    }

    Project project;
    project.activeSequence = SequenceId{toU64(prop(tractor, "ustudio:active_sequence", "1"))};
    project.nextId = toU64(prop(tractor, "ustudio:next_id", "1"));
    project.settings = parseSettingsJson(prop(tractor, "ustudio:settings", "{}"));

    // Asset producers: any <producer> with an ustudio:asset_id property
    // (the "black" backing producer and any future non-asset producer
    // have none, so this filters them out naturally).
    std::unordered_map<std::string, AssetId> assetIdByNodeId;
    for (xmlNodePtr node = mlt->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "producer") != 0)
            continue;
        std::optional<std::string> assetIdStr = getProperty(node, "ustudio:asset_id");
        if (!assetIdStr)
            continue;

        Asset asset;
        asset.id = AssetId{toU64(*assetIdStr)};
        // ustudio:path (format 4) holds a generator shorthand verbatim; the
        // MLT-facing mlt_service/resource pair beside it is for melt.
        if (std::optional<std::string> generatorPath = getProperty(node, "ustudio:path"))
            asset.path = *generatorPath;
        else
            asset.path = resolveResource(prop(node, "resource"), projectDir);
        asset.displayName = prop(node, "ustudio:display_name");
        asset.folder = prop(node, "ustudio:folder");
        asset.fileFingerprint = prop(node, "ustudio:fingerprint");
        asset.proxyPath = prop(node, "ustudio:proxy");
        asset.status = static_cast<Asset::Status>(toI64(prop(node, "ustudio:status", "0")));

        MediaInfo &info = asset.info;
        info.hasVideo = toBool(prop(node, "ustudio:has_video"));
        info.hasAudio = toBool(prop(node, "ustudio:has_audio"));
        info.width = static_cast<int>(toI64(prop(node, "ustudio:width")));
        info.height = static_cast<int>(toI64(prop(node, "ustudio:height")));
        info.fps.num = static_cast<int32_t>(toI64(prop(node, "ustudio:fps_num", "1")));
        info.fps.den = static_cast<int32_t>(toI64(prop(node, "ustudio:fps_den", "1")));
        info.sar.num = static_cast<int32_t>(toI64(prop(node, "ustudio:sar_num", "1")));
        info.sar.den = static_cast<int32_t>(toI64(prop(node, "ustudio:sar_den", "1")));
        info.audioChannels = static_cast<int>(toI64(prop(node, "ustudio:audio_channels")));
        info.sampleRate = static_cast<int>(toI64(prop(node, "ustudio:sample_rate")));
        info.nativeDurationSeconds = toDouble(prop(node, "ustudio:native_duration"));
        info.videoCodec = prop(node, "ustudio:video_codec");
        info.audioCodec = prop(node, "ustudio:audio_codec");
        info.container = prop(node, "ustudio:container");
        info.isImageSequence = toBool(prop(node, "ustudio:is_image_sequence"));
        info.sequenceBegin = static_cast<int>(toI64(prop(node, "ustudio:sequence_begin")));
        info.isStillImage = toBool(prop(node, "ustudio:is_still_image"));
        info.hasAlpha = toBool(prop(node, "ustudio:has_alpha", "0")); // absent before 0.64.1: none
        // Read the length directly rather than deriving it from the node's
        // "out" attribute: the writer collapses both "boundless/unknown"
        // and "exactly 1 frame long" to out="0" (there's no way to tell
        // them apart from out alone), so a real 1-frame asset would
        // round-trip as boundless if this derived length from out.
        info.lengthInSequenceFrames = toI64(prop(node, "ustudio:length_in_sequence_frames", "0"));

        assetIdByNodeId.emplace(attr(node, "id"), asset.id);
        project.bin.push_back(std::move(asset));
    }

    // Track playlists: any <playlist> with an ustudio:track_id property
    // (main_bin, the bin-keeper playlist, has none).
    struct ParsedTrack
    {
        size_t visualIndex;
        Track track;
    };
    std::vector<ParsedTrack> parsedTracks;

    for (xmlNodePtr node = mlt->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "playlist") != 0)
            continue;
        std::optional<std::string> trackIdStr = getProperty(node, "ustudio:track_id");
        if (!trackIdStr)
            continue;

        Track track;
        track.id = TrackId{toU64(*trackIdStr)};
        track.kind = prop(node, "ustudio:kind") == "audio" ? Track::Kind::Audio : Track::Kind::Video;
        track.name = prop(node, "ustudio:name");
        track.muted = toBool(prop(node, "ustudio:muted"));
        track.hidden = toBool(prop(node, "ustudio:hidden"));
        track.locked = toBool(prop(node, "ustudio:locked"));
        track.volume = toDouble(prop(node, "ustudio:volume", "1"));
        size_t visualIndex = static_cast<size_t>(toI64(prop(node, "ustudio:visual_index")));
        track.effects = xml_detail::readEffectFilters(node);

        for (xmlNodePtr entryNode = node->children; entryNode; entryNode = entryNode->next) {
            if (entryNode->type != XML_ELEMENT_NODE)
                continue;
            if (xmlStrcmp(entryNode->name, BAD_CAST "blank") == 0)
                continue; // decorative only -- each entry's own ustudio:position is authoritative
            if (xmlStrcmp(entryNode->name, BAD_CAST "entry") != 0)
                continue;

            std::string producerNodeId = attr(entryNode, "producer");
            auto assetIt = assetIdByNodeId.find(producerNodeId);
            if (assetIt == assetIdByNodeId.end()) {
                // Untrusted input (CLAUDE.md): a hand-edited, truncated, or
                // foreign project file can reference a producer id that was
                // never registered as an asset (or wasn't, because it's
                // missing its ustudio:asset_id property). Fail the load
                // cleanly instead of letting an unordered_map::at() throw
                // past this function's std::expected contract.
                xmlFreeDoc(doc);
                return fail(Kind::Invalid, path + ": clip entry references unknown producer '" + producerNodeId + "'");
            }

            Clip clip;
            clip.id = ClipId{toU64(prop(entryNode, "ustudio:clip_id"))};
            clip.track = track.id;
            clip.asset = assetIt->second;
            clip.position = toI64(prop(entryNode, "ustudio:position"));
            clip.in = toI64(attr(entryNode, "in"));
            clip.out = toI64(attr(entryNode, "out"));
            clip.name = prop(entryNode, "ustudio:name");
            clip.speed = toDouble(prop(entryNode, "ustudio:speed", "1"));
            clip.videoEnabled = toBool(prop(entryNode, "ustudio:video_enabled", "1"));
            clip.audioEnabled = toBool(prop(entryNode, "ustudio:audio_enabled", "1"));
            if (std::optional<std::string> fadeIn = getProperty(entryNode, "ustudio:fade_in"))
                clip.fadeIn = FadeSpec{toI64(*fadeIn)};
            if (std::optional<std::string> fadeOut = getProperty(entryNode, "ustudio:fade_out"))
                clip.fadeOut = FadeSpec{toI64(*fadeOut)};
            clip.sourceParams = xml_detail::readParams(entryNode, "ustudio:source_param.");
            clip.effects = xml_detail::readEffectFilters(entryNode);
            // ADR-018; absent (format 5 and older, or the default): Fit.
            if (std::optional<std::string> bounds = getProperty(entryNode, "ustudio:transform.bounds")) {
                Transform t;
                t.bounds = *bounds == "none"      ? Transform::Bounds::None
                           : *bounds == "stretch" ? Transform::Bounds::Stretch
                                                  : Transform::Bounds::Fit;
                const std::pair<const char *, KeyframedValue *> values[] = {{"x", &t.x},
                                                                            {"y", &t.y},
                                                                            {"width", &t.width},
                                                                            {"height", &t.height},
                                                                            {"rotation", &t.rotation},
                                                                            {"crop_left", &t.cropLeft},
                                                                            {"crop_top", &t.cropTop},
                                                                            {"crop_right", &t.cropRight},
                                                                            {"crop_bottom", &t.cropBottom}};
                for (const auto &[name, value] : values) {
                    value->value = toDouble(prop(entryNode, std::string("ustudio:transform.") + name, "0"));
                    if (std::optional<std::string> keys =
                            getProperty(entryNode, std::string("ustudio:transform.") + name + ".keyframes"))
                        value->keyframes = xml_detail::decodeKeyframes(*keys);
                }
                t.flipH = toBool(prop(entryNode, "ustudio:transform.flip_h", "0"));
                t.flipV = toBool(prop(entryNode, "ustudio:transform.flip_v", "0"));
                clip.transform.set(std::move(t));
            }

            track.clips.push_back(clip.id);
            seq.clips.emplace(clip.id, std::move(clip));
        }

        parsedTracks.push_back({visualIndex, std::move(track)});
    }

    std::sort(parsedTracks.begin(), parsedTracks.end(),
              [](const ParsedTrack &a, const ParsedTrack &b) { return a.visualIndex < b.visualIndex; });
    for (ParsedTrack &parsed : parsedTracks)
        seq.tracks.push_back(std::move(parsed.track));

    // Dissolve transitions: any <transition> with an ustudio:transition_id
    // property, in any top-level <tractor>. Format 4 keeps them on the luma
    // <transition> inside each dissolve's own sub-tractor (writer.cpp's
    // writeDissolveTractor); format 3 kept them as bare children of the
    // sequence tractor -- scanning every tractor covers both. The
    // composite/mix <transition> elements EngineSync regenerates have no
    // such property, so this filters them out naturally, same pattern as
    // the asset-producer filter above. model.check() below is what actually
    // validates `a`/`b` resolve to real clips on `track` and that
    // extendA+extendB==length; a hand-edited or truncated file that gets
    // this wrong fails the load there rather than here.
    for (xmlNodePtr tractorNode = mlt->children; tractorNode; tractorNode = tractorNode->next) {
        if (tractorNode->type != XML_ELEMENT_NODE || xmlStrcmp(tractorNode->name, BAD_CAST "tractor") != 0)
            continue;
        for (xmlNodePtr node = tractorNode->children; node; node = node->next) {
            if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "transition") != 0)
                continue;
            std::optional<std::string> transitionIdStr = getProperty(node, "ustudio:transition_id");
            if (!transitionIdStr)
                continue;

            Transition t;
            t.id = TransitionId{toU64(*transitionIdStr)};
            t.track = TrackId{toU64(prop(node, "ustudio:transition_track"))};
            t.a = ClipId{toU64(prop(node, "ustudio:transition_a"))};
            t.b = ClipId{toU64(prop(node, "ustudio:transition_b"))};
            t.extendA = toI64(prop(node, "ustudio:transition_extend_a"));
            t.extendB = toI64(prop(node, "ustudio:transition_extend_b"));
            t.length = t.extendA + t.extendB;
            t.service = prop(node, "ustudio:transition_service", "luma");
            t.recipe = prop(node, "ustudio:transition_recipe");
            t.params = xml_detail::readParams(node, "ustudio:transition_param.");
            seq.transitions.push_back(std::move(t));
        }
    }

    // Master effects (on the sequence tractor), adjustment blocks and looks
    // (never-played playlists; writer.cpp's writeAdjustmentBlocksAndLooks).
    seq.effects = xml_detail::readEffectFilters(tractor);
    for (xmlNodePtr node = mlt->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "playlist") != 0)
            continue;
        if (std::optional<std::string> blockId = getProperty(node, "ustudio:adjustment_block_id")) {
            AdjustmentBlock block;
            block.id = AdjustmentBlockId{toU64(*blockId)};
            block.lane = static_cast<int>(toI64(prop(node, "ustudio:lane")));
            block.start = toI64(prop(node, "ustudio:start"));
            block.length = toI64(prop(node, "ustudio:length"));
            if (std::optional<std::string> fadeIn = getProperty(node, "ustudio:fade_in"))
                block.fadeIn = FadeSpec{toI64(*fadeIn)};
            if (std::optional<std::string> fadeOut = getProperty(node, "ustudio:fade_out"))
                block.fadeOut = FadeSpec{toI64(*fadeOut)};
            block.effects = xml_detail::readEffectFilters(node);
            seq.adjustmentBlocks.push_back(std::move(block));
        } else if (std::optional<std::string> lookId = getProperty(node, "ustudio:look_id")) {
            Look look;
            look.id = LookId{toU64(*lookId)};
            look.name = prop(node, "ustudio:look_name");
            look.effects = xml_detail::readEffectFilters(node);
            project.looks.push_back(std::move(look));
        }
    }
    // Model's own order for blocks: lane, start, id.
    std::sort(seq.adjustmentBlocks.begin(), seq.adjustmentBlocks.end(),
              [](const AdjustmentBlock &x, const AdjustmentBlock &y) {
                  return std::tie(x.lane, x.start, x.id.value) < std::tie(y.lane, y.start, y.id.value);
              });

    // Same canonical order Model::addTransition keeps (by id). Format 4
    // writes dissolves track by track, which need not be id order.
    std::sort(seq.transitions.begin(), seq.transitions.end(),
              [](const Transition &x, const Transition &y) { return x.id < y.id; });

    // Untrusted input (audit C3): a missing ustudio:next_id defaults to
    // "1" above, and a hand-edited or truncated file could carry a
    // next_id that's simply wrong -- either would collide a future
    // allocateId() with an id already in use here. Rather than trust the
    // file, take the largest id actually seen (sequence, tracks, clips,
    // bin assets) and make sure nextId clears it, exactly what
    // Model::reserveId() does for ids arriving through ordinary commands.
    uint64_t maxIdSeen = std::max<uint64_t>(seq.id.value, project.activeSequence.value);
    for (const Track &t : seq.tracks)
        maxIdSeen = std::max(maxIdSeen, t.id.value);
    for (const auto &[clipId, clipEntry] : seq.clips) {
        (void)clipEntry;
        maxIdSeen = std::max(maxIdSeen, clipId.value);
    }
    for (const Asset &a : project.bin)
        maxIdSeen = std::max(maxIdSeen, a.id.value);
    for (const Transition &t : seq.transitions)
        maxIdSeen = std::max(maxIdSeen, t.id.value);
    auto seeEffects = [&maxIdSeen](const std::vector<Effect> &effects) {
        for (const Effect &e : effects)
            maxIdSeen = std::max(maxIdSeen, e.id.value);
    };
    for (const auto &[clipId, clipEntry] : seq.clips)
        seeEffects(clipEntry.effects);
    for (const Track &t : seq.tracks)
        seeEffects(t.effects);
    seeEffects(seq.effects);
    for (const AdjustmentBlock &b : seq.adjustmentBlocks) {
        maxIdSeen = std::max(maxIdSeen, b.id.value);
        seeEffects(b.effects);
    }
    for (const Look &l : project.looks) {
        maxIdSeen = std::max(maxIdSeen, l.id.value);
        seeEffects(l.effects);
    }
    project.nextId = std::max(project.nextId, maxIdSeen + 1);

    project.sequences.push_back(std::move(seq));

    xmlFreeDoc(doc);
    Model model(std::move(project));
    // Final defense-in-depth gate (audit C3): a hand-edited or foreign-
    // tool-generated file can be well-formed XML yet violate an invariant
    // check() already knows how to name (an out-of-range clip span, a
    // clip on the wrong track, a video-enabled clip on an audio track, an
    // id not less than nextId despite the self-heal above catching the
    // common case) -- refuse to hand back a Model the rest of the app
    // would otherwise have to assume is sound.
    std::vector<std::string> problems = model.check();
    if (!problems.empty()) {
        std::string message = path + ": invalid project (";
        for (size_t i = 0; i < problems.size(); ++i) {
            if (i > 0)
                message += "; ";
            message += problems[i];
        }
        message += ")";
        return fail(Kind::Invalid, message);
    }
    return model;
}

} // namespace ustudio::core

#pragma GCC diagnostic pop
