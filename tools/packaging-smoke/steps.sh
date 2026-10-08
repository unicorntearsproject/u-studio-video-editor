#!/bin/sh
# The smoke-test steps; run.sh runs this inside a private D-Bus session.
# Every check prints PASS or FAIL; the last line is "RESULT: <n> failed".
set -u
H=$SMOKE_HERE; OUT=$SMOKE_OUT; M=$SMOKE_MEDIA
FAILED=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }
check() { name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }
d() { python3 "$H/drive.py" "$@" 2>/dev/null; }
status() { d status; }
# Only this run's processes: other copies of the editor may be running on the
# machine (the owner's, other agents'). drive.py tags ours via the environment.
ours() {
    for p in $(pgrep -x "$1"); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "USTUDIO_SMOKE_RUN=$OUT" && echo "$p"
    done | head -1
}
editor_pid() { ours u-studio-video-; }
no_qt() { p=$1; [ -n "$p" ] && [ "$(grep -ciE 'libqt|qt6' "/proc/$p/maps")" = 0 ]; }
last_status_is() { case "$(status)" in "$1"*) true ;; *) echo "  status: $(status)"; false ;; esac; }
import_file() { d act import; sleep 1.5; d loc "$1"; d enter; sleep 3; }

# Activated services (the portals) run in run.sh's private runtime dir, never
# the desktop's: an activated xdg-document-portal would otherwise mount over
# the desktop's /run/user/<uid>/doc.
dbus-update-activation-environment XDG_RUNTIME_DIR="$SMOKE_RUNTIME" GIO_USE_VFS=local

# --- private X display and accessibility bus
exec 3>"$OUT/display"
Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp >"$OUT/xvfb.log" 2>&1 &
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
echo "display $DISPLAY, runner $SMOKE_RUNNER"

# --- test media: generated, never real footage (CLAUDE.md)
mkdir -p "$M"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=10 \
    -f lavfi -i sine=frequency=440:duration=10:sample_rate=48000 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$M/clip1080.mp4"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=3840x2160:rate=30:duration=6 \
    -f lavfi -i sine=frequency=660:duration=6:sample_rate=48000 \
    -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest "$M/clip4k.mp4"
ffmpeg -loglevel error -y -f lavfi -i "color=c=magenta:size=640x360" -frames:v 1 "$M/logo.png"
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=1280x720" -frames:v 1 -q:v 3 "$M/still.jpg"
AUDIO="$M/sdl-audio.raw"
LAUNCH="GDK_DEBUG=no-portals SDL_AUDIODRIVER=disk SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=$AUDIO SDL_DISKAUDIOFILE=$AUDIO"

# --- 0: the installed package carries nothing from a test run or the build machine
if [ "$SMOKE_RUNNER" = flatpak ]; then
    TREE="$(flatpak info --user --show-location "$SMOKE_APP_ID")/files"
else
    TREE="/snap/$SMOKE_SNAP_NAME/current"
fi
check "installed package is clean ($TREE)" python3 "$H/../check_bundle_clean.py" "$TREE" --home "$SMOKE_REAL_HOME"

# --- 0b: GPU (ADR-019): the module ships, and the probe passes in the sandbox
# The GPU playback and export checks mean something only while the pipeline
# is on: the app stays on the CPU when the probe fails, and then there's no
# GPU playback or export to judge.
gpu_on() { grep -q "\[gpu\] GPU pipeline on (" "$OUT/app.log"; }
gpu_on_and() { gpu_on || { echo "  (the GPU pipeline never came on)"; return 1; }; "$@"; }
if [ "$SMOKE_GPU" = 1 ]; then
    check "MLT movit module in the package" sh -c "find '$TREE' -name 'libmltmovit.so' | grep -q ."
    if [ "$SMOKE_RUNNER" = flatpak ]; then
        probe=$(flatpak run --user --no-documents-portal --command=u-studio-render "$SMOKE_APP_ID" --gpu-probe 2>/dev/null); rc=$?
        echo "  gpu-probe (exit $rc): $probe"
        check "gpu-probe passes in the sandbox" sh -c "[ $rc = 0 ] && echo '$probe' | grep -q '\"status\":\"ok\"'"
        probe=$(flatpak run --user --no-documents-portal --env=USTUDIO_EGL_LIBRARY=/nonexistent/libEGL.so.1 --command=u-studio-render \
            "$SMOKE_APP_ID" --gpu-probe 2>/dev/null); rc=$?
        check "gpu-probe fails cleanly without EGL (exit 1)" sh -c "[ $rc = 1 ] && echo '$probe' | grep -q 'no EGL library'"
    fi
fi

# --- 0c: the Flatpak starts with GPU acceleration off (a vendor override,
# docs/developer/packaging.md "GPU acceleration's default"). This run's HOME
# is fresh, so nothing is saved yet and the key reads its default. The GPU
# checks below then switch it on, as a user would.
if [ "$SMOKE_RUNNER" = flatpak ]; then
    gsettings_in() { flatpak run --user --no-documents-portal --command=gsettings "$SMOKE_APP_ID" "$@" 2>&1; }
    gpu_default=$(gsettings_in get com.ustudio.VideoEditor gpu-acceleration)
    echo "  gpu-acceleration on a fresh install: $gpu_default"
    check "a fresh install starts with GPU acceleration off" test "$gpu_default" = false
    if [ "$SMOKE_GPU" = 1 ]; then
        gsettings_in set com.ustudio.VideoEditor gpu-acceleration true
        check "GPU acceleration switched on for the GPU checks" \
            test "$(gsettings_in get com.ustudio.VideoEditor gpu-acceleration)" = true
    fi
fi

# --- 1: launch
d launch $LAUNCH
sleep 2
d act new-project
sleep 1
d shot 01-launch
check "curated MLT module directory in use" grep -q "curated MLT module dir" "$OUT/app.log"
check "no Qt mapped at launch" no_qt "$(editor_pid)"
if [ "$SMOKE_GPU" = 1 ]; then
    d waitlog "[gpu] GPU pipeline on" 30
    check "GPU pipeline on at startup" grep -q "\[gpu\] GPU pipeline on (" "$OUT/app.log"
fi

# --- 2: import video and stills
import_file "$M/clip1080.mp4"; check "import H.264/AAC video" last_status_is "Imported"
d press "Add track"; sleep 1; d act clear-selection; d act active-track-top; d act seek-home
import_file "$M/logo.png"; check "import PNG still" last_status_is "Imported"
d press "Add track"; sleep 1; d act clear-selection; d act active-track-top; d act seek-home
import_file "$M/still.jpg"; check "import JPEG still" last_status_is "Imported"
d shot 02-imported

# --- 3: transform the top picture on the preview (MLT plus module's affine)
d click 900 300; sleep 1
d drag 960 380 760 300; sleep 1.5
d act transform-rotate-cw; sleep 2
d shot 03-transform
check "transform applied (affine)" grep -q "clip transforms applied in place" "$OUT/app.log"

# --- 4: playback with sound, through SDL's disk driver
before=$(stat -c %s "$AUDIO" 2>/dev/null || echo 0)
d act seek-home; d act play-pause; sleep 1.5
check "no Qt mapped while playing" no_qt "$(editor_pid)"
sleep 1.5; d act play-pause; sleep 1
tail -c +$((before + 1)) "$AUDIO" > "$OUT/play.raw"
tone=$(python3 "$H/tone.py" "$OUT/play.raw"); echo "  $tone"
check "playback produced the 440 Hz tone" sh -c "echo '$tone' | grep -qE 'audible [1-9].*~4[34][0-9] Hz'"

# --- 4b: GPU: the Settings row, and a looped-playback memory soak
if [ "$SMOKE_GPU" = 1 ]; then
    d press "Settings"; sleep 2
    d press "Performance"; sleep 1.5
    row=$(d rowtext "GPU acceleration"); echo "  settings row: $row"
    d shot 04b-settings
    check "Settings shows GPU acceleration On: <renderer>" sh -c "echo \"\$1\" | grep -q '| On: '" _ "$row"
    python3 "$H/drive.py" keysym 0xff1b 2>/dev/null; sleep 1   # Escape closes the dialog

    # Loop the first 9 s (three tracks, one transformed: the movit graph)
    # and sample the editor's RSS; the slope after a 30 s warm-up must be
    # flat. The movit.convert leak this guards against was ~6 MB/min here.
    d act seek-home; d act loop-set-in
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27; do d act step-forward-10 >/dev/null; done
    d act loop-set-out; d act seek-home
    pid=$(editor_pid)
    d act play-pause
    : > "$OUT/rss.txt"
    t=0
    while [ "$t" -le "$SMOKE_GPU_SOAK" ]; do
        echo "$t $(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)" >> "$OUT/rss.txt"
        sleep 10; t=$((t + 10))
    done
    d act play-pause; sleep 1; d press "Clear loop"
    soak=$(python3 - "$OUT/rss.txt" <<'EOF'
import sys
rows = [tuple(map(float, line.split())) for line in open(sys.argv[1]) if len(line.split()) == 2]
rows = [r for r in rows if r[0] >= 30]
n = len(rows)
if n < 3:
    print("slope n/a (too few samples)"); sys.exit()
mx = sum(r[0] for r in rows) / n
my = sum(r[1] for r in rows) / n
slope = sum((r[0] - mx) * (r[1] - my) for r in rows) / sum((r[0] - mx) ** 2 for r in rows)  # kB/s
print(f"slope {slope * 60 / 1024:+.2f} MB/min over {rows[-1][0] - rows[0][0]:.0f} s, RSS {rows[0][1] / 1024:.0f} -> {rows[-1][1] / 1024:.0f} MB")
EOF
)
    echo "  GPU playback soak: $soak"
    check "RSS flat during GPU playback (< 1 MB/min)" gpu_on_and python3 -c "
import re, sys; m = re.search(r'slope ([+-][\d.]+) MB/min', sys.argv[1]); sys.exit(0 if m and float(m.group(1)) < 1.0 else 1)" "$soak"
    check "GPU pipeline stayed on" gpu_on_and sh -c "! grep -qE '\[gpu\] (falling back|the render thread couldn)' '$OUT/app.log'"
fi

# --- 5: edit, undo, redo, save
# No selection first: with exactly one clip selected (the picture from
# step 3), active-track-bottom moves that clip down a track instead.
d act clear-selection; d act active-track-bottom; d act seek-home
for _ in 1 2 3 4 5 6 7 8 9; do d act step-forward-10 >/dev/null; done
d act split-at-playhead; sleep 1
d act undo; sleep 0.5; check "undo split" last_status_is "Undid: Split clip"
d act redo; sleep 0.5; check "redo split" last_status_is "Redid: Split clip"
d act save; sleep 1.5; d settext "$M/smoke.ustudio"; d enter; sleep 2
check "save project" last_status_is "Saved"

# --- 6: close, reopen (the last project reopens by itself)
d close; sleep 4
check "clean exit" test -z "$(editor_pid)"
d launch $LAUNCH; sleep 3
check "last project reopens" last_status_is "Opened"
d shot 06-reopened

# --- 7: render with the default (High quality) profile
d press "Render…"; sleep 2; d press Save exact; sleep 3
check "no Qt mapped while rendering" no_qt "$(editor_pid)"
d waitlog "status: Rendered" 240
check "render finished" last_status_is "Rendered"
if [ "$SMOKE_GPU" = 1 ]; then
    # A GPU export logs nothing on success; these are its two fallbacks.
    check "export ran on the GPU (no CPU fallback)" \
        gpu_on_and sh -c "! grep -qE \"\[gpu\] (exporting on the CPU|the export's render thread couldn't)\" '$OUT/app.log'"
fi
render=$(ls "$M"/smoke-high-quality-*.mp4 2>/dev/null | tail -1)
if [ -n "$render" ]; then
    streams=$(ffprobe -v error -show_entries stream=codec_name,width,height -of csv=p=0 "$render" | tr '\n' ' ')
    echo "  render streams: $streams"
    check "render is 1080p H.264 + AAC" sh -c "echo '$streams' | grep -q 'h264,1920,1080' && echo '$streams' | grep -q aac"
    ffmpeg -loglevel error -y -ss 1 -i "$render" -frames:v 1 "$OUT/07-render-1s.png"
else
    fail "render file exists"
fi

# --- 8: proxy for 4K footage, made by u-studio-render
d press "Add track"; sleep 1; d act clear-selection; d act active-track-top; d act seek-home
import_file "$M/clip4k.mp4"
d press "Create Proxies"; sleep 2
check "no Qt mapped in u-studio-render" no_qt "$(ours u-studio-render)"
d waitlog "status: Proxy ready" 120
check "proxy ready" last_status_is "Proxy ready"

# --- 8b: image sequence (numbered PNGs as one clip; MLT's gdk/pixbuf module)
check "MLT gdk module in the package" sh -c "find '$TREE' -name 'libmltgdk.so' | grep -q ."
mkdir -p "$M/seq"
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30:duration=1 "$M/seq/frame_%04d.png"
d press "Add track"; sleep 1; d act clear-selection; d act active-track-top; d act seek-home
d act import-image-sequence; sleep 1.5; d loc "$M/seq/frame_0010.png"; d enter; sleep 4
seq_status=$(status); echo "  status: $seq_status"
check "import a 30-image sequence" sh -c "echo \"\$1\" | grep -qE '^Imported .*: 30 '" _ "$seq_status"
d shot 08b-sequence
d act save; sleep 1.5

# --- 8c: the titles add-on (a Flatpak extension of the app), when it's
# installed in the same installation. A built-in template from the source
# tree stands in for a user's title.
if [ "$SMOKE_RUNNER" = flatpak ] && flatpak info --user "$SMOKE_APP_ID.DropIn.Titles" >/dev/null 2>&1; then
    check "titles drop-in loaded from its extension" \
        grep -q "\[drop-ins\] Loaded titles from /app/lib/u-studio/extensions/Titles/" "$OUT/app.log"
    cp "$H/../../drop-ins/titles/data/templates/lower-third-two-lines.ustitle" "$M/lower-third.ustitle"
    d press "Add track"; sleep 1; d act clear-selection; d act active-track-top; d act seek-home
    import_file "$M/lower-third.ustitle"
    title_status=$(status); echo "  status: $title_status"
    check "import a title" sh -c "echo \"\$1\" | grep -q '^Imported'" _ "$title_status"
    for _ in 1 2 3; do d act step-forward-10 >/dev/null; done
    sleep 1; d shot 08c-title
    # Rendered by the sandbox's u-studio-render, which loads the drop-in and
    # its MLT module from the extension: the title must have visible alpha.
    flatpak run --user --no-documents-portal --command=u-studio-render "$SMOKE_APP_ID" \
        --title-export "$M/lower-third.ustitle" "$M/lower-third.mov" prores --seconds 2 >"$OUT/title-export.log" 2>&1
    pix=$(ffprobe -v error -select_streams v -show_entries stream=pix_fmt -of csv=p=0 "$M/lower-third.mov" 2>/dev/null)
    # metadata=print logs at info level, so not -v error.
    alpha=$(ffmpeg -v info -ss 1.5 -i "$M/lower-third.mov" -frames:v 1 -vf alphaextract,signalstats,metadata=print:key=lavfi.signalstats.YMAX \
        -f null - 2>&1 | sed -n 's/.*YMAX=\([0-9]*\).*/\1/p' | tail -1)
    echo "  title export: pix_fmt $pix, alpha max ${alpha:-none}"
    check "title renders with alpha in the sandbox" sh -c "echo '$pix' | grep -q yuva && [ '${alpha:-0}' -gt 0 ]"
    d act select-all; d act titles-edit
    # The designer can take several seconds to start in the sandbox.
    for _ in $(seq 1 20); do designer=$(ours u-studio-titles); [ -n "$designer" ] && break; sleep 1; done
    check "Edit Title opens U-Stu Titles in the sandbox" test -n "$designer"
    d shot 08c-designer
    [ -n "$designer" ] && kill "$designer"
    d act clear-selection; d act save; sleep 1.5
    # Template packs (ADR-020, from 0.68): the designer installs one without
    # a window. A build without libarchive refuses every pack, which is the
    # packaging mistake this catches. The repo holds no binary packs.
    python3 "$H/../make_test_pack.py" "$M/smoke-pack.zip"
    pack=$(flatpak run --user --no-documents-portal --command=/app/lib/u-studio/extensions/Titles/bin/u-studio-titles \
        "$SMOKE_APP_ID" --install-pack "$M/smoke-pack.zip" 2>&1); rc=$?
    echo "  install-pack (exit $rc): $pack"
    check "U-Stu Titles installs a template pack (libarchive in the extension)" \
        sh -c "[ $rc = 0 ] && echo \"\$1\" | grep -q '^installed test/smoke-pack '" _ "$pack"
    # Animated (Lottie) layers (ADR-021, from 0.78): the bell template's
    # ringing bell, drawn by the extension's ThorVG. It rings in bursts with
    # rests between (one resting pose), so count the distinct pictures of
    # its box over the hold (frames 20-80): without ThorVG it's the pill's
    # flat fill, one picture.
    mkdir -p "$M/bell"
    cp "$H/../../drop-ins/titles/data/templates/subscribe-bell.ustitle" "$M/bell/"
    cp -r "$H/../../drop-ins/titles/data/templates/animations" "$M/bell/"
    flatpak run --user --no-documents-portal --command=u-studio-render "$SMOKE_APP_ID" \
        --title-export "$M/bell/subscribe-bell.ustitle" "$M/bell.mov" prores --seconds 3 >"$OUT/bell-export.log" 2>&1
    bell_pictures=$(ffmpeg -v error -i "$M/bell.mov" -vf "select='between(n,20,80)',crop=120:120:180:858" -vsync 0 \
        -f framemd5 - 2>/dev/null | grep -v '^#' | awk -F', ' '{print $6}' | sort -u | wc -l)
    echo "  bell box: $bell_pictures distinct pictures over frames 20-80"
    check "an animated (Lottie) title layer renders in the sandbox" test "$bell_pictures" -ge 5
fi

# --- 8d: the effects add-on (a Flatpak extension carrying frei0r and MLT's
# frei0r module), when it's installed in the same installation. Hooks and
# the Browser steps are VE Effects' (tools/effects-smoke/steps.sh).
if [ "$SMOKE_RUNNER" = flatpak ] && flatpak info --user "$SMOKE_APP_ID.DropIn.Effects" >/dev/null 2>&1; then
    check "effects drop-in loaded from its extension" \
        grep -q "\[drop-ins\] Loaded effects from /app/lib/u-studio/extensions/Effects/" "$OUT/app.log"
    check "effects registered in the editor" grep -q "\[effects\] registered in editor" "$OUT/app.log"
    probe=$(flatpak run --user --no-documents-portal --command=u-studio-render "$SMOKE_APP_ID" \
        --probe-effect frei0r.glow 2>/dev/null | tail -1)
    echo "  probe-effect frei0r.glow: $probe"
    check "frei0r Glow is usable in the sandbox" sh -c "echo '$probe' | grep -q '\"status\":\"ok\"'"
    check "the health scan ran" grep -q "\[effects\] health scan" "$OUT/app.log"
    # A render before and after adding Glow, at the same moment: Glow
    # brightens the picture, so the mean luma must rise.
    # After a render the header button reads "Open Render" until the next
    # start, which a relaunch gives (the last project reopens by itself).
    # The driver's own lines go to stderr: only the file name is returned.
    render_now() {
        before=$(ls "$M"/smoke-high-quality-*.mp4 2>/dev/null | wc -l)
        { d close; sleep 4; d launch $LAUNCH; sleep 3
          d press "Render…"; sleep 2; d press Save exact; sleep 3; } >&2
        # A render appears under its name only when done (temp file +
        # rename), and the log already holds step 7's "Rendered".
        for _ in $(seq 1 240); do [ "$(ls "$M"/smoke-high-quality-*.mp4 2>/dev/null | wc -l)" -gt "$before" ] && break; sleep 1; done
        ls -t "$M"/smoke-high-quality-*.mp4 2>/dev/null | head -1
    }
    luma() {
        ffmpeg -v info -ss 2.5 -i "$1" -frames:v 1 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG -f null - 2>&1 |
            sed -n 's/.*YAVG=\([0-9.]*\).*/\1/p' | tail -1
    }
    plain=$(render_now)
    d act clear-selection; d act select-all; sleep 1
    d act effects-browser; sleep 1
    SMOKE_DRIVE="$H" python3 "$H/../effects-smoke/entry.py" "Search effects" glow 2>>"$OUT/helpers.err"; sleep 4
    d press "Search effects"; sleep 0.5; d enter; sleep 2
    d shot 08d-glow
    # Its previews let go of the clip once hidden and idle (0.78.3: the
    # hidden Browser used to keep a 1080p decoder, about 180 MB; VE
    # Effects' tools/effects-smoke/soak_steps.sh has the full soak).
    check "the Browser's previews opened the clip" grep -q "frame renderer: opened" "$OUT/app.log"
    d press "Effects" exact; sleep 5
    check "the idle frame renderer closed the clip" grep -q "frame renderer: closed" "$OUT/app.log"
    d act save; sleep 2
    check "Glow added through the Browser (saved project)" \
        grep -q '<property name="mlt_service">frei0r.glow</property>' "$M/smoke.ustudio"
    glowing=$(render_now)
    l0=$(luma "$plain"); l1=$(luma "$glowing")
    echo "  render luma at 2.5 s: without Glow ${l0:-none}, with Glow ${l1:-none}"
    check "the render with Glow is brighter" \
        python3 -c "import sys; sys.exit(0 if float('${l1:-0}') > float('${l0:-0}') + 2 else 1)"
fi

# --- 9: Copy Diagnostics
d act copy-diagnostics; sleep 1
python3 "$H/clipboard.py" > "$OUT/diagnostics.txt"
grep -vE '^\[|^Last [0-9]+ log lines|^$' "$OUT/diagnostics.txt" | sed 's/^/  /'
if [ "$SMOKE_GPU" = 1 ]; then
    check "diagnostics say GPU acceleration: On" grep -q "^GPU acceleration: On: " "$OUT/diagnostics.txt"
fi
case "$SMOKE_RUNNER" in
    flatpak) check "diagnostics say Flatpak: yes" grep -q "^Flatpak: yes" "$OUT/diagnostics.txt" ;;
    *) check "diagnostics list a log folder" grep -q "^Log folder: " "$OUT/diagnostics.txt" ;;
esac

# --- 10: close
d close; sleep 4
check "clean exit at the end" test -z "$(editor_pid)"

kill "$REGISTRY" "$LAUNCHER" "$XVFB" 2>/dev/null
echo "RESULT: $FAILED failed"
