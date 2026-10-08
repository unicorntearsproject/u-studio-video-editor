# Troubleshooting

[Docs home](../README.md) › [User guide](README.md) › Troubleshooting

## Reporting a bug

1. Open **Help › About** and click **Copy Diagnostics**. This copies the app,
   MLT, GTK and libadwaita versions, whether you're running the Flatpak,
   the log folder and the last 50 log lines. It never includes your
   environment or file contents.
2. Paste that into your report, along with what you did and what you
   expected.

**Open Log Folder** (same tab) opens the full logs:

| Install | Log folder |
|---|---|
| Flatpak | `~/.var/app/com.ustudio.VideoEditor/.local/state/ustudio/logs/` |
| Built from source | `~/.local/state/ustudio/logs/` (or `$XDG_STATE_HOME/ustudio/logs/`) |

## Common problems

### Playback stutters on 4K or phone footage

Set the preview scale to **Half** or **Quarter**, or make a
[proxy](importing-media.md#proxies-smooth-editing-of-4k-and-phone-footage).
A conformed proxy also fixes phone recordings with a variable frame rate.

### Playback looks wrong or glitches, or GPU acceleration turned itself off

Turn off **GPU acceleration** (Settings › Performance › Hardware): playback
then runs on the processor. If that fixes it, your graphics driver is the
cause; please report it with **Copy Diagnostics** from Help › About, which
includes what the GPU row says. If U-Stu turned GPU acceleration off by
itself, it closed unexpectedly while using the graphics card last time;
turn it back on to try again.

### Memory keeps growing during long playback with GPU acceleration on

Native builds from Fedora or other distributions use their own copy of the
MLT library, which leaks a little memory for every frame played on the
graphics card (about 10 MB a minute). The Flatpak will carry a fix. If a long
session gets slow, save and restart U-Stu, or turn GPU acceleration off.

### No sound

- Check the transport volume and that the track isn't muted (its name
  strip says "Muted").
- Check that the clip has sound: an audio-less video clip has no
  waveform.
- U-Stu plays through PipeWire or PulseAudio. Check that the right
  output device is selected in your system's sound settings.

### Clips are red and striped

The media file has moved. Click **Relink…** in the banner
([Missing media](importing-media.md#missing-media)).

### The app closed unexpectedly

Start it again and accept the recovery offer. At most about two minutes of
work is lost ([Autosave](projects-and-saving.md#autosave-and-crash-recovery)).

### A project won't open

A message says why, and the project file is left as it is:

- **Project not found**: the file was moved, renamed or deleted. Copies from
  each of your last five saves are kept in the hidden `.ustudio-backups`
  folder beside the project; copy the newest one back to the project's name
  and open it.
- **Saved by a newer U-Stu**: a newer version of the editor saved it. Update
  this one (the message names the version you need).
- **Not a U-Stu project**: the file isn't a project this version opens. That
  includes projects from the very first prototype editor.
- **The project is damaged**: its contents don't fit together. Try the newest
  copy in `.ustudio-backups`, and report it (see above), attaching the file.

The reason is also written to the log.

### A drop or move is refused

Red while dragging means the edit isn't allowed: the clip would overlap
another, the track is locked, or the track is the wrong kind (audio vs
video).

### Flatpak installed into the wrong place from a VS Code terminal

Terminals inside the VS Code snap set `XDG_DATA_HOME` to a private folder,
so `flatpak --user` installs somewhere your desktop can't see. Install from
a normal terminal, or run `env -u XDG_DATA_HOME flatpak install …`.

### A wipe stutters in the preview

With GPU acceleration on, a wipe takes the processor's slower path while
it plays, so at **Full** preview quality it can drop frames. Set the
preview to **Half**, or turn GPU acceleration off in Settings. The render
plays it exactly either way ([Transition styles](effects.md#transition-styles)).

### An effect is missing or turned off

With the Effects add-on, U-Stu checks every effect in the background the
first time (and after new effects are installed). One that crashed, hung
or ruined the picture in that check is turned off: it's hidden from the
**Add** page, and a project that uses it plays without it; its card on the
**Effects** page says why. Tick **Unstable** on the **Add** page to see
them; they may take the editor down. An effect whose tile says
**checking…** hasn't been checked yet: wait a few minutes. An effect card
that says **Not available on this computer** was saved on a machine with
effects this one doesn't have ([Effects](effects.md#unstable-effects)).

## Known limitations

These are planned; see the [roadmap](../../README.md#roadmap).

- Effects (the Effects add-on) can't draw masks yet. Transitions come in
  dissolve, dip, flash, slide, push, zoom, spin and wipe styles; blend
  styles and zooming out aren't there yet.
- Titles (the Titles drop-in, not yet in the packages) have no template
  gallery yet, and a moved title can't be relinked yet ([Titles](titles.md)).
- Renders are MP4 (H.264 + AAC) only.
- Markers can't be named, and the loop region isn't drawn on the timeline.
- Keyboard shortcuts can't be changed.
- Image sequences need their own command (Import Image Sequence…).
- Linux only for now. Windows is planned later; macOS isn't planned.
