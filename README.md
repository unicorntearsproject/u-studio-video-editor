# U-Stu Video Editor

**A fast, focused video editor for Linux, built for people who talk on
camera.** Drop in an hour-long recording, find the good bits by waveform,
cut them with single keys, and export an MP4 ready to upload. No
subscription, no account, no network access, and it never touches your
original files.

- 🎙️ **Made for podcasters.** Sync separate mic and camera recordings
  automatically by their sound, split audio from video, and see a waveform
  on every clip.
- ✂️ **Made for clip editors.** Use J/K/L shuttle, split with `X`, ripple
  delete with `Shift+Delete`, and edit a whole cut without touching the
  mouse.
- 🖼️ **OBS-style picture placement.** Drag a webcam into the corner, crop
  it, rotate it, and snap it into place.
- ⚡ **Stays fast on long, heavy footage.** Imports, waveforms,
  thumbnails, saves and renders all run in the background, and proxies
  make 4K and phone footage play smoothly.
- 🛟 **Hard to lose work.** Undo everything, autosave with crash recovery
  (at most about 2 minutes lost), and timestamped backups on every save.
- 🐧 **GNOME-native.** Built with GTK4 + libadwaita on the proven
  [MLT](https://www.mltframework.org/) engine. No Qt, no KDE.

> **Status: public beta (0.50).** Cutting, audio sync, picture placement
> and MP4 export work today, with effects and titles as add-ons
> ([roadmap](#roadmap)).

## Install

```sh
# Prepare Flatpak for dependencies
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
# Download our Flatpak file (will be in Flatpak repos soon!)
wget https://software.unicornviz.com/u-studio-video-editor-latest.flatpak
# Install the package
flatpak install --user ./u-studio-video-editor-latest.flatpak
# Optional add-ons (same version as the app): titles and effects
wget https://software.unicornviz.com/u-studio-video-editor-dropin-titles-latest.flatpak
flatpak install --user ./u-studio-video-editor-dropin-titles-latest.flatpak
wget https://software.unicornviz.com/u-studio-video-editor-dropin-effects-latest.flatpak
flatpak install --user ./u-studio-video-editor-dropin-effects-latest.flatpak
# Launch from your app launcher & pin to dash!
# Or, from command line:
flatpak run com.ustudio.VideoEditor
```

This works on any Linux distribution with Flatpak (tested on Fedora 44,
made for Linux Mint 22, where you can also double-click the file). The
first install also downloads the GNOME runtime, about 450 MB. To update,
install the latest file again.

→ [Full install guide](docs/user/installing.md): updating, uninstalling,
permissions · [Getting started](docs/user/getting-started.md)

## Features

### Primary

- **Multi-track timeline**: any number of video and audio tracks. Upper tracks
  draw over lower ones and all audio mixes. → [Editing](docs/user/editing-the-timeline.md)
- **Fast cutting**: split, trim, ripple trim, slip, ripple delete, close
  gap, copy, multi-select, and snapping to edges, markers and the
  playhead. → [Editing](docs/user/editing-the-timeline.md)
- **Keyboard-first**: J/K/L shuttle, frame and cut jumps, and select, move
  and nudge clips from the keyboard. → [Shortcuts](docs/user/keyboard-shortcuts.md)
- **Sync by sound**: line up recordings of the same event to the frame
  with one click. → [Audio](docs/user/audio.md)
- **Audio tools**: per-clip waveforms, split audio, track volume and mute,
  and cross-fading dissolves. → [Audio](docs/user/audio.md)
- **Picture placement**: move, scale, rotate, crop and flip with handles on
  the preview, or type exact values. → [Preview and transform](docs/user/preview-and-transform.md)
- **MP4 export**: H.264 + AAC, with render profiles (720p to 4K, draft to
  max quality, any common frame rate) and a background render queue.
  → [Rendering](docs/user/rendering.md)

### Secondary

- Import files or whole folders, or drag them in; bad files are listed, not
  silently dropped. Numbered image sequences import as one clip.
  → [Importing](docs/user/importing-media.md)
- Media browser with thumbnails and badges for resolution, proxy and
  missing state. → [Importing](docs/user/importing-media.md#the-media-browser)
- Proxies for 4K and variable-frame-rate phone footage; renders always use
  the originals. → [Proxies](docs/user/importing-media.md#proxies-smooth-editing-of-4k-and-phone-footage)
- Mixed frame rates on one timeline; change the project rate at any time.
- Any background colour under the clips, in the preview and in renders.
- Missing media opens anyway and can be relinked in one step.
- Autosave, crash recovery, save backups, and recent projects.
  → [Projects and saving](docs/user/projects-and-saving.md)
- Markers, named tracks and clips, locked and hidden tracks, and a loop in
  and out.
- Preview scale (Auto, Full, Half, Quarter) for smooth playback on any
  machine, and GPU acceleration: tracks composited and clips placed on the
  graphics card, with hardware video decoding, where a check at startup
  says it works.
- Built-in Help with every control and shortcut, release notes, and
  one-click diagnostics for bug reports.
  → [Settings and Help](docs/user/settings-and-help.md)
- Project files are plain MLT XML, so `melt` can play them.

- Effects (an [add-on](docs/user/installing.md#add-ons)): hundreds of
  colour, light, blur, keying and audio effects on clips, tracks or the
  whole picture, tried on your picture before you add them, with
  keyframes (drawn as curves, or performed live) for effects and for a
  picture's position, size and rotation, looks, masks, LUTs,
  adjustment blocks over several tracks, copy and paste, before/after
  compare, audio plugins, and dissolves that dip, flash, blend, slide,
  push, zoom, spin or wipe.
  → [Effects](docs/user/effects.md)
- Titles and captions (an [add-on](docs/user/installing.md#add-ons)): a
  title designer with templates and animated (Lottie) layers, and
  `.srt`/`.vtt` captions in and out.
  → [Titles](docs/user/titles.md)

**Not yet:** zoom and blend transitions, formats other than MP4, named
markers, rebindable shortcuts. → [Known limitations](docs/user/troubleshooting.md#known-limitations)

## Roadmap

**Done:** the foundation (M0), project model with undo and save (M1),
frame-accurate playback (M2), the multi-track timeline (M3), a
multi-threaded engine (MT0–MT3), and the media bin with proxies,
relinking and clip transforms (M4, shipped as the 0.50 beta).

**Next:**

- **Effects, keyframes and transitions** (M5): effects, keyframes,
  looks, transitions, curve lanes, touch-record, the FX lane, masks, LUTs
  and audio plugins are in (FX1-FX5), as a drop-in add-on; still to come:
  blend-mode track compositing. → [doc 15](docs/plans/v2/15-effects-and-transitions.md),
  [ADR-013](docs/plans/v2/adr/013-effects-and-titles-as-drop-in-modules.md)
- **Titles**: a companion title designer with animation and reusable
  templates. → [doc 16](docs/plans/v2/16-titles-tool.md)
- **Audio polish** (the first optional drop-in): one-click voice cleanup
  and loudness targets for podcast (-16 LUFS) and streaming (-14 LUFS),
  plus level meters. → [doc 17](docs/plans/v2/17-drop-in-catalogue-and-distribution.md)
- **More drop-ins**: keying, stabilise, auto-captions (on your own
  machine), motion tracking, speed ramps. → [doc 17](docs/plans/v2/17-drop-in-catalogue-and-distribution.md)
- **Export** (M6): a standalone render tool, presets, and hardware
  encoders. → [doc 10](docs/plans/v2/10-export-and-rendering.md)
- **Polish and 2.0** (M7): accessibility and the first stable release. A
  project importer (Lightworks or an interchange format) is being
  considered instead of kdenlive import. → [doc 11](docs/plans/v2/11-build-test-ci-packaging.md)
- **Later**: Windows 10/11 ([ADR-017](docs/plans/v2/adr/017-windows-secondary-target.md)),
  and screen and mic capture, cross-track transitions and more
  (post-2.0 candidates).

→ [Full roadmap and acceptance criteria](docs/plans/v2/12-roadmap-and-milestones.md) ·
[All planning docs](docs/plans/v2/README.md) · [CHANGELOG](CHANGELOG.md)

## Documentation

- **Users:** [User guide](docs/user/README.md). Covers install, editing,
  audio, rendering, shortcuts and troubleshooting.
- **Developers:** [Developer docs](docs/developer/README.md). Covers
  [building](docs/developer/building.md),
  [architecture](docs/developer/architecture.md),
  [contributing](docs/developer/contributing.md) and the
  [MLT implementation notes](docs/developer/notes/README.md).
- **Everything:** [Documentation index](docs/README.md).

## Building from source

```sh
sudo dnf install mlt-devel      # plus GTK4, libadwaita, meson, ninja
meson setup builddir
meson compile -C builddir
./builddir/src/app/u-studio-video-editor
```

→ [Building, running and testing](docs/developer/building.md)

## About

U-Stu is a from-scratch editor. It isn't a port of kdenlive, though it
uses the same MLT engine. It's built in C++23 with meson. It's developed on
Fedora for 1080p/4K livestream and promo editing for the Unicorn Tears
brand.

Licence: [MIT](LICENSE). The Flatpak and other binary packages also bundle
third-party components under their own licences (FFmpeg built with x264 is
GPL), whose texts ship inside the package.
