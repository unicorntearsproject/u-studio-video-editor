#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/project_loader.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"

#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <latch>
#include <mutex>
#include <optional>
#include <string>

using namespace ustudio;
using app::ProjectLoader;

namespace {

// Stands in for the GTK main thread (as in test_import_queue.cpp).
struct FakeMain
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> posted;
    size_t postCount = 0;

    ProjectLoader::Post post()
    {
        return [this](std::function<void()> fn) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                posted.push_back(std::move(fn));
                ++postCount;
            }
            cv.notify_all();
        };
    }
    // Waits for `n` posts in total, then runs everything posted.
    void runAfterPosts(size_t n)
    {
        std::deque<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return postCount >= n; });
            batch.swap(posted);
        }
        for (auto &fn : batch)
            fn();
    }
};

core::Model modelWithTracks(int tracks)
{
    core::Model model = core::Model::createEmpty();
    for (int t = 0; t < tracks; ++t)
        model.addTrack(core::Track::Kind::Video, static_cast<size_t>(t), "V" + std::to_string(t + 1));
    return model;
}

} // namespace

TEST_CASE("ProjectLoader: a second load cancels the first, even if the first finishes last")
{
    std::optional<core::concurrency::ThreadPool> pool(std::in_place, 2);
    FakeMain main;
    std::latch firstRelease(1);
    ProjectLoader loader(*pool, main.post(), [&](const std::string &path) -> ProjectLoader::Result {
        if (path == "first")
            firstRelease.wait();
        return modelWithTracks(path == "first" ? 1 : 2);
    });
    std::vector<size_t> delivered;
    loader.load("first", [&](ProjectLoader::Result r) { delivered.push_back(r->sequence().tracks.size()); });
    loader.load("second", [&](ProjectLoader::Result r) { delivered.push_back(r->sequence().tracks.size()); });
    CHECK(loader.busy());
    main.runAfterPosts(1); // "second" is in
    CHECK(delivered == std::vector<size_t>{2});
    CHECK_FALSE(loader.busy());
    firstRelease.count_down();
    // "first" was already running when cancelled: it finishes but never
    // posts (stop requested), and even a post would be dropped by its id.
    pool.reset(); // join
    main.runAfterPosts(1);
    CHECK(delivered == std::vector<size_t>{2});
}

TEST_CASE("ProjectLoader: cancel() drops a load in flight (New Project meanwhile)")
{
    std::optional<core::concurrency::ThreadPool> pool(std::in_place, 1);
    FakeMain main;
    std::latch release(1);
    ProjectLoader loader(*pool, main.post(), [&](const std::string &) -> ProjectLoader::Result {
        release.wait();
        return modelWithTracks(1);
    });
    bool delivered = false;
    loader.load("p", [&](ProjectLoader::Result) { delivered = true; });
    loader.cancel();
    CHECK_FALSE(loader.busy());
    release.count_down();
    pool.reset(); // join
    main.runAfterPosts(0);
    CHECK_FALSE(delivered);
}

TEST_CASE("ProjectLoader: a real project parses on the pool; a bad one reports its error")
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("ustudio-loader-test-" + std::to_string(getpid()));
    fs::create_directories(dir);
    core::Model saved = modelWithTracks(3);
    REQUIRE(core::saveProject(saved, (dir / "p.ustudio").string()).empty());
    {
        std::ofstream bad(dir / "bad.ustudio");
        bad << "<not a project";
    }

    core::concurrency::ThreadPool pool(2);
    FakeMain main;
    ProjectLoader loader(pool, main.post(), core::loadProjectFile);
    std::optional<ProjectLoader::Result> good;
    loader.load((dir / "p.ustudio").string(), [&](ProjectLoader::Result r) { good = std::move(r); });
    main.runAfterPosts(1);
    REQUIRE(good.has_value());
    REQUIRE(good->has_value());
    CHECK((*good)->sequence().tracks.size() == 3);

    std::optional<ProjectLoader::Result> failed;
    loader.load((dir / "bad.ustudio").string(), [&](ProjectLoader::Result r) { failed = std::move(r); });
    main.runAfterPosts(2);
    REQUIRE(failed.has_value());
    CHECK_FALSE(failed->has_value());
    CHECK(failed->error().kind == core::ProjectLoadError::Kind::Unreadable);
    CHECK_FALSE(failed->error().message.empty());

    // A missing file says so, rather than looking like a format problem.
    std::optional<ProjectLoader::Result> missing;
    loader.load((dir / "gone.ustudio").string(), [&](ProjectLoader::Result r) { missing = std::move(r); });
    main.runAfterPosts(3);
    REQUIRE(missing.has_value());
    REQUIRE_FALSE(missing->has_value());
    CHECK(missing->error().kind == core::ProjectLoadError::Kind::Missing);
    fs::remove_all(dir);
}
