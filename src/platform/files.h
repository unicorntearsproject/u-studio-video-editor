#pragma once

// ADR-017: file-system calls std::filesystem can't do portably. Std and OS
// headers only.

#include <filesystem>
#include <vector>
#include <string>

namespace ustudio::platform {

// A new directory under `base`, named `prefix` plus a random suffix, created
// atomically and readable by this user only (a directory whose contents get
// loaded into the process must not be pre-creatable by anyone else). Empty on
// failure, with `error` saying why.
std::filesystem::path makePrivateDirectory(const std::filesystem::path &base, const std::string &prefix,
                                           std::string *error = nullptr);

// Makes `link` open `target`: a symbolic link on Linux (a copy where links
// need privileges, on Windows). False on failure, with `error` saying why.
bool linkFile(const std::filesystem::path &target, const std::filesystem::path &link, std::string *error = nullptr);

// The user's cache folder: $XDG_CACHE_HOME, else ~/.cache (%LOCALAPPDATA%
// on Windows). Empty if neither is known. Not created.
std::filesystem::path userCacheDirectory();

// Where removable and extra drives are mounted, most specific first, those
// that exist: on Linux /run/media/<user> and /media/<user> (udisks), then
// /media and /mnt (drive letters on Windows). For searching missing media.
std::vector<std::filesystem::path> mountedVolumeRoots();

// A shared library's file-name ending here: ".so" on Linux (".dll" on
// Windows). MLT's modules are "libmlt<name>" plus this.
const char *sharedLibrarySuffix();

} // namespace ustudio::platform
