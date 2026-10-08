# Demo tour

Drives the editor through every user-facing feature on an off-screen X
display and records it: screen, the app's own audio, burned-in chapter
captions, and an 8× fast-forward through the render wait. It doubles as a
visual regression pass: rerun it after a milestone and watch the video.

Not built by meson and not part of `meson test`. Media is never committed.

## Needs

`Xvfb`, `ffmpeg` (x11grab, libx264, libass), `python3` with `gi`/Atspi,
`python-xlib` and Pillow, ImageMagick's `import`, and the AT-SPI bus
(`/usr/libexec/at-spi-bus-launcher`, `at-spi2-registryd`). Everything
runs under `env -i` with scratch XDG directories, so the owner's settings,
autosaves and desktop are untouched, and nothing plays on the speakers
(SDL's disk driver feeds `audiorec.py`, which keeps audio in step across
the consumer restarts every edit causes).

## Media

`TOUR_MEDIA` points at a folder with `unicorn/` and `zizzle/` (numbered
scene clips), `stills/` (a few PNGs) and `finals/unicorn-dj-final.mp4`
(a finished cut with a soundtrack). The 2026-09-25 recording used copies
of the owner's AI-generated projects (unicorn DJ at 1080p30, zizzle zap
zone at 1344×768@25).

## One command (the rolling series)

```sh
tools/demo-tour/make_demo.sh   # from the demo worktree (agent/strategist-demo)
```

Stages the media once into `~/.cache/ustudio-demo-media` (copies from the
owner's drive, read-only), builds a separate release `builddir-demo`,
records, and saves `u-studio-demo-<date>-v<version>.mp4` into
`/home/jj/projects/_software-dist/u-stu-video-editor/demos/` (since 2026-10-08; earlier videos stay in `/home/jj/projects/u-studio-video-editor-projects/demo-videos/`). It never
overwrites or removes a video there; a repeat run the same day gets `-2`,
`-3`, and so on.

## Run by hand

```sh
TOUR_MEDIA=/path/to/media ./run_tour.sh /tmp/tour-out 1   # 1 = record
python3 post.py /tmp/tour-out /tmp/tour-out/u-studio-demo.mp4
```

`TOUR_UPTO=N` stops after chapter N; without recording, each chapter
leaves screenshots in the output folder for checking.

## Notes

- GTK4 reports no widget positions over AT-SPI, so the timeline is found by
  pixel: clips are selected and their cyan selection outline gives exact
  edges (`selected_box`).
- There's no window manager: `fitwin.py` sizes the main window to the
  screen, and `center_dialogs()` centres dialogs.
- File choosers get their path set through AT-SPI (`set_location`), not
  typed, which avoids the location entry's autocomplete race.
- Playback on Xvfb is software-rendered, so it's choppier than on a real
  desktop.

## Narrated parts (the series)

Since 2026-09-28 every demo is a short, narrated part of the tour:

```sh
TOUR_PART=basics-1 tools/demo-tour/make_demo.sh   # -> u-studio-demo-<date>-basics-1-v<version>.mp4
```

- `parts.json` names each part's chapters (by their `step()` titles), its
  card title and its narration script. The tour still runs from the start
  (later chapters need the state earlier ones build) and stops after the
  part's last chapter; `post.py` keeps only the part's chapters.
- `narration/<part>.txt`: one `## <chapter title>` block per chapter, plus
  `## intro` and `## outro` for the cards. Plain language for users.
- `narrate.py` speaks every line with `tts.py` (OpenAI speech, voice
  `cedar`, `gpt-4o-mini-tts`, speed 0.9) before the run, cached in
  `~/.cache/ustudio-demo-tts` by a hash of the request. The key is read at
  call time from `OPENAI_API_KEY` or the owner's
  `~/Repos/ai-animated-video/config.env`, and never printed or stored. This
  is the demo tooling's only network use; the editor never goes online.
- Each chapter lasts at least as long as its line. `post.py` lays the voice
  at the chapter starts (never inside a fast-forward), ducks the app's own
  sound under it, narrates the cards, and normalises to -16 LUFS.
- `TOUR_HOLD=1` records and posts but doesn't save: check the run, then copy it in with `cp -n`.
- `TOUR_GPU=0` presets GPU acceleration off; `TOUR_NO_MERGE=1` records the
  build as it is; `TOUR_STOP_AFTER=<title>` ends a run early.

Series (one per slot, each under 5 minutes; done ones are dated):

| Part | Topic | Recorded |
|---|---|---|
| basics-1 | Importing, mixed frame rates, the media browser, split audio | 2026-09-28 (0.70.0-beta.1) |
| basics-2 | Playback, shuttle, loop, timeline editing | 2026-09-28 (0.71.0-beta.1, GPU off; file -2) |
| basics-3 | Save and backups, settings, the project format | 2026-09-28 (0.73.0-beta.1, GPU off; file -2) |
| basics-4 | Rendering, the queue, quit while rendering and restart | 2026-09-29 (0.78.0-beta.1, GPU on, drop-ins off) |
| titles-1 | New Title, the template gallery, editing text, the brand kit | 2026-09-29 (0.78.2-beta.1) |
| pip | Picture in picture: move, scale, rotate, crop, flip, Edit Transform | 2026-09-29 (0.78.2-beta.1, GPU off) |
| titles-2 | Animation: behaviours, the strip, fields on the Title page | 2026-09-29 (0.78.2-beta.1) |
| titles-3 | Bake, Export for OBS, template packs, animated templates | 2026-09-29 (0.78.2-beta.1) |
| mixed-media | Stills, image sequences, proxies, missing media and relink | 2026-09-29 (0.78.0-beta.1) |
| relink | Missing media and relink; proxies | |
| transitions | T, dissolves, wipes, slides and pushes | 2026-09-29 (0.78.3-beta.1) |
| settings-help | Settings, shortcuts, Help, diagnostics | 2026-09-29 (0.78.0-beta.1) |
| captions | Import SRT/VTT, colours and top placement, fix a line, export | 2026-09-29 (0.78.0-beta.1, file -2) |
| titles-4 | Animated layers (Lottie), Plays and Speed, the ringing-bell templates | 2026-09-29 (0.79.0-beta.1) |
| transitions-2 | Zoom, spin, tiles with your clips, equal-power sound | 2026-09-29 (0.79.0-beta.1) |
| whats-new-078 | Highlight reel for testers, cut from the day's recordings (reel.py) | 2026-09-29 (0.79.0-beta.1) |
| keyframes-2 | Keyframed transforms: the Transform card, pins, key-aware drags, curves, touch-record | 2026-09-29 (0.80.2-beta.1) |
| transitions-3 | Additive, Screen and Lighten dissolves, Sound: Cut | 2026-09-29 (0.80.2-beta.1) |
| whats-new-080 | Highlight reel for 0.80 (reel.py) | 2026-09-29 (0.80.2-beta.1) |
| showreel | Every major feature at speed, edited in U Stu (marin, hype) | 2026-10-01 (0.80.2-beta.1) |
| gpu | GPU acceleration, on the real desktop (screencast.py + gpu_desktop2.sh; the owner consents in the portal) | 2026-09-29 (0.78.5-beta.1) |
| effects-1 | The Add page, trying effects, the Effects page, Looks, Compare | 2026-09-29 (0.78.3-beta.1) |
| effects-2 | Keyframes, curve lanes, masks, adjustment blocks, LUTs | 2026-09-29 (0.78.5-beta.1) |

## Browsing the videos

Next to the videos (in `_software-dist/u-stu-video-editor/demos/`) is a dated index (`INDEX-<date>.md`) for the owner:
every video in suggested order, with its topic, length and version, plus
the superseded takes. Nothing in that folder is ever overwritten, so an
updated index is a new dated file (`-2`, `-3` for a second one the same day).

## The showreel

A 6–7 minute show-off of every major feature, edited in U Stu itself:

```sh
# 1. raw footage: the part scripts recorded without narration (see the
#    showreel's record.sh in the run notes), into <runs>/r1-... r12-...
python3 showreel_plan.py <runs> <runs>/b1          # segments, music video, plan.json
cp <runs>/b1/plan.json <runs>/build/ && TOUR_SCRIPT=$PWD/showreel_build.py ./run_tour.sh <runs>/build 0
TOUR_VOICE=marin TOUR_VOICE_STYLE=hype TOUR_VOICE_SPEED=1.0 python3 narrate.py narration/showreel.txt <runs>/narration.json
python3 showreel_mix.py <runs>/b1/plan.json <runs>/narration.json <runs>/build/work/showreel-*.mp4 <out>.mp4
# then cp -n <out>.mp4 into _software-dist/u-stu-video-editor/demos/ (never overwrite: -2, -3)
```

The build imports the footage and a black music video into U Stu, splits the
music onto A1, trims spare frames for transitions, puts a look on the bookend
clips, a transition style on every cut, an animated title (Background: None)
from U Stu Titles at each section, then saves and renders with U Stu. Only the
narration is mixed afterwards. Music: the Unicorn Tears songs "Neon Level Up
01" and "Rave All Night (Radio Edit)", copied read-only from the owner's songs
folder. Titles start after each cut's transition: a New Title inside a Push
Left transition never opened the designer (0.80.2, reported).
