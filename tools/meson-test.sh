#!/bin/sh
# `meson test` from a clean environment: the way to run the tests (just test,
# just asan/tsan, the drop-in recipes and landing gates all use it). meson
# logs its whole environment into meson-logs/testlog.{txt,json}, and a
# developer's shell holds keys and tokens; tools/test_env.py keeps only an
# allowlist. Arguments are meson test's: tools/meson-test.sh -C builddir ...
exec python3 -I "$(dirname "$0")/test_env.py" meson "$@"
