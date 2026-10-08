# Importing media

[Docs home](../README.md) › [User guide](README.md) › Importing media

## Ways to import

| Do this | What happens |
|---|---|
| `Ctrl+I` (Import…) | Pick one or more files. Each is placed after the previous one on the active track. |
| `Ctrl+Shift+I` (Import Folder…), or drop a folder | Adds every file in the folder and its subfolders to the media browser (hidden files are skipped). |
| Drag files onto the timeline | Imports them and places them one after another, starting where you dropped. |
| Drag files onto the media browser | Adds them to the project without placing a clip. |
| `Ctrl+Alt+I` (Import Image Sequence…) | Pick any image of a numbered sequence; it becomes one clip in the media browser (below). |

- Files are opened in the background. The status bar shows progress
  ("Importing 3 of 12…") and the window stays responsive.
- However many files you import, it's **one undo step**.
- A file that can't be opened is skipped. At the end, a dialog lists each
  skipped file and why: empty, a folder, not media, or it took over 20
  seconds to open.
- **Subtitle files** (`.srt`, `.vtt`) become captions on tracks of their
  own, with the Titles add-on ([Titles › Captions](titles.md#captions)).
- **Still images** (PNG, JPEG, …) stretch from where they land to the end
  of the project, so a logo on an empty top track covers the whole video.

### Image sequences

Numbered images (`frame_0001.png`, `frame_0002.png`, …: a render from a
3D or animation tool) can be one clip, one image per frame at the
project's frame rate. Use **Import Image Sequence…** (`Ctrl+Alt+I`) and
pick any image of it. It's a separate command because camera photos are
numbered too and shouldn't turn into a clip by themselves.

- The sequence is every image with the same name before and after the
  number and the same number of digits, counting on from the one you
  picked **without a gap**: `frame_0001`–`frame_0030`, then a missing
  `frame_0031`, ends it at 30 images.
- The media browser names it with its range, `frame_[0001-0030].png`, and
  marks it **SEQUENCE**.
- If its first image is moved or deleted it shows as missing, like any
  file. Relink it like any missing file: pick any image of the sequence in
  its new place (or let **Search a Folder** find its first image). It may
  be numbered from somewhere else now, as long as it has enough images.
- Sequences get proxies like video (right-click › Create Proxy), unless
  their images are transparent (see below).
- Limits: images are loaded through GTK's image loaders (gdk-pixbuf); if
  those can't open a format, the frames stay blank. A sequence whose
  numbers start above 100 may show no thumbnail in the media browser
  (the clip itself plays).

## Frame rates and sizes

- A new, empty project takes its size and frame rate from the first video
  you import.
- You can mix clips of any rate from 23.976 to 60 fps on one timeline. If
  you import a clip at a different rate, the import summary says whether
  frames will repeat or be skipped.
- To change the project's rate, click the title in the header bar. Every
  cut, dissolve, marker and keyframe keeps its time, and the change is one
  undo step.
- The same dialog sets the project's **background**: the colour shown
  wherever no clip covers the frame (black by default), in the preview
  and in renders. It is saved with the project and is one undo step.

## The media browser

Toggle it with the button next to **Add track** in the header bar. Each row
shows a thumbnail, name, length, frame rate and format, with badges:

| Badge | Meaning |
|---|---|
| IMAGE, SEQUENCE, AUDIO | What kind of file it is |
| 4K, 1440p, 1080p, 720p, SD | Its resolution. **Cyan** means it's larger than the project, so a proxy would help. |
| PROBING, FAILED | Still being read, or couldn't be opened |
| MISSING | The file has moved or been deleted |
| PROXY, PROXY n%, PROXY MISSING | Proxy ready, being made, or lost |

- **Double-click** a row to insert it at the playhead on the active track.
- **Drag** a row onto the timeline to place it exactly where you drop it.
  The drop is refused if there's no room or the track is locked.
- **Right-click** a row:
  - **Remove from Project** removes the file and every clip cut from it.
    You can undo it, and the file on disk is untouched.
  - **Move File to Trash…** asks first, then removes it from the project
    and moves the file to your desktop's Trash, where you can still
    restore it.
  - **Create Proxy** / **Create Conformed Proxy** (see below).

## Missing media

If files have moved since the project was saved, it still opens, and one
message offers every way to find them:

- **Find Automatically** looks, in this order, in the project's folder, where
  the files were (and the folders beside it), the folders the project's other
  media is in, your Videos, Pictures and Music folders, your home folder, and
  mounted drives. A progress window shows where it's looking; **Cancel**
  stops it. Files are matched by name, and by size and date when several
  have the same name.
- **Search a Folder…** searches a folder you pick, and everything in it.
- **Locate…** lets you point at each file yourself. If others are in the same
  folder, it offers to relink them too.
- **Not Now** opens the project as it is: the affected clips are drawn with
  red stripes and play as dark red, and a banner keeps a **Relink…** button
  for later (with the same three ways).

Relinking is one undo step and changes nothing else. Rendering with missing
media asks you first.

## Proxies (smooth editing of 4K and phone footage)

A proxy is a smaller copy of a clip that plays smoothly while you edit.

- **Create Proxy** makes a 540p copy. Change the size in Settings ›
  Performance.
- **Create Conformed Proxy** makes a full-size copy at the project's frame
  rate. Use it for phone and screen recordings with variable frame rates.
- Footage taller than 1080p is offered a proxy once per project.
- Video and image sequences with transparency (ProRes 4444, WebM with
  alpha, QuickTime Animation, PNGs with alpha) don't get proxies: a proxy
  couldn't keep the transparency. Put them on a track above your video and
  the transparent parts show the tracks below, with clean edges. A project
  saved before 0.64.1 knows a video has transparency only once you import
  it again.
- Proxies are made in the background and stored in your user cache folder.
- The **Proxies** toggle beside the preview scale switches playback between
  proxies and originals.
- **Renders always use the originals**, so proxies never lower your
  export quality.

See also: [Editing on the timeline](editing-the-timeline.md) ·
[v2 doc 07: Media bin and assets](../plans/v2/07-media-bin-and-assets.md)
