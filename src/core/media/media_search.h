#pragma once

// Finding missing media again (doc 07, "Missing media and relink"): where to
// look, in what order, and the search itself. Pure std; the app supplies the
// places (XDG folders, mounted drives) and runs the search on the pool.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ustudio::core {

struct MediaSearchPlaces
{
    std::string projectFolder;               // "" for an untitled project
    std::vector<std::string> missingFolders; // each missing file's old folder
    std::vector<std::string> mediaFolders;   // where the project's other media is
    std::vector<std::string> userFolders;    // Videos, Pictures, Music
    std::string home;
    std::vector<std::string> mounts; // mounted drives (platform::mountedVolumeRoots())
};

// The folders "Find automatically" searches, in order, each with its
// subfolders: the project's folder; each missing file's old folder, then
// that folder's parent (its siblings); the folders the project's other media
// is in; Videos, Pictures, Music; home; mounted drives. Duplicates and
// empty entries are left out; a folder inside one listed earlier stays,
// since findMediaFiles() skips what it has already searched.
std::vector<std::string> mediaSearchRoots(const MediaSearchPlaces &places);

struct WantedMedia
{
    uint64_t id = 0;         // the caller's (an asset id)
    std::string name;        // file name to match
    std::string fingerprint; // fileFingerprint() when saved; "" if unknown
};

struct MediaSearchProgress
{
    std::string folder; // being searched now
    size_t filesSeen = 0;
    size_t found = 0;
};

// Walks `roots` in order, each with its subfolders, for files named as
// `wanted`: one whose fingerprint also matches settles it; otherwise the
// first file with that name (in search order) is kept as a likely match.
// Skips hidden folders below a root (".cache", ".local", the Trash),
// symlinked folders, unreadable ones, and any folder already searched as
// part of an earlier root; stops early once everything is settled, or when
// `cancel` reads true, or after `maxFiles`. `progress` is called every few
// hundred files and at each root. Blocking: pool threads only.
std::map<uint64_t, std::string> findMediaFiles(const std::vector<WantedMedia> &wanted,
                                               const std::vector<std::string> &roots,
                                               const std::atomic<bool> *cancel = nullptr,
                                               const std::function<void(const MediaSearchProgress &)> &progress = {},
                                               size_t maxFiles = 2'000'000);

} // namespace ustudio::core
