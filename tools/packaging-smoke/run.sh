#!/bin/sh
# Packaging smoke test: drives the installed Flatpak or Snap end to end.
#
#   tools/packaging-smoke/run.sh <flatpak|snap> <outdir> [--cleanup]
#
# The installed package must already be there (flatpak install --user ... or
# snap install --dangerous ...). Everything runs on a private Xvfb display
# with its own D-Bus session and a clean environment, so nothing appears on
# the desktop. Audio goes to SDL's disk driver, so nothing plays on the
# speakers. Needs Xvfb, ffmpeg, python3 with gi (Atspi) and python-xlib,
# ImageMagick's `import`, and the AT-SPI bus (at-spi-bus-launcher,
# at-spi2-registryd).
#
# Flatpak runs are isolated from the user's own state: the app runs with
# HOME set to a throwaway directory ($SMOKE_HOME, default
# ~/.cache/ustudio-smoke-home), so its ~/.var/app/<id> settings, logs,
# autosaves and proxies, and the test media and projects, all live under
# that directory. --cleanup deletes it afterwards.
#
# SMOKE_FLATPAK_USER_DIR picks the Flatpak installation that holds the app
# under test (default: the user's own, ~/.local/share/flatpak). Test a new
# bundle in a separate installation instead of upgrading the user's:
#   FLATPAK_USER_DIR=<dir> flatpak --user remote-add flathub <flathub repo>
#   FLATPAK_USER_DIR=<dir> flatpak --user install <bundle or test repo>
#   SMOKE_FLATPAK_USER_DIR=<dir> tools/packaging-smoke/run.sh flatpak <outdir>
#
# GPU checks (ADR-019: the probe, the pipeline, the Settings row, a
# playback memory soak of SMOKE_GPU_SOAK seconds, default 180) run unless
# SMOKE_GPU=0, e.g. on a machine without a usable GPU.
#
# Snap runs can't be isolated that way (snapd takes the home directory from
# the passwd database, not $HOME): they use the real ~/snap/<name> data, and
# the media goes to ~/ustudio-smoke-media (not a hidden folder: the home plug
# can't read those). --cleanup deletes both, so only use it for a snap
# nobody else uses on this machine.
#
# Screenshots, logs and the results summary land in <outdir>. Exit status 0
# means every check passed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
RUNNER=${1:?usage: run.sh <flatpak|snap> <outdir> [--cleanup]}
OUT=$(realpath -m "${2:?usage: run.sh <flatpak|snap> <outdir> [--cleanup]}")
CLEANUP=${3:-}
case "$RUNNER" in flatpak|snap) ;; *) echo "runner must be flatpak or snap" >&2; exit 2 ;; esac
mkdir -p "$OUT"
REAL_HOME=$HOME
FLATPAK_DIR=${SMOKE_FLATPAK_USER_DIR:-$REAL_HOME/.local/share/flatpak}
APP_ID=${SMOKE_APP_ID:-com.ustudio.VideoEditor}
SNAP_NAME=${SMOKE_SNAP_NAME:-u-studio-video-editor}
if [ "$RUNNER" = flatpak ]; then
    RUN_HOME=${SMOKE_HOME:-$REAL_HOME/.cache/ustudio-smoke-home}
    MEDIA=$RUN_HOME/media
else
    RUN_HOME=$REAL_HOME
    MEDIA=$REAL_HOME/ustudio-smoke-media
fi
mkdir -p "$RUN_HOME" "$MEDIA"

# The services the private D-Bus session activates get their own runtime
# dir: steps.sh makes it their activation environment first thing. With the
# desktop's (/run/user/<uid>), an xdg-document-portal activated there
# mounted its FUSE filesystem over the desktop's /run/user/<uid>/doc and
# unmounted it on exit, leaving the desktop's portal without its mount
# (2026-09-27). The processes steps.sh starts keep the real runtime dir,
# which AT-SPI needs (a private one for everything breaks the a11y bus).
# Same method as tools/titles-smoke (4ff4065, docs/developer/testing.md).
# flatpak run also gets --no-documents-portal (drive.py, steps.sh), and the
# desktop's doc mount is compared before and after the run.
REAL_RUNTIME=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/ust-rt.XXXXXX")
trap 'rm -rf "$RUNTIME"' EXIT
DOC_BEFORE=$(findmnt -n -o SOURCE,FSTYPE "$REAL_RUNTIME/doc" 2>/dev/null || echo "not mounted")

# env -i: the caller's environment may point XDG_DATA_HOME and GTK/GIO
# module paths elsewhere (a VS Code snap terminal does), which would move
# `flatpak --user` to another installation and break GTK in the test.
# DISABLE_WAYLAND: a snap's desktop-launch (the gnome extension's) exports
# GDK_BACKEND=wayland whenever $XDG_RUNTIME_DIR/../wayland-0 exists, over
# our GDK_BACKEND=x11, which would put the app on the desktop's own
# compositor (read in gnome-46-2404's command-chain, 2026-10-08).
env -i HOME="$RUN_HOME" USER="$USER" PATH=/usr/local/bin:/usr/bin:/bin:/snap/bin LANG=C.UTF-8 DISABLE_WAYLAND=1 \
    XDG_RUNTIME_DIR="$REAL_RUNTIME" SMOKE_RUNTIME="$RUNTIME" GIO_USE_VFS=local \
    FLATPAK_USER_DIR="$FLATPAK_DIR" \
    XDG_DATA_DIRS="$FLATPAK_DIR/exports/share:/var/lib/flatpak/exports/share:/usr/local/share:/usr/share" \
    SMOKE_GPU="${SMOKE_GPU:-1}" SMOKE_GPU_SOAK="${SMOKE_GPU_SOAK:-180}" \
    SMOKE_RUNNER="$RUNNER" SMOKE_OUT="$OUT" SMOKE_MEDIA="$MEDIA" SMOKE_REAL_HOME="$REAL_HOME" \
    SMOKE_APP_ID="$APP_ID" SMOKE_SNAP_NAME="$SNAP_NAME" SMOKE_HERE="$HERE" GTK_A11Y=atspi \
    dbus-run-session -- sh "$HERE/steps.sh" 2>&1 | tee "$OUT/run.log"
FAILED=$(tail -1 "$OUT/run.log" | sed -n 's/^RESULT: \([0-9]*\) failed.*/\1/p')

DOC_AFTER=$(findmnt -n -o SOURCE,FSTYPE "$REAL_RUNTIME/doc" 2>/dev/null || echo "not mounted")
if [ "$DOC_BEFORE" = "$DOC_AFTER" ]; then
    echo "PASS the desktop's $REAL_RUNTIME/doc is untouched ($DOC_AFTER)" | tee -a "$OUT/run.log"
else
    echo "FAIL the desktop's $REAL_RUNTIME/doc changed: before '$DOC_BEFORE', after '$DOC_AFTER'" | tee -a "$OUT/run.log"
    FAILED=$((${FAILED:-0} + 1))
fi

if [ "$CLEANUP" = "--cleanup" ]; then
    if [ "$RUNNER" = flatpak ]; then
        rm -rf "$RUN_HOME"
        echo "cleanup: removed $RUN_HOME (test media, projects and the app's test state)"
    else
        rm -rf "$MEDIA" "$REAL_HOME/snap/$SNAP_NAME"
        echo "cleanup: removed $MEDIA and $REAL_HOME/snap/$SNAP_NAME"
    fi
fi
[ "$FAILED" = 0 ]
