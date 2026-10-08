#!/usr/bin/env python3
"""The environment tests run in: an allowlist, so no secret reaches a test or its log.

    test_env.py meson <meson test args...>   (tools/meson-test.sh)
    test_env.py wrap <script> <args...>      (tools/test-headless.sh)

meson writes the whole environment of the process that runs `meson test`
into meson-logs/testlog.txt ("Inherited environment:") and testlog.json
(each test's env), and nothing in meson.build can stop it. A developer's or
an agent's shell holds API keys and tokens, so on 2026-10-08 every builddir's
testlog held them. `meson` mode starts meson from this allowlist alone
(`env -i` plus the names below), so the logs never see anything else.

`wrap` mode covers a plain `meson test` too: the wrapper's parent is the
meson process, so a variable with the same value there came from the caller's
shell, while one that is new or changed came from the test's own env: in
meson.build or from meson itself (MESON_TEST_ITERATION, MALLOC_PERTURB_).
Inherited names off the allowlist are dropped; the test's own are kept,
without naming them here. Secret-looking names are dropped always.
Linux-only like the wrapper (Xvfb, dbus-run-session): /proc/<pid>/environ.
"""
import os
import re
import sys

ALLOWED = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TZ", "TMPDIR", "LANG", "LANGUAGE", "NO_COLOR",
    "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_DATA_DIRS",
    "XDG_CONFIG_DIRS", "XDG_RUNTIME_DIR",
    "PKG_CONFIG_PATH", "LD_LIBRARY_PATH", "MESON_TESTTHREADS", "SDL_AUDIODRIVER",
    # just asan / just tsan
    "LD_PRELOAD", "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS",
}
ALLOWED_PREFIXES = ("LC_", "USTUDIO_", "MLT_", "CCACHE_")
# Inside a harness that already made a private display and bus (USTUDIO_HEADLESS
# set: the smoke harnesses), those are the test's and stay.
ALLOWED_HEADLESS = {"DISPLAY", "GDK_BACKEND", "GDK_DEBUG", "GSETTINGS_BACKEND", "DBUS_SESSION_BUS_ADDRESS",
                    "GIO_USE_VFS"}

# Never passed on, allowlisted or not; tests/headless_guard.py fails on them.
SECRET_NAME = re.compile(r"^OPENAI_|_TOKEN$|_SECRET$|_API_KEY$|^AWS_SECRET")


def allowed(name, env):
    if SECRET_NAME.search(name):
        return False
    if name in ALLOWED or name.startswith(ALLOWED_PREFIXES):
        return True
    return bool(env.get("USTUDIO_HEADLESS")) and name in ALLOWED_HEADLESS


def parent_environ():
    try:
        with open(f"/proc/{os.getppid()}/environ", "rb") as f:
            raw = f.read()
    except OSError:
        return None
    env = {}
    for entry in raw.split(b"\0"):
        name, sep, value = entry.partition(b"=")
        if sep:
            env[os.fsdecode(name)] = os.fsdecode(value)
    return env


def main(argv):
    if len(argv) < 2 or argv[1] not in ("meson", "wrap"):
        sys.exit("usage: test_env.py meson <meson test args...> | wrap <script> <args...>")
    env = dict(os.environ)
    if argv[1] == "meson":
        clean = {k: v for k, v in env.items() if allowed(k, env)}
        os.execvpe("meson", ["meson", "test", *argv[2:]], clean)
    parent = parent_environ()
    clean = {}
    for name, value in env.items():
        inherited = parent is None or parent.get(name) == value
        if SECRET_NAME.search(name) or (inherited and not allowed(name, env)):
            continue
        clean[name] = value
    clean["USTUDIO_TEST_ENV_CLEAN"] = "1"
    os.execve("/bin/sh", ["sh", *argv[2:]], clean)


if __name__ == "__main__":
    main(sys.argv)
