#pragma once

#include "core/concurrency/thread_pool.h"
#include "core/model/model.h"
#include "core/xml/reader.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace ustudio::app {

// doc 19 MT1: opening a project parses it on the worker pool; the window
// swaps the model in on the main thread. Latest wins: starting a load
// cancels the one in flight and drops its result, so a second Open (or a
// New Project meanwhile) is never overtaken by the first. No GTK here, so
// tests/app/test_project_loader.cpp drives it directly.
class ProjectLoader
{
  public:
    using Result = std::expected<core::Model, core::ProjectLoadError>;
    // Pool thread: core::loadProjectFile() in the app; tests substitute their own.
    using Parse = std::function<Result(const std::string &path)>;
    // Main thread, only for the latest load.
    using Done = std::function<void(Result result)>;
    using Post = std::function<void(std::function<void()>)>;

    ProjectLoader(core::concurrency::ThreadPool &pool, Post post, Parse parse);

    void load(std::string path, Done done);
    // Drops whatever is in flight (New Project, shutdown). Main thread.
    void cancel();
    bool busy() const;

  private:
    core::concurrency::ThreadPool &m_pool;
    Post m_post;
    Parse m_parse;
    // Main thread only; shared with posted closures so one arriving after
    // the loader is gone reads this, not freed memory.
    struct State
    {
        uint64_t latest = 0;
        bool busy = false;
    };
    std::shared_ptr<State> m_state = std::make_shared<State>();
    core::concurrency::JobHandle m_job;
};

} // namespace ustudio::app
