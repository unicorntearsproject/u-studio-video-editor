#include "dropins/registry.h"

#include "core/log.h"

#include <gmodule.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ustudio::dropins {

namespace Log = core::Log;

namespace {
const DropInRegistry *s_current = nullptr;
} // namespace

const DropInRegistry *DropInRegistry::current()
{
    return s_current;
}

void DropInRegistry::setCurrent(const DropInRegistry *registry)
{
    s_current = registry;
}

std::vector<std::string> DropInRegistry::trustedDirectories()
{
    std::vector<std::string> dirs{USTUDIO_DROPIN_DIR};
    const std::vector<std::string> extensions = extensionDirectories(USTUDIO_DROPIN_EXTENSION_DIR);
    dirs.insert(dirs.end(), extensions.begin(), extensions.end());
    return dirs;
}

std::vector<std::string> DropInRegistry::extensionDirectories(const std::string &extensionPoint)
{
    // One subdirectory per installed extension (Flatpak mounts
    // com.ustudio.VideoEditor.DropIn.<Name> at <extension point>/<Name>),
    // each an install prefix of its own. Sorted, so the load order is stable.
    std::vector<std::string> dirs;
    std::error_code ec;
    if (extensionPoint.empty() || !fs::is_directory(extensionPoint, ec))
        return dirs;
    for (const fs::directory_entry &entry : fs::directory_iterator(extensionPoint, ec))
        if (entry.is_directory(ec))
            dirs.push_back((entry.path() / "lib" / "u-studio" / "drop-ins").string());
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

std::vector<std::string> DropInRegistry::searchDirectories()
{
    const char *overridePath = std::getenv("USTUDIO_DROPIN_PATH");
    if (!overridePath || !*overridePath)
        return trustedDirectories();
    Log::warn(std::string("[drop-ins] USTUDIO_DROPIN_PATH is set: loading drop-ins from ") + overridePath +
              " instead of the installed ones (development only)");
    std::vector<std::string> dirs;
    std::string list = overridePath;
    for (size_t start = 0, end; start <= list.size(); start = end + 1) {
        end = list.find(':', start);
        if (end == std::string::npos)
            end = list.size();
        if (end > start)
            dirs.push_back(list.substr(start, end - start));
    }
    return dirs;
}

std::string DropInRegistry::problemWith(const UStudioDropInDescription *describe, const std::string &appVersion)
{
    if (!describe)
        return "it describes nothing";
    if (describe->apiVersion != DROPIN_API_VERSION)
        return "it was built for drop-in API " + std::to_string(describe->apiVersion) + ", this app has " +
               std::to_string(DROPIN_API_VERSION);
    if (!describe->appVersion || appVersion != describe->appVersion)
        return std::string("it was built for U-Stu ") + (describe->appVersion ? describe->appVersion : "(unknown)") +
               ", this is " + appVersion + " (drop-ins update with the app)";
    if (!describe->name || !*describe->name)
        return "it has no name";
    return {};
}

bool DropInRegistry::hasName(const std::string &name) const
{
    return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry &e) { return e.name == name; });
}

bool DropInRegistry::has(const std::string &name) const
{
    return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry &e) { return e.name == name && e.enabled; });
}

bool DropInRegistry::isDisabled(const std::string &name) const
{
    return std::find(m_disabled.begin(), m_disabled.end(), name) != m_disabled.end();
}

void DropInRegistry::addBuiltin(const UStudioDropInDescription *describe)
{
    if (std::string problem = problemWith(describe, USTUDIO_VERSION); !problem.empty()) {
        // Built from this tree, so this is a build mistake, not a user's.
        m_refusals.push_back("built-in drop-in: " + problem);
        Log::error("[drop-ins] Built-in drop-in refused: " + problem);
        return;
    }
    if (hasName(describe->name))
        return;
    const bool enabled = !isDisabled(describe->name);
    m_entries.push_back(
        {describe->name, describe->description ? describe->description : "", "", describe, USTUDIO_VERSION, enabled});
    Log::info(std::string("[drop-ins] Built in: ") + describe->name + (enabled ? "" : " (disabled in Settings)"));
}

void DropInRegistry::loadModules(const std::vector<std::string> &directories)
{
    if (!g_module_supported())
        return;
    for (const std::string &dir : directories) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        std::vector<fs::path> files;
        for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with("libustudio-dropin-") && name.ends_with(".so"))
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path &file : files) {
            auto refuse = [&](const std::string &why) {
                m_refusals.push_back(file.string() + ": " + why);
                Log::warn("[drop-ins] Not loading " + file.string() + ": " + why);
            };
            // libustudio-dropin-<name>.so
            const std::string stem = file.filename().string();
            const std::string fileName = stem.substr(18, stem.size() - 18 - 3);
            if (isDisabled(fileName)) {
                if (!hasName(fileName))
                    m_entries.push_back({fileName, "", file.string(), nullptr, "", false});
                Log::info("[drop-ins] Not loading " + fileName + ": disabled in Settings");
                continue;
            }
            // Local binding: a module's symbols don't leak into later ones.
            GModule *module = g_module_open(file.c_str(), G_MODULE_BIND_LOCAL);
            if (!module) {
                refuse(g_module_error());
                continue;
            }
            gpointer symbol = nullptr;
            if (!g_module_symbol(module, "ustudio_drop_in_describe", &symbol) || !symbol) {
                refuse("it doesn't export ustudio_drop_in_describe");
                g_module_close(module);
                continue;
            }
            const UStudioDropInDescription *describe = reinterpret_cast<UStudioDropInDescribeFn>(symbol)();
            if (std::string problem = problemWith(describe, USTUDIO_VERSION); !problem.empty()) {
                refuse(problem);
                g_module_close(module);
                continue;
            }
            if (fileName != describe->name) {
                refuse(std::string("it calls itself \"") + describe->name + "\" but its file is named for \"" +
                       fileName + "\"");
                g_module_close(module);
                continue;
            }
            if (hasName(describe->name)) {
                refuse(std::string("a drop-in called \"") + describe->name + "\" is already registered");
                g_module_close(module);
                continue;
            }
            // Kept loaded for the process's life: unloading native code a
            // running graph may still call isn't safe (doc 17).
            g_module_make_resident(module);
            m_entries.push_back({describe->name, describe->description ? describe->description : "", file.string(),
                                 describe, describe->appVersion, true});
            Log::info("[drop-ins] Loaded " + std::string(describe->name) + " from " + file.string());
        }
    }
}

void DropInRegistry::contributeFactoryPaths(FactoryPaths &paths) const
{
    for (const Entry &entry : m_entries)
        if (entry.enabled && entry.describe->contributeFactoryPaths)
            entry.describe->contributeFactoryPaths(&paths);
}

void BasicDropInHost::log(const std::string &message)
{
    Log::info(message);
}

void BasicDropInHost::addEngineExtension(engine::EngineExtensionFactory factory)
{
    engine::registerEngineExtension(std::move(factory));
}

void BasicDropInHost::addRenderSubcommand(RenderSubcommand subcommand)
{
    const std::string &name = subcommand.name;
    const bool wellFormed = !name.empty() && name.front() != '-' && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
    if (!wellFormed || name == "help" || !subcommand.run) {
        Log::warn("[dropins] refused render subcommand \"" + name +
                  "\": needs a lowercase name (not \"help\") and a run function");
        return;
    }
    const bool taken = std::any_of(m_renderSubcommands.begin(), m_renderSubcommands.end(),
                                   [&](const RenderSubcommand &existing) { return existing.name == name; });
    if (taken) {
        Log::warn("[dropins] refused render subcommand --" + name + ": already registered by another drop-in");
        return;
    }
    m_renderSubcommands.push_back(std::move(subcommand));
}

void DropInRegistry::registerAll(DropInHost &host) const
{
    for (const Entry &entry : m_entries)
        if (entry.enabled && entry.describe->registerDropIn)
            entry.describe->registerDropIn(&host);
}

} // namespace ustudio::dropins
