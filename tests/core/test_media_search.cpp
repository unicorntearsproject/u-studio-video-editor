// doc 07, "Missing media and relink": where Find Automatically looks and how
// it matches (core/media/media_search.h).

#include "doctest.h"

#include "core/media/fingerprint.h"
#include "core/media/media_search.h"

#include <filesystem>
#include <fstream>
#include <random>

using namespace ustudio::core;
namespace fs = std::filesystem;

namespace {

fs::path scratch()
{
    std::random_device random;
    const fs::path dir = fs::temp_directory_path() / ("ustudio-media-search-" + std::to_string(random()));
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

} // namespace

TEST_CASE("media search: roots in a logical order, each once")
{
    MediaSearchPlaces places;
    places.projectFolder = "/home/u/projects/promo/";
    places.missingFolders = {"/home/u/footage/day1", "/home/u/footage/day2", "/home/u/footage/day1"};
    places.mediaFolders = {"/home/u/projects/promo", "/mnt/raid/music"};
    places.userFolders = {"/home/u/Videos", "/home/u/Pictures", "/home/u/Music"};
    places.home = "/home/u";
    places.mounts = {"/run/media/u", "/mnt"};
    CHECK(mediaSearchRoots(places) == std::vector<std::string>{"/home/u/projects/promo", "/home/u/footage/day1",
                                                               "/home/u/footage/day2", "/home/u/footage",
                                                               "/mnt/raid/music", "/home/u/Videos", "/home/u/Pictures",
                                                               "/home/u/Music", "/home/u", "/run/media/u", "/mnt"});
    CHECK(mediaSearchRoots({}).empty());
}

TEST_CASE("media search: by name, the fingerprint deciding between same-named files; hidden folders skipped")
{
    const fs::path dir = scratch();
    write(dir / "a" / "clip.mp4", "not this one");
    write(dir / "b" / "deep" / "clip.mp4", "the right one");
    write(dir / "b" / "other.mov", "other");
    write(dir / ".cache" / "lost.wav", "hidden");
    const std::string right = (dir / "b" / "deep" / "clip.mp4").string();

    std::vector<WantedMedia> wanted{
        {1, "clip.mp4", fileFingerprint(right)}, {2, "other.mov", ""}, {3, "lost.wav", ""}, {4, "nowhere.mp4", ""}};
    size_t calls = 0;
    const auto found = findMediaFiles(wanted, {dir.string()}, nullptr, [&](const MediaSearchProgress &) { ++calls; });
    CHECK(found.at(1) == right); // not the earlier, same-named a/clip.mp4
    CHECK(found.at(2) == (dir / "b" / "other.mov").string());
    CHECK_FALSE(found.contains(3)); // under .cache
    CHECK_FALSE(found.contains(4));
    CHECK(calls >= 2);

    // Without a fingerprint, the first one in search order is taken.
    const auto first = findMediaFiles({{1, "clip.mp4", "1:2"}}, {(dir / "a").string(), dir.string()});
    CHECK(first.at(1) == (dir / "a" / "clip.mp4").string());

    // Cancelled: nothing searched.
    std::atomic<bool> cancel{true};
    CHECK(findMediaFiles(wanted, {dir.string()}, &cancel).empty());
    fs::remove_all(dir);
}

TEST_CASE("media search: a folder searched as an earlier root isn't walked again")
{
    const fs::path dir = scratch();
    write(dir / "inner" / "x.mp4", "x");
    // `inner` first, then its parent: the parent's walk skips `inner`, so the
    // file is counted once.
    size_t seen = 0;
    findMediaFiles({{1, "absent.mp4", ""}}, {(dir / "inner").string(), dir.string()}, nullptr,
                   [&](const MediaSearchProgress &p) { seen = p.filesSeen; });
    CHECK(seen == 1);
    fs::remove_all(dir);
}
