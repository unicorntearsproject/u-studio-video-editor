# Testing

[Docs home](../README.md) › [Developer docs](README.md) › Testing

All tests are [doctest](../plans/v2/adr/010-doctest-vendored.md) suites
under `tests/`, registered with meson and run with `just test` (or
`meson test -C builddir --print-errorlogs`). Run one suite by its meson
name, for example `meson test -C builddir engine-sync`.

## Tests never use your desktop

Every test runs headless, by default (owner rule, 2026-10-08: a plain
`meson test` opened GTK windows on the real desktop). The root
`meson.build` makes `tools/test-headless.sh` the wrapper of the default
test setup (`headless`), so `meson test`, `just test`, `just asan` and
`just tsan` all go through it. For each test it:

- unsets `WAYLAND_DISPLAY`, sets `GDK_BACKEND=x11`, `GDK_DEBUG=no-portals`
  and `GSETTINGS_BACKEND=memory` (so no test writes your settings);
- starts a private Xvfb (`-displayfd`, a free display) and points
  `DISPLAY` at it; without Xvfb installed, `DISPLAY` is unset instead;
- runs the test in a private `dbus-run-session` whose activated services
  (the document portal) get a scratch runtime dir, so the desktop's
  `/run/user/<uid>/doc` is never mounted over (see below); the test keeps
  the real `XDG_RUNTIME_DIR`, which AT-SPI and audio need;
- passes a caller's `LD_PRELOAD` (`just asan`) to the test only, not to
  Xvfb and dbus-daemon.

It costs about 85 ms a test (a 67-test run: about 6 s of CPU across the
parallel jobs). A harness already inside such a session sets
`USTUDIO_HEADLESS=1` and the wrapper runs the test as it is. The
`headless-guard` test fails if a test can see `WAYLAND_DISPLAY` or `:0`.
Running a test binary by hand: run it through the wrapper,
`tools/test-headless.sh builddir/tests/app/test_<name>`.

## Layout

| Folder | Meson names | Needs |
|---|---|---|
| `tests/core/` | `core`, `core-undo-fuzz`, `core-thread-pool`, `core-model-release` | Nothing but the compiler: pure C++ |
| `tests/engine/` | `engine-*` (sync, playback controller, render, A/V sync, XML playback, caches, proxies, transform, mixed rates, …) | MLT, but no display and no media files |
| `tests/app/` | `app-*` (timeline controller and renderer, viewport, queues, settings, autosave, UI hints, …) | GTK for some; most test logic that was kept out of widgets |
| `tests/dropins/` | `dropins`, `dropin-*` | The drop-in build options (`just dropins-builtin`, `just dropins-module`) |
| `drop-ins/<name>/tests/` | `titles-*`, `effects-*` | A drop-in's own tests, built only with its `-Ddropin_<name>` option on (ADR-013: the folder is self-contained) |
| `tests/sanitizers/` | — | LeakSanitizer suppressions for `just asan` |
| `tests/common/` | — | Shared helpers, such as the random command stream for property tests |

## Rules

The full rules are in [CLAUDE.md](../../CLAUDE.md), "Testing discipline".
The short version:

- **No binary media in the repo.** Tests generate their inputs with MLT
  generators (`color:`, `noise:`, `tone:`). Renders during a test go to a
  temporary path, never next to source media.
- **Behaviour changes ship with a test** in the same change when practical.
- **Build it and exercise it** before claiming a change works. Run the app
  for UI changes, or a standalone repro for engine changes.
- **A red test you didn't write** may encode a planned API. Ask before
  changing production code to make it pass.
- **Sanitizers** (`just asan`, `just tsan`) are required for thread and
  MLT-lifetime changes, in the tiers CLAUDE.md sets out. See
  [Building](building.md#sanitizers). Don't preload MLT's movit module for
  every test: preloaded, it replaces same-named functions in other modules
  (plus's `lift_gamma_gain`), so `just asan` preloads it only for tests
  that start a GPU session: the "gpu" ones, `dropin-engine` (FX4's GPU
  lane cases) and `engine-hardware-decode`. A new test that opens a GPU
  session goes on that list in the justfile, or LeakSanitizer reports
  movit's `glsl.manager`, which MLT never frees, without a name to
  suppress.
- **No fixed time limits in tests.** A loaded machine (other suites, a
  Flatpak build) fails any absolute limit. A test times its work against a
  yardstick measured under the same load (as `engine-thread` and
  `titles-captions` do), and absolute times are benchmarks, run with
  `meson test -C builddir --benchmark` (`core-snapshot`,
  `titles-captions-bench`).

## The titles designer's smoke test

`tools/titles-smoke/run.sh <builddir> <outdir>` designs a lower third in
`u-studio-titles` from a blank canvas with the mouse and keyboard only
(doc 16, T2 acceptance), saves it through the Save dialog and checks the
saved title: three layers, the typed text, the bar's place and size, the
brand's fonts and gradient. It runs on a private Xvfb with its own D-Bus
session and AT-SPI bus, so nothing shows on the desktop; screenshots of
each step land in `<outdir>`. It needs a build with `-Ddropin_titles`, Xvfb,
python3 with `gi` (Atspi) and python-xlib, and ImageMagick's `import`.
Notes on driving GTK dialogs there are in
[the titles notes](notes/titles.md).

**Template packs in a smoke test**: `tools/make_test_pack.py OUT.zip
[--version V]` writes a small valid pack (one built-in template, a
generated preview; the repository holds no binary files), and
`u-studio-titles --install-pack OUT.zip` installs it without a window:
exit status 0 and "installed test/smoke-pack V in …", or 1 and
"refused: …" (the same or an older version, or a pack that fails
validation). Test `titles-install-pack` runs that round trip.

**The sharing service, locally**: `tools/share_mock.py [--seed DIR]
[--log FILE] [--corrupt]` serves doc 21's first-version API on
127.0.0.1 (signed, expiring download and upload URLs; OAuth code + PKCE
that really checks the verifier). `--log` records every request, for "no
request without a user action"; `--corrupt` serves tampered downloads.
Test `titles-share` runs the client against it; point `u-studio-share` at
it with `USTUDIO_SHARE_URL`.

**Any harness with a private D-Bus session** (`dbus-run-session`) must keep
the services that session activates out of the desktop's runtime dir. With
the real `XDG_RUNTIME_DIR`, the private session's `xdg-document-portal`
mounts over `/run/user/<uid>/doc` and unmounts it when it exits, which
leaves the desktop's own portal without its mount: Flatpak apps can't open
files through it, and `app-portal-path` fails, until the owner restarts it
(`systemctl --user restart xdg-document-portal.service`). Run
`dbus-update-activation-environment XDG_RUNTIME_DIR=<a mktemp dir>` first
thing inside the session, and set `GIO_USE_VFS=local`. Keep the real
`XDG_RUNTIME_DIR` for the processes the harness starts itself: a private
one for everything breaks the AT-SPI bus. `tools/titles-smoke` does this.

## The effects smoke test

`tools/effects-smoke/run.sh <builddir> <outdir>` drives a build's editor over
AT-SPI on a private Xvfb display, with its own D-Bus session and AT-SPI bus.
It imports a generated clip, opens the Effects page of the inspector, opens
the Browser with **E**, searches "glow", auditions Glow on the preview once
the scan has passed it, adds it with Enter, undoes and redoes it, pins Blur with **P** and changes it 60 frames on, and checks the
saved project each time. Then it saves the stack as a Look, imports a
second clip, applies the brand Look Neon Night to both, changes a value on
both at once, drags a brand Look onto the picture, turns compare on, and
holds **\\**. Then two stills go at the end of the track, **T** adds a
dissolve between them, the first wipe's tile (Wipe Right) on the
Transitions page is clicked, and the saved project must name `wipe.left`
and its map `ustudio-wipes/v1/left.pgm` (written beside it); undo takes it back. It also checks an audition leaves the live graph's rebuild
count unchanged. The window is
resized to the screen first (`fitwin.py`), so the inspector docks.
`value.py`, `entry.py`, `tile.py`, `where.py`, `drag.py` and `hold.py` set a
spin button, set an entry by name, select a Browser tile, find a widget's
centre, drag slowly enough for XDND, and hold a key, over AT-SPI (the main window
reports no focus without a window manager). The build needs `-Ddropin_effects`.
It reuses `tools/packaging-smoke/drive.py` for each step; screenshots and
logs land in `<outdir>`. Activated services get their own runtime directory,
as for the titles smoke test.

A steps file can set `SMOKE_SCREEN` (for example `1280x800x24`) before
sourcing `start.sh` to run on a smaller Xvfb screen, where the inspector
floats over the picture instead of docking.

`effects-render` (built with the render tool) is M5's box 2: one
synthetic project with a keyframed transform, a masked effect, a dissolve
with effects on both clips and a faded adjustment block, saved, then the
live Engine's frame hashes compared with the real `u-studio-render
--frames` process's.

`tools/effects-smoke/run.sh <builddir> <outdir> soak_steps.sh` is the
drop-in's playback memory soak (about 15 minutes): a 1080p H.264 clip,
selected, the Browser shown and then hidden while a fresh profile's health
scan runs, and 9 s looped for 12 minutes (on the GPU pipeline when it's
on). It fails if anything opens the clip while the Browser is hidden or if
the idle frame renderer never closes it. The slope of the RSS floor (each
60 s window's minimum after a 120 s warm-up) is reported against 1 MB/min
as a WARN only: under Xvfb the same build read -6 and +18 MB/min in two
15-minute runs, and an effects-free build +10 to +22 over 5 minutes, the
core GPU pipeline's swings to settle. Run it for changes to the
drop-in's frame renderer or anything it keeps open (why: [effects
notes](notes/effects.md#frame-renderer-memory)).

## Notable tests

- **Undo property test** (`core-undo-fuzz`, the same binary filtered to
  this one test: about 43 s alone, so it has its own 180 s timeout and
  `core` keeps 30 s): 10,000 random commands, undo them all,
  and the model must equal the start.
- **Bulk edits scale** (`core`, "bulk inserts, ripples and their undo
  scale near-linearly"): n and 10n clips inserted in order, rippled and
  undone; 10n must take under 30x as long (about 12x; quadratic was
  about 100x). A ratio, never a fixed time limit.
- **`EngineSync::verify()`** (`engine-sync`): after each of the first 500
  commands of that same stream, and each undo, the MLT graph must match
  the model.
- **A/V sync** (`engine-av-sync`): a generated beep and flash must line up
  in playback and render.
- **Project files play in `melt`** (`engine-xml-playback`): a saved
  project plays through MLT's `xml` producer frame for frame like the
  editor.
- **Render matches the preview** (`engine-render-frames`): `u-studio-render
  --frames` hashes every frame of a transformed clip and a dissolve, and the
  live Engine must show the same pixels; its `--ffv1` render must decode to
  the graph's YUV exactly.
- **Timeline draw speed** (`app-timeline-render`): 10 tracks × 500 clips
  draw in under 4 ms.
- **GPU pipeline** (`engine-gpu-pipeline`, `engine-gpu-engine`,
  `engine-gpu-probe`, `app-gpu-acceleration`): the GPU graph against the
  CPU one scene by scene, playback through the real consumer on the GPU and
  back, the probe, and the editor's startup decisions with a fake render
  tool. The engine ones need a GL driver (EGL); without one they print a
  message and pass, so run them on a machine with a GPU before landing GPU
  work. `engine-gpu-probe-no-egl` checks the clean failure everywhere.
- **Playback soak**: `tests/engine/playback_soak.cpp` is a manual tool for
  long playback runs. `--gpu` plays on the GPU pipeline, `--hwdecode` adds
  VAAPI, `--no-rotation` leaves the transformed tracks unrotated
  (ADR-019).
- **GPU stress** (`engine-gpu-stress`, and the `gpu_stress` tool): a
  tour-like project played through Engine on the GPU pipeline with random
  play, pause, seek, steps, preview scale, proxies and dissolve edits; 45 s
  with seed 1 in `meson test` (serial), for minutes by hand
  (`SDL_AUDIODRIVER=dummy gpu_stress <seconds> [seed]`). Every action is
  printed, so a crash replays; exit 77 skips on a machine without a GPU. Judge memory by its "RSS growth after warm-up" line, not
  by the first report: the first minute is warm-up (see the
  [playback notes](notes/playback-engine.md)).

The acceptance criteria each test backs are listed per milestone in
[v2 doc 12](../plans/v2/12-roadmap-and-milestones.md).
