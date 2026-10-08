# Building, running and testing

[Docs home](../README.md) › [Developer docs](README.md) › Building

Testers don't need any of this: install the Flatpak instead
([Installing](../user/installing.md)).

## Dependencies

On Fedora, install MLT's development package:

```sh
sudo dnf install mlt-devel
```

A GNOME development machine usually has everything else already: GTK4 ≥
4.10, libadwaita, GLib/GIO, libxml2, meson and ninja. libxml2 normally
comes in with MLT anyway.

- **doctest** is vendored under `subprojects/doctest/`, so you don't need a
  system package ([ADR-010](../plans/v2/adr/010-doctest-vendored.md)).
- **No PulseAudio package.** Audio goes through MLT's own `sdl2_audio` or
  `rtaudio` consumer, and both ship in the base `mlt` package
  ([Playback engine notes](notes/playback-engine.md)).
- **No Qt, no KDE Frameworks**, not even at run time
  ([ADR-007](../plans/v2/adr/007-mlt-module-load-policy.md)).
- **frei0r-plugins** (`sudo dnf install frei0r-plugins`) for the effects
  drop-in, built in by default in development builds
  (`-Ddropin_effects=builtin`; [ADR-011](../plans/v2/adr/011-frei0r-required-and-effect-families.md),
  the drop-in only). It's needed at run time and by the effects tests, not
  to compile; without it the frei0r effects are simply absent. `melt-7`
  (the `mlt` package) lets one effects test check a saved project in stock
  melt. The core Flatpak pins `-Ddropin_effects=disabled`.
- **libarchive** (`sudo dnf install libarchive-devel`) for the titles
  drop-in's template packs ([ADR-020](../plans/v2/adr/020-template-packages-and-sharing.md);
  the drop-in only). Without its headers the drop-in still builds, and
  opening or saving a pack says it can't.
- **ThorVG** 1.0 or newer (`sudo dnf install thorvg-devel`; pkg-config
  `thorvg-1`) for the titles drop-in's animated (Lottie) layers
  ([ADR-021](../plans/v2/adr/021-lottie-layers-via-thorvg.md); the
  drop-in's renderer only). Meson option `titles_thorvg` (auto): without
  ThorVG the drop-in still builds, and an animated layer draws nothing
  with a warning. Fedora's package runs
  Lottie expressions and can open files; the titles' own check refuses
  both before ThorVG sees a file, and the Flatpak builds ThorVG without
  either.
- **libsoup 3, json-glib, libsecret** (`sudo dnf install libsoup3-devel
  json-glib-devel libsecret-devel`) for `u-studio-share`, the titles
  drop-in's sharing helper (ADR-020; the helper only). Meson option
  `titles_share` (auto): without libsoup or json-glib the helper isn't
  built; without libsecret, sign-in isn't remembered.
- Anything else needs an ADR and the owner's sign-off
  ([CLAUDE.md](../../CLAUDE.md), "Language & build standards").

Optional development tools:

```sh
sudo dnf install clang-tools-extra just
```

- `clang-tools-extra` provides `clang-format`, which `just fmt` runs with
  the repo's `.clang-format`.
- `just` runs the `justfile` recipes below. Without it, run the command
  each recipe's body shows.

## Build, run, test

| `just` recipe | Plain command |
|---|---|
| `just setup` | `meson setup builddir -Dbuildtype=debug -Dtests=enabled` |
| `just build` | `meson compile -C builddir` |
| `just test` | `tools/meson-test.sh -C builddir --print-errorlogs` (`meson test` from a clean environment, [testing](testing.md#no-secrets-in-tests-or-their-logs)) |
| `just run` | `./builddir/src/app/u-studio-video-editor` |
| `just fmt` | `clang-format -i` on every tracked `.cpp`/`.h` |
| `just check-qt` | Build, then check that no Qt is loaded (below) |
| `just asan [tests…]` | ASan + UBSan + LeakSanitizer build in `builddir-asan` |
| `just tsan [tests…]` | ThreadSanitizer build in `builddir-tsan` |
| `just flatpak` | Build the Flatpak bundle ([Packaging](packaging.md)) |
| `just dist <bundle>` | Copy a bundle to the distribution folder with a checksum |

Run `just test` (or `tools/meson-test.sh -C builddir`) before every commit.

## Logging

Logs go to `$XDG_STATE_HOME/ustudio/logs/`, or
`~/.local/state/ustudio/logs/` when that isn't set. `USTUDIO_LOG_LEVEL`
sets the level: `debug`, `info`, `warn`, `error` or `none`. The default is
`debug` while the app is under active debugging.

```sh
USTUDIO_LOG_LEVEL=debug ./builddir/src/app/u-studio-video-editor
```

At `debug`:

- Every status-bar message is also logged.
- Consumer start/stop, graph rebuilds, renders and waveform jobs log how
  long they took (`Log::ScopedTimer`, `src/core/log.h`).
- The stall monitor logs any main-loop iteration over 16 ms. A new stall
  with our code in it is a bug.
- The engine thread logs its own work over 50 ms as "engine thread busy".

Messages start with a bracketed subsystem (`[engine]`, `[timeline]`), so
you can grep by layer.

## Checking that Qt stays out

```sh
ldd builddir/src/app/u-studio-video-editor | grep -iE 'qt|kde'   # expect no output
./builddir/tests/engine/test_factory_policy                      # expect PASS
```

The `ldd` check covers only the link line. The factory-policy test checks
the running process (`/proc/self/maps` has no `libQt`). That test is the
one that counts, because a plain `Mlt::Factory::init()` loads 32 Qt
libraries at run time.

## Sanitizers

`just asan` and `just tsan` each build in their own directory, and both
suites pass clean, so any report they print is a real finding. Both
recipes take meson test names (`just asan engine-thread`) to run part of
the suite.

- The full suites take about 6½ min (ASan) and 4½ min (TSan). CLAUDE.md,
  "Testing discipline", says when each tier is required.
- `tests/sanitizers/lsan.supp` lists the leaks that aren't ours: MLT's
  loader and module repository, FFmpeg worker threads, and SDL. Its header
  explains why a leaked mlt++ wrapper of ours still gets reported through
  it.
- `just asan` runs the GPU tests (names with "gpu") separately, with MLT's
  movit module preloaded for its leak's names; everything else runs without
  it, because a preloaded movit overrides same-named functions in other
  modules (plus's `lift_gamma_gain` became movit's, which fails without GL).
- `lsan.supp` also names MLT's avfilter filter, which leaks a properties
  object each time it sets up a graph ([MLT upstream candidates](notes/mlt-upstream.md)).
- The `justfile` comments explain the non-default sanitizer options.
- First run and findings:
  [2026-09-23 sanitizer report](../audit/2026-09-23-sanitizer-report.md).

## Settings during development

A binary run straight from `builddir` uses the build's own compiled
GSettings schema (`builddir/data`), so you don't need `meson install` or
`GSETTINGS_SCHEMA_DIR`. Without any schema, the app falls back to built-in
defaults and shows a banner. See
[Settings and GSettings notes](notes/settings.md).

## See also

- [Testing](testing.md): how the test suites are organised and what they
  need.
- [v2 doc 11: Build, test, CI, packaging](../plans/v2/11-build-test-ci-packaging.md).
