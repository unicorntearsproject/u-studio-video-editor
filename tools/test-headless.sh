#!/bin/sh
# meson's default test wrapper (tests/meson.build, add_test_setup): every
# test runs on a private X display and a private D-Bus session, never on the
# desktop the developer is sitting at (owner rule, 2026-10-08: a plain
# `meson test` opened GTK windows on the real desktop). The portal-safe
# recipe from docs/developer/testing.md: services the private bus activates
# (xdg-document-portal) get a scratch runtime dir, or they'd mount over the
# desktop's /run/user/<uid>/doc and unmount it on exit.
set -u
if [ -n "${USTUDIO_HEADLESS:-}" ]; then
    exec "$@" # already inside one (a smoke harness, or this script)
fi
unset WAYLAND_DISPLAY
# The test's own preloads (just asan's FFmpeg and MLT modules) are for the
# test, not for Xvfb and dbus-daemon: kept aside and put back for it.
PRELOAD=${LD_PRELOAD:-}
export PRELOAD
export GDK_BACKEND=x11 GDK_DEBUG=no-portals USTUDIO_HEADLESS=1 GSETTINGS_BACKEND=memory
work=$(mktemp -d "${TMPDIR:-/tmp}/ust-test.XXXXXX") || exit 1
xvfb=""
cleanup() {
    [ -n "$xvfb" ] && kill "$xvfb" 2>/dev/null
    # A private document portal (app-portal-path) unmounts its runtime/doc
    # just after the session ends: retry until the folder can go.
    i=0
    while ! rm -rf "$work" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.05; i=$((i + 1)); done
}
trap cleanup EXIT INT TERM
if command -v Xvfb >/dev/null 2>&1; then
    # -displayfd: Xvfb picks a free display and writes its number when ready.
    env -u LD_PRELOAD Xvfb -displayfd 3 -nolisten tcp -screen 0 1280x800x24 3>"$work/display" >"$work/xvfb.log" 2>&1 &
    xvfb=$!
    i=0
    while [ ! -s "$work/display" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
    if [ -s "$work/display" ]; then
        export DISPLAY=":$(cat "$work/display")"
    else
        unset DISPLAY
    fi
else
    unset DISPLAY # no Xvfb: GTK tests can't open a display, rather than use the desktop's
fi
mkdir -p "$work/runtime"
chmod 700 "$work/runtime"
env -u LD_PRELOAD dbus-run-session -- sh -c '
    env -u LD_PRELOAD dbus-update-activation-environment XDG_RUNTIME_DIR="$0" GIO_USE_VFS=local \
        ${DISPLAY:+DISPLAY="$DISPLAY"} GDK_BACKEND=x11 >/dev/null 2>&1
    exec env ${PRELOAD:+LD_PRELOAD="$PRELOAD"} "$@"' "$work/runtime" "$@" &
child=$!
wait "$child"
