# Settings and Help

[Docs home](../README.md) › [User guide](README.md) › Settings and Help

## Settings (tools button)

| Tab | Settings |
|---|---|
| **General** | Autosave delay, size of the recent-projects list, maximum shuttle speed |
| **Toggles** | Reopen the last project on startup; snap while dragging; follow the playhead while playing; timeline thumbnails; waveforms; thumbnails in clip tooltips |
| **Performance** | Default preview scale, proxy size, [GPU acceleration and hardware video decoding](#gpu-acceleration), worker threads, thumbnail and waveform jobs |
| **Locations** | Default project folder (where Open and Save As start); default export folder (where renders go) |
| **Render** | Render profiles and render threads ([Rendering](rendering.md#render-profiles)) |
| **Drop-ins** | Installed add-ons with an on/off switch (applies from the next start); known add-ons that aren't installed, and the package to install; any that failed to load, and why. Nothing is ever downloaded. |

Settings are saved automatically.

### GPU acceleration

Settings › Performance › Hardware. **Off by default in the Flatpak**; turn
it on here, and it stays on through updates (a build you compile yourself
starts with it on). When it's on and U-Stu starts, it checks in the
background that your graphics card can do the work, and if it can, the
preview composites your tracks and places, scales, crops and flips your
clips on the graphics card. The processor is then free, so
several moved or scaled tracks play smoothly at full size. The row shows
what it's using ("On: …"), or why it isn't ("Not available here: …").

Good to know:

- **Soft edges and dissolves look slightly brighter** with it on. The
  graphics card blends in linear light (the physically correct way), and
  the processor blends the stored values. Solid pictures look the same
  either way.
- **Rotated clips** are still drawn on the processor, so they don't get
  faster.
- **Exports look like the preview.** An export started while the preview
  uses the graphics card renders on it too, soft edges and dissolves
  included; one started with GPU acceleration off uses the processor.
  Turning it off in the middle of an export doesn't affect that export.
- If the graphics card stops working mid-session, playback carries on on
  the processor and U-Stu tells you. If U-Stu ever closes unexpectedly
  while using the graphics card, it turns GPU acceleration off and says so
  the next time it starts; you can turn it back on here.

**Hardware video decoding** (on by default) decodes your video files on
the graphics card while GPU acceleration is on, in the preview and in
exports, which halves the processor's work again. Without GPU acceleration it isn't used: it would
cost frames.

## Help (`?` button)

| Tab | What's there |
|---|---|
| **Controls** | Every button, menu item and timeline gesture |
| **Keyboard Shortcuts** | Every action and its key, by category |
| **Release notes** | What's new in each beta and release |
| **About** | The logo, version and what the editor is; links to the Unicorn Tears Project and the source code; the licence (MIT; the Flatpak also bundles GPL-licensed FFmpeg and x264); **Open Log Folder** (opens the folder in your file manager with this session's log selected), **Copy Diagnostics**. Links open in your browser; the editor itself never uses the network |

Help remembers which sections you opened, the tab and where you scrolled,
even after a restart. Tooltips and the Controls tab come from the same
source, so they always show the current key.

See also: [Keyboard shortcuts](keyboard-shortcuts.md) ·
[Troubleshooting](troubleshooting.md)
