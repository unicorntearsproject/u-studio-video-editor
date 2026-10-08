#include "gpu_acceleration.h"

#include "settings.h"
#include "core/log.h"

#include <fstream>
#include <system_error>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {

// The cache setting: "<key>\t<1|0>\t<renderer or why not>".
struct Cached
{
    std::string key;
    bool ok = false;
    std::string detail;
};

Cached parseCache(const std::string &text)
{
    Cached cached;
    const size_t first = text.find('\t');
    const size_t second = first == std::string::npos ? std::string::npos : text.find('\t', first + 1);
    if (second == std::string::npos)
        return cached;
    cached.key = text.substr(0, first);
    cached.ok = text.substr(first + 1, second - first - 1) == "1";
    cached.detail = text.substr(second + 1);
    return cached;
}

} // namespace

std::string gpuProbeCacheKey(const std::string &appVersion, const std::string &mltVersion)
{
    return appVersion + "|" + mltVersion;
}

GpuAcceleration::GpuAcceleration(Settings &settings, std::unique_ptr<ProxyQueue::Launcher> launcher,
                                 std::string renderTool, std::filesystem::path sentinel, std::string cacheKey,
                                 std::string decodeApi, Hooks hooks)
    : m_settings(settings), m_launcher(std::move(launcher)), m_renderTool(std::move(renderTool)),
      m_sentinel(std::move(sentinel)), m_cacheKey(std::move(cacheKey)), m_decodeApi(std::move(decodeApi)),
      m_hooks(std::move(hooks))
{}

GpuAcceleration::~GpuAcceleration()
{
    if (m_probe)
        m_probe->cancel();
}

void GpuAcceleration::start()
{
    if (!m_settings.gpuAcceleration()) {
        setStatus(Status::Off, {});
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(m_sentinel, ec)) {
        std::filesystem::remove(m_sentinel, ec);
        m_settings.setGpuAcceleration(false);
        Log::warn("[gpu] the last session ended with the GPU pipeline live; GPU acceleration turned off");
        setStatus(Status::Off, {});
        m_hooks.notify("GPU acceleration was turned off because U-Stu closed unexpectedly while using it. "
                       "You can turn it back on in Settings › Performance.");
        return;
    }
    useCacheOrProbe();
}

void GpuAcceleration::useCacheOrProbe()
{
    const Cached cached = parseCache(m_settings.gpuProbeCache());
    if (cached.key == m_cacheKey && cached.ok) {
        setStatus(Status::Checking, cached.detail);
        requestOn();
    } else {
        setStatus(Status::Checking, {});
    }
    probe(); // re-checked every launch: a driver update isn't in the key
}

void GpuAcceleration::probe()
{
    if (m_probe)
        return;
    if (m_renderTool.empty()) {
        probeFinished({}, -1);
        return;
    }
    auto output = std::make_shared<std::string>();
    m_probe = m_launcher->start(
        {m_renderTool, "--gpu-probe"}, [output](const std::string &line) { *output += line; },
        [this, output](int exitStatus) {
            m_probe.reset();
            probeFinished(*output, exitStatus);
        });
}

void GpuAcceleration::probeFinished(const std::string &output, int exitStatus)
{
    const bool ok = exitStatus == 0 && jsonField(output, "status").value_or("") == "ok";
    const std::string detail = ok ? jsonField(output, "renderer").value_or("")
                               : m_renderTool.empty()
                                   ? "the render tool wasn't found"
                                   : jsonField(output, "message").value_or("the GPU check crashed or couldn't run");
    Log::info(std::string("[gpu] probe ") + (ok ? "passed: " : "failed: ") + detail);
    m_settings.setGpuProbeCache(m_cacheKey + "\t" + (ok ? "1" : "0") + "\t" + detail);
    if (!m_settings.gpuAcceleration())
        return; // switched off meanwhile
    if (ok) {
        if (!m_requested)
            requestOn();
        return;
    }
    const bool wasOn = m_requested;
    m_requested = false;
    if (wasOn) {
        m_hooks.setPipeline(false, {});
        m_hooks.notify("GPU acceleration is off: " + detail);
    }
    setStatus(Status::Unavailable, detail);
}

void GpuAcceleration::requestOn()
{
    m_requested = true;
    m_hooks.setPipeline(true, decodeApi());
}

std::string GpuAcceleration::decodeApi() const
{
    return m_settings.hardwareDecode() ? m_decodeApi : std::string();
}

void GpuAcceleration::setEnabled(bool on)
{
    m_settings.setGpuAcceleration(on);
    if (on) {
        if (!m_requested && !m_probe)
            useCacheOrProbe();
        return;
    }
    if (m_probe) {
        m_probe->cancel();
        m_probe.reset();
    }
    const bool wasOn = m_requested;
    m_requested = false;
    if (wasOn)
        m_hooks.setPipeline(false, {});
    setStatus(Status::Off, {});
}

void GpuAcceleration::setHardwareDecode(bool on)
{
    m_settings.setHardwareDecode(on);
    if (m_live)
        m_hooks.setHardwareDecode(decodeApi());
}

void GpuAcceleration::engineChanged(bool on, const std::string &detail)
{
    m_live = on;
    std::error_code ec;
    if (on) {
        // The sentinel: removed again on a clean exit or a switch back.
        std::filesystem::create_directories(m_sentinel.parent_path(), ec);
        std::ofstream(m_sentinel) << detail << "\n";
        setStatus(Status::On, detail);
        return;
    }
    std::filesystem::remove(m_sentinel, ec);
    if (!m_requested || detail.empty())
        return; // asked for: setEnabled() or probeFinished() set the status
    // The engine couldn't start or keep the pipeline.
    m_requested = false;
    setStatus(Status::Stopped, detail);
    m_hooks.notify("GPU acceleration stopped (" + detail + "); playing on the processor instead.");
}

void GpuAcceleration::shutdown()
{
    if (m_probe) {
        m_probe->cancel();
        m_probe.reset();
    }
    std::error_code ec;
    std::filesystem::remove(m_sentinel, ec);
}

std::string GpuAcceleration::statusText() const
{
    switch (m_status) {
    case Status::Off:
        return "Off";
    case Status::Checking:
        return "Checking the graphics card…";
    case Status::On:
        return "On: " + m_detail;
    case Status::Unavailable:
        return "Not available here: " + m_detail;
    case Status::Stopped:
        return "Stopped: " + m_detail;
    }
    return {};
}

void GpuAcceleration::setStatus(Status status, std::string detail)
{
    m_status = status;
    m_detail = std::move(detail);
    if (m_hooks.changed)
        m_hooks.changed();
}

} // namespace ustudio::app
