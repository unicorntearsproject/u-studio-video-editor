// app::snapEnvironmentFixes on made-up environments.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/snap_env.h"

#include <algorithm>

using namespace ustudio::app;

namespace {

std::optional<std::optional<std::string>> changeFor(const std::vector<EnvChange> &changes, const std::string &name)
{
    auto it = std::find_if(changes.begin(), changes.end(), [&](const EnvChange &c) { return c.name == name; });
    if (it == changes.end())
        return std::nullopt; // untouched
    return it->value;
}

bool unsets(const std::vector<EnvChange> &changes, const std::string &name)
{
    auto change = changeFor(changes, name);
    return change && !*change;
}

bool sets(const std::vector<EnvChange> &changes, const std::string &name, const std::string &value)
{
    auto change = changeFor(changes, name);
    return change && *change == value;
}

} // namespace

TEST_CASE("snap environment: VS Code's snap terminal is undone")
{
    const std::string home = "/home/user";
    std::map<std::string, std::string> env = {
        {"GIO_MODULE_DIR", "/home/user/snap/code/common/.cache/gio-modules"},
        {"GTK_PATH", "/snap/code/263/usr/lib/x86_64-linux-gnu/gtk-3.0"},
        {"LOCPATH", "/snap/code/263/usr/lib/locale"},
        {"XDG_DATA_HOME", "/home/user/snap/code/263/.local/share"},
        {"XDG_DATA_DIRS", "/home/user/snap/code/263/.local/share:/snap/code/263/usr/share:/usr/share"},
        {"XDG_DATA_DIRS_VSCODE_SNAP_ORIG", "/usr/local/share/:/usr/share/:/var/lib/snapd/desktop"},
        {"PATH", "/snap/code/263/usr/bin:/home/user/.local/bin:/usr/bin"},
        {"LD_LIBRARY_PATH", "/snap/core20/current/lib"},
        {"SNAP", "/snap/code/263"},
        {"SNAP_NAME", "code"},
        {"GIO_LAUNCHED_DESKTOP_FILE", "/var/lib/snapd/desktop/applications/code_code.desktop"},
        {"HOME", "/home/user"},
        {"LANG", "en_US.UTF-8"},
    };
    std::vector<EnvChange> changes = snapEnvironmentFixes(env, home);

    CHECK(unsets(changes, "GIO_MODULE_DIR"));
    CHECK(unsets(changes, "GTK_PATH"));
    CHECK(unsets(changes, "LOCPATH"));
    CHECK(unsets(changes, "XDG_DATA_HOME"));
    // The saved original wins over filtering.
    CHECK(sets(changes, "XDG_DATA_DIRS", "/usr/local/share/:/usr/share/:/var/lib/snapd/desktop"));
    CHECK(unsets(changes, "XDG_DATA_DIRS_VSCODE_SNAP_ORIG"));
    CHECK(sets(changes, "PATH", "/home/user/.local/bin:/usr/bin"));
    CHECK(unsets(changes, "LD_LIBRARY_PATH"));
    CHECK(unsets(changes, "SNAP"));
    CHECK(unsets(changes, "SNAP_NAME"));
    CHECK(unsets(changes, "GIO_LAUNCHED_DESKTOP_FILE"));
    CHECK_FALSE(changeFor(changes, "HOME"));
    CHECK_FALSE(changeFor(changes, "LANG"));
}

TEST_CASE("snap environment: values outside a snap are left alone")
{
    std::map<std::string, std::string> env = {
        {"GSETTINGS_SCHEMA_DIR", "/home/user/Repos/u-studio-video-editor/builddir/data"},
        {"XDG_DATA_HOME", "/home/user/.local/share"},
        {"PATH", "/usr/local/bin:/usr/bin"},
        {"LD_PRELOAD", "/lib64/libavutil.so.60 /lib64/libx264.so.165"},
        {"SNAPSHOT_DIR", "/tmp/x"}, // not a SNAP_ variable
    };
    CHECK(snapEnvironmentFixes(env, "/home/user").empty());
}

TEST_CASE("snap environment: space-separated LD_PRELOAD loses only its snap entry")
{
    std::map<std::string, std::string> env = {{"LD_PRELOAD", "/snap/x/lib/a.so /lib64/b.so"}};
    std::vector<EnvChange> changes = snapEnvironmentFixes(env, "/home/user");
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].value == std::optional<std::string>("/lib64/b.so"));
}
