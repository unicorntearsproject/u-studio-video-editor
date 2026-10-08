#!/bin/bash
# One-command demo recording for the rolling series.
#   make_demo.sh    run from the demo worktree (branch agent/strategist-demo): merges the
#                   latest origin/main into it, builds its own release builddir, records.
# Stages media from the owner's drive (read-only) into ~/.cache/ustudio-demo-media once,
# records the tour, and saves the result into the demo-videos folder without ever
# overwriting or removing an existing video.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# Owner rule 2026-10-08: demos live beside the packages (videos before then stay in
# projects/u-studio-video-editor-projects/demo-videos/).
DEST=${TOUR_DEST:-/home/jj/projects/_software-dist/u-stu-video-editor/demos}
MEDIA=${TOUR_MEDIA:-$HOME/.cache/ustudio-demo-media}
AI=/run/media/jj/Expansion/Work/video/exports/ai-generated
WORK=${TMPDIR:-/tmp}/ustudio-demo-run-$$

# 1. media (copies; the originals are only read)
if [ ! -f "$MEDIA/.complete" ]; then
  [ -d "$AI" ] || { echo "drive not mounted: $AI"; exit 1; }
  U=$AI/a-semi-abstract-fantasy-story-about-a-lonely-unicorn-dj-wand
  mkdir -p "$MEDIA"/{unicorn,zizzle,stills,finals}
  for i in 0 1 2 3 4 5 6 7 8 9; do cp -n "$U/video/chunk_00$i.mp4" "$MEDIA/unicorn/"; done
  cp -n "$U/video/a-semi-abstract-fantasy-story-about-a-lonely-unicorn-dj-wand-title-card.mp4" "$MEDIA/unicorn/title-card.mp4"
  for i in 0 1 2 3 4 5 6 7; do cp -n "$AI/zizzle-zap-zone/video/chunk_00$i.mp4" "$MEDIA/zizzle/"; done
  cp -n "$U/thumbnail.png" "$MEDIA/stills/unicorn-thumbnail.png"
  cp -n "$AI/cog-of-the-quark/thumbnail.png" "$MEDIA/stills/cog-thumbnail.png"
  cp -n "$AI/zizzle-zap-zone/images/chunk_000.png" "$MEDIA/stills/zizzle-000.png"
  cp -n "$U/a-semi-abstract-fantasy-story-about-a-lonely-unicorn-dj-wand-final.mp4" "$MEDIA/finals/unicorn-dj-final.mp4"
  touch "$MEDIA/.complete"
fi

# 2. current main, built as release in this worktree (never in the owner's checkout)
# TOUR_NO_MERGE=1 records the build as it is (the one the dry run passed on).
if [ "${TOUR_NO_MERGE:-0}" != 1 ]; then
  git -C "$REPO" fetch -q origin && git -C "$REPO" merge -q --no-edit origin/main
fi
BUILD=$REPO/builddir
[ -d "$BUILD" ] || meson setup "$BUILD" "$REPO" >/dev/null
meson configure "$BUILD" -Dbuildtype=release -Ddropin_titles=builtin -Ddropin_effects=builtin >/dev/null   # the titles chapters need the designer
meson compile -C "$BUILD" >/dev/null
VERSION=$(meson introspect "$BUILD" --projectinfo | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')

# 3. narration (a part from parts.json: TOUR_PART=basics-1), record, post
mkdir -p "$WORK"
PART=${TOUR_PART:-}
STOP=
NAME=u-studio-demo-$(date +%F)
if [ -n "$PART" ]; then
  NARR=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]]["narration"])' "$HERE/parts.json" "$PART")
  STOP=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]]["chapters"][-1])' "$HERE/parts.json" "$PART")
  # A part can skip tour sections it doesn't need and start without drop-ins.
  export TOUR_SKIP=${TOUR_SKIP:-$(python3 -c 'import json,sys; print(",".join(json.load(open(sys.argv[1]))[sys.argv[2]].get("skip", [])))' "$HERE/parts.json" "$PART")}
  SCRIPT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]].get("script", ""))' "$HERE/parts.json" "$PART")
  [ -n "$SCRIPT" ] && export TOUR_SCRIPT="$HERE/$SCRIPT"   # a part with its own short tour (captions.py)
  export TOUR_DROPINS_OFF=${TOUR_DROPINS_OFF:-$(python3 -c 'import json,sys; print(",".join(json.load(open(sys.argv[1]))[sys.argv[2]].get("dropins_off", [])))' "$HERE/parts.json" "$PART")}
  python3 "$HERE/narrate.py" "$HERE/$NARR" "$WORK/narration.json"
  NAME=$NAME-$PART
fi
TOUR_STOP_AFTER="$STOP" TOUR_MEDIA="$MEDIA" TOUR_BIN="$BUILD/src/app/u-studio-video-editor" "$HERE/run_tour.sh" "$WORK" 1
# Only a complete run is saved: videos in $DEST can never be removed, so a crash
# or a failed step must stop here (2026-09-28: a GPU abort cut a part short).
FAIL=
grep -q "Traceback" "$WORK/run.log" && FAIL="the tour script failed"
grep -q "Assertion .* failed" "$WORK/app.stderr" && FAIL="the editor aborted (see app.stderr)"
if [ -n "$PART" ]; then
  grep -q "=== (end)" "$WORK/tour.log" || FAIL="${FAIL:-the part did not reach its end}"
fi
if [ -n "$FAIL" ]; then
  echo "NOT SAVED: $FAIL; run folder: $WORK"
  exit 1
fi
TOUR_PART="$PART" TOUR_VERSION="$VERSION" python3 "$HERE/post.py" "$WORK" "$WORK/demo.mp4" || { echo "NOT SAVED: post.py failed; run folder: $WORK"; exit 1; }

# 4. save, never overwrite
mkdir -p "$DEST"
BASE="$DEST/$NAME-v$VERSION"
OUT="$BASE.mp4"; n=2
while [ -e "$OUT" ]; do OUT="$BASE-$n.mp4"; n=$((n + 1)); done
# TOUR_HOLD=1: stop here so the run can be checked first; save it afterwards with
#   cp -n <run>/demo.mp4 <the path below>   (checking again that it doesn't exist)
if [ "${TOUR_HOLD:-0}" = 1 ]; then
  echo "HELD (not saved): $WORK/demo.mp4 -> $OUT"
  exit 0
fi
cp "$WORK/demo.mp4" "$OUT"
echo "saved $OUT"
echo "run folder (screenshots, logs): $WORK"
