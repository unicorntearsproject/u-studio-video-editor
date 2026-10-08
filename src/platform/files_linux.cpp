#include "platform/files.h"

#include <cerrno>
#include <cstdlib>
#include <system_error>
#include <vector>

namespace ustudio::platform {

std::filesystem::path makePrivateDirectory(const std::filesystem::path &base, const std::string &prefix,
                                           std::string *error)
{
    // mkdtemp(): atomic, mode 0700, a random name (engine/factory_policy.cpp
    // says why a guessable one would be a hole).
    const std::string pattern = (base / (prefix + "XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (!::mkdtemp(buffer.data())) {
        if (error)
            *error = std::error_code(errno, std::generic_category()).message(); // strerror isn't thread-safe
        return {};
    }
    return std::filesystem::path(buffer.data());
}

bool linkFile(const std::filesystem::path &target, const std::filesystem::path &link, std::string *error)
{
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec && error)
        *error = ec.message();
    return !ec;
}

std::vector<std::filesystem::path> mountedVolumeRoots()
{
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> candidates;
    if (const char *user = std::getenv("USER"); user && *user) {
        candidates.push_back(std::filesystem::path("/run/media") / user);
        candidates.push_back(std::filesystem::path("/media") / user);
    }
    candidates.insert(candidates.end(), {"/media", "/mnt"});
    for (const std::filesystem::path &candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec))
            roots.push_back(candidate);
    }
    return roots;
}

std::filesystem::path userCacheDirectory()
{
    // As GLib's g_get_user_cache_dir(): an empty or relative
    // XDG_CACHE_HOME is ignored (the XDG base directory spec).
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg == '/')
        return xdg;
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache";
    return {};
}

const char *sharedLibrarySuffix()
{
    return ".so";
}

} // namespace ustudio::platform
