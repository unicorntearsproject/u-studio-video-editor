#pragma once

#include <gio/gio.h>

#include <string>
#include <vector>

namespace ustudio::app {

// Thin wrapper around this app's GSettings schema (com.ustudio.VideoEditor,
// data/com.ustudio.VideoEditor.gschema.xml). Every getter falls back to the
// same hardcoded default the corresponding call site used before this
// existed; every setter is a silent no-op when the schema wasn't found --
// see settings.cpp for why that degrade-don't-crash path is load-bearing,
// not defensive-programming boilerplate.
//
// Modularity note (the reason this is its own small class rather than four
// GSettings calls inlined at each call site): a future hotkey-rebinding
// feature needs the exact same schema-lookup/degrade plumbing to persist
// accelerator overrides. That should be a new key on the same schema and a
// new accessor pair here, not a second, separately-wired GSettings object.
class Settings
{
  public:
    // `fallbackSchemaDir`: where to look for gschemas.compiled when no
    // installed schema source has ours (builddirSchemaDir() by default: a
    // binary run straight from builddir finds builddir/data). "" for none.
    explicit Settings(const std::string &fallbackSchemaDir = builddirSchemaDir());
    ~Settings();

    // <dir of /proc/self/exe>/../../data: builddir/data for builddir/src/app
    // and builddir/tests/app alike.
    static std::string builddirSchemaDir();

    Settings(const Settings &) = delete;
    Settings &operator=(const Settings &) = delete;

    static constexpr int kDefaultAutosaveDelayMinutes = 2;
    static constexpr const char *kDefaultPreviewScale = "auto";
    static constexpr double kDefaultShuttleMaxSpeed = 8.0;
    static constexpr int kDefaultRecentProjectsMax = 10;
    static constexpr int kDefaultWorkerThreads = 0; // automatic
    static constexpr int kDefaultCacheJobs = 0;     // automatic

    int autosaveDelayMinutes() const;
    void setAutosaveDelayMinutes(int minutes);

    // One of "auto"/"full"/"half"/"quarter" -- matches the transport bar's
    // preview-scale dropdown entries (buildUi()) and the gschema's
    // <choices>. Only consulted once, for that dropdown's initial
    // selection at startup; changing the dropdown afterwards does not
    // write back here (see showSettingsDialog()'s own comment).
    std::string defaultPreviewScale() const;
    void setDefaultPreviewScale(const std::string &scale);

    double shuttleMaxSpeed() const;
    void setShuttleMaxSpeed(double speed);

    int recentProjectsMax() const;
    void setRecentProjectsMax(int max);

    // The worker pool's size (doc 19 MT1); 0 means automatic
    // (core::concurrency::ThreadPool::defaultThreadCount()). Read once, when
    // the window creates its pool.
    int workerThreads() const;
    void setWorkerThreads(int threads);

    // How many pool threads the timeline's thumbnail and waveform caches may
    // use at once; 0 means automatic (half the pool). Read at startup.
    int cacheJobs() const;
    void setCacheJobs(int jobs);

    // Settings > Toggles.
    bool reopenLastProject() const;
    void setReopenLastProject(bool reopen);
    bool snapWhileDragging() const;
    void setSnapWhileDragging(bool snap);
    bool followPlayhead() const;
    void setFollowPlayhead(bool follow);
    bool showTimelineThumbnails() const;
    void setShowTimelineThumbnails(bool show);
    bool showWaveforms() const;
    void setShowWaveforms(bool show);
    bool showInspector() const; // docked; the narrow layout always starts hidden
    void setShowInspector(bool show);
    bool showHoverPreview() const;
    void setShowHoverPreview(bool show);

    // The project open when the app last ran ("" if it was untitled); what
    // "Reopen last project on startup" opens.
    std::string lastProjectPath() const;
    void setLastProjectPath(const std::string &path);

    // Settings > Locations; "" means not set.
    std::string defaultProjectFolder() const;
    void setDefaultProjectFolder(const std::string &folder);
    std::string defaultExportFolder() const;
    void setDefaultExportFolder(const std::string &folder);

    // Settings > Render > Render threads: a percentage of the machine's
    // hardware threads (core::renderThreadBudget()).
    static constexpr int kDefaultRenderThreadsPercent = 80;
    int renderThreadsPercent() const;
    void setRenderThreadsPercent(int percent);

    // The render profile the Render button uses (RenderProfileStore).
    std::string defaultRenderProfile() const;
    void setDefaultRenderProfile(const std::string &name);
    // Settings > Drop-ins: installed drop-ins not to load, read once at
    // startup (unloading native code at runtime isn't safe; doc 17).
    std::vector<std::string> disabledDropIns() const;

    // M4 C: whether playback uses proxies (view state, remembered here, not
    // in the project), and the height new ones get (0: source size).
    static constexpr int kDefaultProxyHeight = 540;
    bool useProxies() const;
    void setUseProxies(bool use);
    // ADR-019: GPU acceleration (Automatic when on), hardware decode while
    // it's on, and the last probe result (GpuAcceleration).
    bool gpuAcceleration() const;
    void setGpuAcceleration(bool on);
    bool hardwareDecode() const;
    void setHardwareDecode(bool on);
    std::string gpuProbeCache() const;
    void setGpuProbeCache(const std::string &value);
    int proxyHeight() const;
    void setProxyHeight(int height);
    void setDropInEnabled(const std::string &name, bool enabled);

    // M4 G: Help as it was left: the open sections ("<tab>:<section>"),
    // the tab, and each tab's scroll position ("<tab>=<pixels>"). Empty
    // without a schema, or with an older installed one lacking the keys.
    std::vector<std::string> helpOpenSections() const;
    void setHelpOpenSections(const std::vector<std::string> &sections);
    std::string helpTab() const;
    void setHelpTab(const std::string &tab);
    std::vector<std::string> helpScroll() const;
    void setHelpScroll(const std::vector<std::string> &positions);

    // False when the schema wasn't found (not installed, and
    // GSETTINGS_SCHEMA_DIR doesn't point at a compiled one) -- every
    // setter above is then a no-op. The Settings dialog shows a note when
    // this is false, so changes made there don't look silently accepted.
    bool isPersistent() const
    {
        return m_settings != nullptr;
    }

  private:
    GSettingsSchemaSource *m_fallbackSource = nullptr;
    bool getBool(const char *key, bool fallback) const;
    void setBool(const char *key, bool value);
    std::string getString(const char *key, const char *fallback) const;
    void setString(const char *key, const std::string &value);
    // For keys newer than a schema that may be installed: g_settings_get_*
    // aborts on a key the schema doesn't have.
    bool hasKey(const char *key) const;
    std::vector<std::string> getStrv(const char *key) const;
    void setStrv(const char *key, const std::vector<std::string> &values);

    GSettings *m_settings = nullptr;
};

} // namespace ustudio::app
