#include "core/media/media_search.h"

#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace ustudio::core {

namespace fs = std::filesystem;

namespace {

std::string normal(const std::string &folder)
{
    if (folder.empty())
        return {};
    std::string text = utf8String(pathFromUtf8(folder).lexically_normal());
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\'))
        text.pop_back();
    return text;
}

// `path` is `folder` or inside it.
bool within(const fs::path &path, const fs::path &folder)
{
    auto [end, rest] = std::mismatch(folder.begin(), folder.end(), path.begin(), path.end());
    return end == folder.end();
}

} // namespace

std::vector<std::string> mediaSearchRoots(const MediaSearchPlaces &places)
{
    std::vector<std::string> roots;
    auto add = [&](const std::string &folder) {
        const std::string text = normal(folder);
        if (!text.empty() && std::find(roots.begin(), roots.end(), text) == roots.end())
            roots.push_back(text);
    };
    add(places.projectFolder);
    for (const std::string &folder : places.missingFolders)
        add(folder);
    for (const std::string &folder : places.missingFolders) {
        const fs::path parent = pathFromUtf8(normal(folder)).parent_path();
        if (!parent.empty() && parent != pathFromUtf8(normal(folder)))
            add(utf8String(parent));
    }
    for (const std::string &folder : places.mediaFolders)
        add(folder);
    for (const std::string &folder : places.userFolders)
        add(folder);
    add(places.home);
    for (const std::string &folder : places.mounts)
        add(folder);
    return roots;
}

std::map<uint64_t, std::string> findMediaFiles(const std::vector<WantedMedia> &wanted,
                                               const std::vector<std::string> &roots, const std::atomic<bool> *cancel,
                                               const std::function<void(const MediaSearchProgress &)> &progress,
                                               size_t maxFiles)
{
    std::multimap<std::string, const WantedMedia *> byName;
    for (const WantedMedia &want : wanted)
        if (!want.name.empty())
            byName.emplace(want.name, &want);
    std::map<uint64_t, std::string> settled, likely;
    MediaSearchProgress state;
    std::vector<fs::path> searched;
    auto done = [&] { return settled.size() == wanted.size() || (cancel && cancel->load()); };

    for (const std::string &rootText : roots) {
        if (done() || state.filesSeen >= maxFiles)
            break;
        const fs::path root = pathFromUtf8(rootText);
        std::error_code ec;
        if (!fs::is_directory(root, ec) ||
            std::any_of(searched.begin(), searched.end(), [&](const fs::path &s) { return within(root, s); }))
            continue;
        state.folder = rootText;
        if (progress)
            progress(state);
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && !done() && state.filesSeen < maxFiles; it.increment(ec)) {
            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();
            std::error_code typeEc;
            if (entry.is_directory(typeEc)) {
                const std::string name = utf8String(path.filename());
                const bool skip =
                    (!name.empty() && name[0] == '.') || entry.is_symlink(typeEc) ||
                    std::any_of(searched.begin(), searched.end(), [&](const fs::path &s) { return within(path, s); });
                if (skip)
                    it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(typeEc))
                continue;
            if (++state.filesSeen % 500 == 0 && progress) {
                state.folder = utf8String(path.parent_path());
                state.found = settled.size();
                progress(state);
            }
            auto [first, last] = byName.equal_range(utf8String(path.filename()));
            for (auto match = first; match != last; ++match) {
                const WantedMedia &want = *match->second;
                if (settled.contains(want.id))
                    continue;
                const std::string file = utf8String(path);
                if (want.fingerprint.empty() || fileFingerprint(file) == want.fingerprint)
                    settled[want.id] = file;
                else
                    likely.emplace(want.id, file); // keeps the first one seen
            }
        }
        searched.push_back(root);
    }
    for (const auto &[id, file] : likely)
        settled.emplace(id, file); // only where nothing better was found
    state.found = settled.size();
    if (progress)
        progress(state);
    return settled;
}

} // namespace ustudio::core
