#!/usr/bin/env python3
"""headless-guard (tests/meson.build): fails when the test setup stops applying.

A test must not see the desktop (WAYLAND_DISPLAY, DISPLAY :0, no
USTUDIO_HEADLESS: tools/test-headless.sh didn't run), nor a secret from the
shell that ran meson test (tools/test_env.py's SECRET_NAME). A secret is
named, never printed.
"""
import os
import sys

sys.path.insert(0, sys.argv[1])  # tools/
from test_env import SECRET_NAME  # noqa: E402

problems = []
if os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY") == ":0" or not os.environ.get("USTUDIO_HEADLESS"):
    problems.append(f"tests can see the desktop (WAYLAND_DISPLAY={os.environ.get('WAYLAND_DISPLAY', '')} "
                    f"DISPLAY={os.environ.get('DISPLAY', '')}): run them through the headless test setup")
secrets = sorted(name for name in os.environ if SECRET_NAME.search(name))
if secrets:
    problems.append("tests can see secret-looking variables (values not shown): " + ", ".join(secrets) +
                    "; tools/test-headless.sh should have dropped them")
for problem in problems:
    print(problem)
sys.exit(1 if problems else 0)
