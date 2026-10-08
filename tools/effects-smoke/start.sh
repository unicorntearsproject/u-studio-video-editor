# Sourced by the smoke's steps files: the check helpers, a private Xvfb and
# AT-SPI bus, the generated test clip (CLIP_SIZE and CLIP_SECONDS, 1280x720
# and 6 s by default) and the editor, started and fitted to the screen.
OUT=$SMOKE_OUT; M=$SMOKE_MEDIA
FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }
check() { name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }
d() { python3 "$SMOKE_DRIVE/drive.py" "$@" 2>/dev/null; }
shot() { import -window root "$OUT/$1.png" 2>/dev/null; }
saved_has() { grep -q "$1" "$OUT/smoke.ustudio"; }
saved_lacks() { ! grep -q "$1" "$OUT/smoke.ustudio"; }

dbus-update-activation-environment XDG_RUNTIME_DIR="$SMOKE_RUNTIME" GIO_USE_VFS=local

exec 3>"$OUT/display"
Xvfb -displayfd 3 -screen 0 "${SMOKE_SCREEN:-1920x1080x24}" -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
XVFB=$!
for _ in $(seq 1 40); do [ -s "$OUT/display" ] && break; sleep 0.25; done
export DISPLAY=":$(cat "$OUT/display")"
/usr/libexec/at-spi-bus-launcher --launch-immediately >/dev/null 2>&1 &
LAUNCHER=$!
for _ in $(seq 1 40); do
    gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress \
        >/dev/null 2>&1 && break
    sleep 0.25
done
/usr/libexec/at-spi2-registryd >/dev/null 2>&1 &
REGISTRY=$!
sleep 1

# Test media: generated, never real footage (CLAUDE.md).
mkdir -p "$M"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=${CLIP_SIZE:-1280x720}:rate=30:duration=${CLIP_SECONDS:-6} \
    -f lavfi -i sine=frequency=440:duration=${CLIP_SECONDS:-6}:sample_rate=48000 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$M/clip.mp4"

SDL_AUDIODRIVER=dummy USTUDIO_LOG_LEVEL=debug GTK_A11Y=atspi USTUDIO_SMOKE_RUN="$OUT" \
    "$SMOKE_APP" >"$OUT/app.log" 2>&1 &
APP=$!
for _ in $(seq 1 60); do
    python3 -c "import sys; sys.path.insert(0, '$SMOKE_DRIVE'); import drive; sys.exit(0 if drive.app_node() else 1)" \
        2>/dev/null && break
    sleep 0.5
done
# The whole screen, as the editor opens maximised on a desktop: wide
# enough for the inspector to dock beside the picture.
python3 "$SMOKE_DRIVE/fitwin.py" >/dev/null 2>&1
sleep 2
