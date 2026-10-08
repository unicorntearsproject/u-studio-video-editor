#include "portal_path.h"

#include "core/log.h"

#include <fcntl.h>
#include <glib.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cstring>

namespace ustudio::app::portal {

std::string resolveHostPath(const std::string &path, bool createIfMissing)
{
    return resolveHostPath(path, createIfMissing, std::string(g_get_user_runtime_dir()) + "/doc/");
}

std::string resolveHostPath(const std::string &path, bool createIfMissing, const std::string &portalRoot)
{
    if (path.rfind(portalRoot, 0) != 0)
        return path;

    if (createIfMissing && !g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0)
            close(fd);
    }

    char buffer[4096];
    ssize_t length = getxattr(path.c_str(), "user.document-portal.host-path", buffer, sizeof(buffer));
    if (length <= 0) {
        core::Log::debug("[app] document-portal path with no host-path xattr, using it as-is: " + path);
        return path;
    }
    std::string host(buffer, strnlen(buffer, static_cast<size_t>(length)));
    if (host.empty() || !g_file_test(host.c_str(), G_FILE_TEST_EXISTS)) {
        core::Log::debug("[app] document-portal host path not reachable from here, using the portal path: " + path);
        return path;
    }
    core::Log::debug("[app] resolved document-portal path " + path + " -> " + host);
    return host;
}

} // namespace ustudio::app::portal
