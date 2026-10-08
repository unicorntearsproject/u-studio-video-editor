# Flatpak packaging

`com.ustudio.VideoEditor.yml` builds the beta-tester Flatpak: the app on the
GNOME 51 runtime, with its own MLT 7.40.0, FFmpeg 8.1.3 and x264. It's a
single-file bundle, not a Flathub submission.

## Building

```sh
just flatpak
```

- Needs `flatpak-builder`, the Flathub remote, and the GNOME 51 runtime and
  SDK as a `--user` install (`--install-deps-from=flathub` fetches them).
- The first build takes about 15 minutes (FFmpeg and MLT from source).
  Later builds reuse `build-flatpak/state` and take a few minutes.
- Between the build and the export, `tools/check_bundle_clean.py` fails
  the build if the tree carries test projects, media, logs, user settings
  or build-machine paths.
- The bundle lands in `build-flatpak/` (gitignored scratch). Then
  `just dist` copies it to `~/projects/_software-dist/u-stu-video-editor/`
  with a `<name>.sha256` sidecar.
- Before publishing, run the smoke test on the installed bundle:
  `tools/packaging-smoke/run.sh flatpak <outdir> --cleanup`
  ([packaging docs](../../docs/developer/packaging.md#package-checks)).
- `just dist` never overwrites or removes anything there: a repeated name
  becomes `-2`, `-3`, and so on (owner's rule). `USTUDIO_DIST_DIR` moves the
  destination.
- **VS Code snap terminals:** they export `XDG_DATA_HOME` into
  `~/snap/code/…`, which silently moves every `flatpak --user` operation
  into a second, private installation there. Run `just flatpak` from a
  normal terminal, or with `env -u XDG_DATA_HOME`.

### Drop-in extensions

`com.ustudio.VideoEditor.DropIn.Titles.yml` and
`com.ustudio.VideoEditor.DropIn.Effects.yml` build the titles and effects
drop-ins as extensions of the app's `com.ustudio.VideoEditor.DropIn` extension point:

```sh
just flatpak                                # the app first, same version
flatpak install --user build-flatpak/u-studio-video-editor-<version>.flatpak
just flatpak-titles
just flatpak-effects                        # frei0r and MLT's frei0r module inside
```

It builds against the installed app, so install that version first (into a
scratch `FLATPAK_USER_DIR` if you don't want to replace your own). How the
extension point works and what the extension carries:
[packaging docs](../../docs/developer/packaging.md#drop-ins-as-flatpak-extensions).

## What's in it, and why

Every dependency is a module file under `modules/`, shared with the
Flathub submission (`tools/flathub_prep.py`).

| Module | Source | Why it's built here |
|---|---|---|
| x264 | git `b35605ac` (stable; same as Fedora 44) | H.264 export; no Flathub runtime has it |
| FFmpeg 8.1.3 | release tarball, sha256-pinned | `--enable-gpl --enable-libx264`, dav1d, VA-API; no avdevice, no network |
| Eigen 3.4.1 | git tag + commit | Build-time only (headers for movit); nothing ships |
| movit 1.7.2 | release tarball, sha256-pinned | GPU compositing ([ADR-019](../../docs/plans/v2/adr/019-gpu-acceleration.md)); no runtime has it |
| MLT 7.40.0 | release tarball, sha256-pinned | Distros such as Mint 22 ship 7.22; the engine relies on 7.40 |
| U-Stu | this repo (`dir` source) | `-Dbuildtype=release`, editor plus `u-studio-render` |

- **MLT modules.** Only the modules the app and MLT's loader use are
  built: core, plus (the `affine` clip transform), normalize (`volume`),
  avformat, xml, sdl2, rtaudio, gdk (`pixbuf` stills), resample, xine
  (the loader's `deinterlace` normaliser) and movit (GPU, ADR-019).
  - Every normaliser named in MLT's `core/loader.ini` must have its first
    choice built. Without xine the loader falls back to `avdeinterlace`,
    which turns every frame into BT.601 limited-range YUV: an extra
    conversion per frame, and the GPU probe failed on the shifted colour.
  - MLT carries VE GPU's `patches/mlt-movit-convert-input-leak.patch`
    (a movit.convert leak of about 10 MB a minute of GPU playback) until
    upstream has a fix.
  - The movit module is inert until the app creates a `glsl.manager`.
    Then every producer opened through `loader` gets GPU normalisers;
    only the `loader-nogl` service stays on the CPU chain (ADR-019,
    decisions 3 and 4).
  - movit links FFTW, libepoxy and GL from the GNOME runtime (FFTW 3.3.11
    is in the Platform) and the GL extension through `--device=dri`, so
    none of those is bundled. Flatpak installs the runtime's
    `org.freedesktop.Platform.GL.default` (Mesa) automatically.
  - Check a build with `u-studio-render --gpu-probe` in the sandbox
    (`flatpak run --command=u-studio-render com.ustudio.VideoEditor
    --gpu-probe`): it prints `{"status":"ok",…}` and exits 0 when the GPU
    pipeline works. The smoke test runs it.
  - The Qt6 and glaxnimate modules are explicitly off (ADR-007), and so is
    frei0r (a future drop-in, ADR-014).
  - To recheck the list, match the service strings in `src/` against the
    `identifier:` lines of MLT's module `.yml` files.
- **SDL2** comes from the runtime (sdl2-compat on SDL3), so audio goes to
  PulseAudio/PipeWire.
- **Licence.** The app itself is MIT (`LICENSE`), but linking x264 makes
  the bundle as a whole GPL. flatpak-builder installs
  each module's licence files under `/app/share/licenses/`.
- **Permissions.** Wayland with X11 fallback, DRI, PulseAudio, and file
  access to home, `/media`, `/run/media` and `/mnt`. There's no network
  access; the editor makes no requests.
- **Fonts.** Space Grotesk, JetBrains Mono and Anton aren't bundled yet
  (doc 11 plans that for M7); the CSS fallbacks apply.

## For testers (Linux Mint 22 and other distros)

```sh
flatpak install --user ./u-studio-video-editor-<version>.flatpak
flatpak run com.ustudio.VideoEditor
```

Double-clicking the file in Mint's Software Manager works too.

- The bundle names Flathub as its runtime source, so the first install
  also downloads the GNOME 51 runtime (about 450 MB) if it isn't there
  already.
- Logs are under
  `~/.var/app/com.ustudio.VideoEditor/.local/state/ustudio/logs/`.
  Help › About has Open Log Folder and Copy Diagnostics for bug reports.
- Settings, autosaves and proxies live under
  `~/.var/app/com.ustudio.VideoEditor/`.
- To uninstall: `flatpak uninstall --user com.ustudio.VideoEditor`.
