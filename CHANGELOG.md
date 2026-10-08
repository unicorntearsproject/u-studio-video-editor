# Changelog

All notable user-facing changes to this project are documented here.
Format: newest first, one line per change. Internal refactors, tests, and
docs-only changes are not listed (CLAUDE.md).

## (next)

- The name is now spelt "U-Stu": U-Stu Video Editor and U-Stu Titles, in
  titles, About, menus, messages and the docs.

## 0.81.0-beta.1

- A project that won't open says why (not found, saved by a newer version,
  not a U Stu project, damaged), and the reason goes to the log.
- Opening a project with missing media offers Find Automatically, Search a
  Folder, Locate or Not Now in one message; Find Automatically looks in the
  likely places with progress and Cancel.
- Ctrl+Q quits (asking first if there are unsaved changes).

## 0.80.2-beta.1

- The editor's number settings (Settings, Render profiles, Edit Transform)
  are reachable by screen readers.

## 0.80.1-beta.1

- U Stu Titles: the inspector's number rows (Speed, position, size and
  the rest) and the Export dialog's length can be reached by screen
  readers.
- U Stu Titles: an added animation's Height shows its real height.

## 0.80.0-beta.1

- Effects: a picture's position, size and rotation can change over time:
  the Transform card at the top of the Effects page pins, steps, eases and
  records them like an effect's values, with curves in the curve lanes;
  dragging a keyframed picture on the preview sets its keyframe at the
  playhead.
- Transitions: a transition's sound can cut at its middle instead of
  crossfading (Sound: Cut).
- Transitions: Additive, Screen and Lighten blend dissolves; a styled
  dissolve opened without the Effects add-on plays as a plain one instead
  of refusing the project.
- Effects: a health check still running when the editor quits no longer
  outlives it; adding to an adjustment block says so.

## 0.79.0-beta.1

- Transitions: Zoom In and Spin In (either way) join the motion styles; a
  transition's sound can be an even (equal-power) crossfade instead of a
  straight one; the style tiles show your own two clips half-way through.
- Effects: a LUT or other file inside the project's folder is saved
  relative to it, so the project keeps it when the folder moves. Such a
  project needs this version or newer to open.

## 0.78.5-beta.1

- Dragging a picture in the preview is smoother: audio waveforms on the
  timeline are no longer redrawn from scratch on every step.

## 0.78.4-beta.1

- With GPU acceleration on, a flipped clip now shows mirrored in place; before, a cropped or rotated flipped clip could come out turned and smeared, or slide out of its box.

## 0.78.3-beta.1

- Effects: the Add page no longer renders its tiles while it's hidden, and
  its previews let go of a clip's memory a moment after they're drawn
  (about 180 MB for a 1080p clip, which the editor used to keep).
- Effects: fixed two crashes, one when a selected title couldn't be opened
  for the Add page's tiles and one at quit; effects that only run on a
  graphics card's compute interfaces (Vulkan, OpenCL, CUDA, VAAPI) are no
  longer offered.

## 0.78.2-beta.1

- U Stu Titles no longer crashes when you close it with a layer selected.
- A caption clip's name on the timeline follows its words when you edit
  them on the Title page (a name you gave it yourself stays).
- Exporting or baking a title can no longer crash as it finishes (the
  encoder's last thread is now waited for).

## 0.78.1-beta.1

- A dissolve into a still or a colour clip works from its first frame
  (it used to be refused there).

## 0.78.0-beta.1

- Titles: two built-in templates with a ringing bell, a lower third and a
  subscribe reminder, animated layers ready to use.

## 0.77.0-beta.1

- Effects (the Effects add-on): adjustment blocks on an FX lane above the
  tracks (effects over a stretch of time on the tracks beneath, with
  fades), curve lanes under a clip (C), touch-record, masks (rectangle or
  ellipse, soft edge, invert), an eyedropper and rectangles you drag on the
  picture, a LUT library for .cube files, and LADSPA audio plugins (VST2 and
  OpenFX behind a choice). OpenFX plugins can no longer load unasked.

## 0.76.0-beta.1

- Titles: animated layers. Add a Lottie animation (a logo sting, a
  sticker) to a title; it plays in the designer, the editor and exports,
  and travels in templates and template packs.

## 0.75.1-beta.1

- Editing while playing restarts playback faster: frames about to be thrown
  away are no longer rendered, and one from before the edit can't flash up.

## 0.75.0-beta.1

- Transition styles (the Effects add-on): a dissolve can dip to black,
  flash, slide, push or wipe (20 shapes) from the Transitions page; T adds one at the
  nearest cut, and a double-click on one opens its styles.

## 0.74.1-beta.1

- Importing thousands of clips onto one track (a long caption file, a big
  folder) is fast: 20,000 clips now take a fraction of a second
  instead of minutes, and undoing them just as fast.

## 0.74.0-beta.1

- Effects (the Effects add-on): add, try on the picture, keyframe and
  compare hundreds of effects on clips, tracks or the whole picture, with
  looks, copy and paste, and several clips at once; the inspector now sits
  beside the picture on wide windows.

## 0.73.1-beta.1

- With GPU acceleration on, pausing, seeking or playing around dissolves
  no longer closes the editor when a frame runs late.

## 0.73.0-beta.1

- Captions: the eight standard caption colours and top placement come in
  from .vtt and .srt files and go back out on export.

## 0.72.0-beta.1

- Titles: a title remembers its template, and U Stu Titles offers to
  update it when the template changes, keeping the title's field text.

## 0.71.1-beta.1

- The Flatpak starts with GPU acceleration off; switch it on in Settings ›
  Performance › Hardware, and it stays on through updates.

## 0.71.0-beta.1

- Captions: Export Captions… writes the project's captions to an .srt or
  .vtt file that imports back to the same captions.

## 0.70.0-beta.1

- Captions: import .srt and .vtt subtitle files as caption clips on their
  own tracks, styled by one caption title (three built-in looks).

## 0.69.3-beta.1

- With GPU acceleration on, pausing no longer crashes the editor when a
  clip and a copy of it play on different tracks at once (for example a
  rotated picture-in-picture over the original).

## 0.69.2-beta.1

- New Title in an unsaved project never makes a folder loose in your home
  folder: default project folder, then Videos, then U Stu's data folder.

## 0.69.1-beta.1

- U Stu Share uses the editor's Unicorn Tears styling.

## 0.69.0-beta.1

- U Stu Share: a separate helper to browse, install and publish template
  packs online (the catalogue isn't online yet). U Stu itself stays
  offline.

## 0.68.0-beta.1

- Template packs: save templates as a .zip or .tar.gz and open packs from
  others in U Stu Titles' gallery; each is checked before it's installed.

## 0.67.1-beta.1

- The editor opens maximised, and the Render button is a little narrower
  ("Render 42%" while it runs), so the header's project title has room.

## 0.67.0-beta.1

- The header's Help button is a plain question mark, and with the Titles
  add-on a T button (between Render and Settings) opens U Stu Titles.

## 0.66.0-beta.1

- Title templates: New Title (Shift+T) puts a title at the playhead and
  opens U Stu Titles with a gallery of 29 built-in designs; save your own
  as templates, and rename, duplicate or delete them in My Templates.

## 0.65.1-beta.1

- Titles come as a Flatpak add-on, installed next to the app: import
  titles, fill their fields, edit them in U Stu Titles, bake them and
  export them for OBS.

## 0.65.0-beta.1

- Export a title clip on its own from the editor (Title page › Export on
  its own): with transparency for OBS, with the clip's fields and length.

## 0.64.1-beta.1

- Video with transparency (ProRes 4444, WebM with alpha, QuickTime
  Animation) has clean edges over other tracks, like PNGs and titles; such
  video and transparent image sequences are no longer offered proxies,
  which would lose the transparency.

## 0.64.0-beta.1

- Bake Title: turn a title clip into a ProRes 4444 file with transparency
  that any program can play, in one undoable step (Title page › Bake…).

## 0.63.0-beta.1

- Exports started while GPU acceleration is on render on the graphics card
  too, so soft edges and dissolves look exactly as in the preview; turning
  it off mid-export doesn't disturb that export.

## 0.62.0-beta.1

- The inspector has a Title page: select a title clip and fill in its
  fields (a guest's name and role), each clip its own, one title file for
  all of them.

## 0.61.1-beta.1

- Exporting with GPU acceleration on no longer closes the editor.

## 0.61.0-beta.1

- Titles can show live text: `{{timecode}}`, `{{clip_time}}`,
  `{{countdown:mm:ss}}` and `{{date}}` update as the video plays.
- Fixed: a title clip that didn't start at the beginning of the timeline
  played its intro and outro at the wrong moments.

## 0.60.0-beta.1

- GPU acceleration (Settings › Performance › Hardware, on by default where a
  startup check passes): the preview composites tracks and places clips on
  the graphics card, so several moved or scaled 1080p tracks play smoothly
  at full size; soft edges and dissolves blend in linear light. Hardware
  video decoding is used alongside it. Exports still use the processor.

## 0.59.1-beta.1

- Single-key shortcuts (Space, J/K/L, X, Delete, letters) no longer fire
  while you type in any text box in the editor window, such as a field in
  Settings; a click on the timeline or preview gives them back.

## 0.59.0-beta.1

- U Stu Titles animates: an animation strip with the intro, hold and outro
  (drag to retime, click to scrub, Space to play), 17 ready-made
  behaviours to add from a drawer of live thumbnails (fade, rise, pop,
  typewriter, word by word, scramble, wipe, kinetic stack, float, shimmer,
  glow and more), and keyframes on position, rotation, scale, opacity and
  blur.
## 0.58.1-beta.1

- Transparent pictures (PNG stills, titles, rotated or scaled clips) no
  longer get dark, coloured edges over video, in the preview and in renders.
- A still image or image sequence with its video turned off no longer shows.

## 0.58.0-beta.1

- U Stu Titles: Export (Ctrl+E) a title on its own for OBS and other apps:
  ProRes 4444, WebM VP9 or QuickTime Animation with a transparent
  background, a PNG sequence, or H.264 on its background.

## 0.57.0-beta.1

- U Stu Titles: double-click text to type on the canvas; picture (PNG)
  layers. In the editor, double-click a title clip (or Edit Title) to open
  it in U Stu Titles over the video at the playhead.

## 0.56.0-beta.1

- Project background colour: click the title in the header bar to pick the
  colour shown wherever no clip covers the frame, in the preview and in
  renders (black by default). Saved with the project; one undo step.

## 0.55.0-beta.1

- U Stu Titles: a layers list (show, hide, lock, restack by dragging) and
  an inspector for text, fonts, fills and gradients, outlines, shadows and
  exact positions, with the title's own background and timing when nothing
  is selected. The Unicorn Tears colours, gradients and fonts are one
  click away, and Apply Brand restyles a whole title.

## 0.54.0-beta.2

- Clips of another shape than the project (4:3, phone video) play much
  faster at their default Fit.
- Exporting at another size (e.g. 720p from a 1080p project) places moved,
  scaled and fitted pictures correctly; they used to run off to the right.

## 0.54.0-beta.1

- U Stu Titles, the title designer (with the Titles drop-in; early): add
  text and shapes, move them with snapping to the centre, safe areas and
  other layers, resize, undo, save and open. Design over a checkerboard,
  any colour or a picture. A title can carry its own background colour or
  gradient, or none for a transparent overlay.

## 0.53.0-beta.1

- Titles, with the Titles drop-in (not in the packages yet): `.ustitle`
  files import as title clips that play over the video with transparency,
  fit any clip length (only the hold stretches), and update in the editor
  within a second when the file is saved.

## 0.52.0-beta.2

- Colours are right: BT.709 footage (most HD video) was shown and exported
  with slightly wrong colours (pure red came out 233 of 255, cyan picked up
  red), because the graph's black background labelled every frame BT.601.

## 0.52.0-beta.1

- Image sequences can be relinked (pick any of their images, or search a
  folder) and get proxies.

## 0.51.0-beta.3

- Open Log Folder opens the folder the first time too, with this
  session's log selected.

## 0.51.0-beta.2

- The app is now called U Stu Video Editor (window title, Help, the app
  menu, the software centre). Its app id, command and .ustudio projects
  are unchanged.

## 0.51.0-beta.1

- Import Image Sequence… (Ctrl+Alt+I): numbered images as one clip, one
  image per frame; pick any of them. Stills now know their size when
  imported, so an odd-sized picture fits the frame by its real aspect.

## 0.50.0-beta.5

- Help › About shows the new logo, says what the editor is and that it's
  part of the Unicorn Tears Project, links the source code, and states the
  MIT licence (the Flatpak's FFmpeg and x264 are GPL).

## 0.50.0-beta.4

- Fixed a crash some seconds after changing the project frame rate (and
  undoing it): the dialog's list was freed twice.
- No Gtk-CRITICAL on quit (the clip tooltip was read after the window
  had gone).
- The app icon is the owner's new logo (landed in 29502ad).

## 0.50.0-beta.3

- No more crackle in the first half second after pressing play on a large
  project: playback waits a quarter second for its first frames.

## 0.50.0-beta.2

- The clip tooltip on the timeline no longer flashes: its own appearance
  sent a motion and a leave that hid and re-armed it.

## 0.50.0-beta.1

- The media browser no longer redraws every row while a folder imports:
  new files are appended. A releasable beta.

## 0.50.0

- Media browser badges: each asset shows what it is (image, sequence,
  audio), its resolution (cyan when taller than the project) and its state
  (probing, failed, missing, proxy and its progress, proxy missing). Large
  bins stay responsive: the list updates only the rows that changed.

## 0.49.0-beta.2

- Help › About gains Open Log Folder and Copy Diagnostics (app, MLT, GTK
  and libadwaita versions, whether it runs in Flatpak, the log folder and
  the last 50 log lines) for bug reports. Also in Keyboard Shortcuts under
  Help.

## 0.49.0-beta.1

- Help: every section of Controls and Keyboard Shortcuts folds away and
  opens with a click; Help remembers the open sections, the tab and where
  you scrolled, also after a restart. A new Release notes tab lists each
  released build's notes, newest first. This is the first beta.

## 0.48.0

- Transform handles on the preview: click a picture to select it, then
  move, scale, stretch, rotate and crop it by dragging, with snapping to
  the frame and other pictures and arrow-key nudges. A right-click menu
  resets, fits, stretches, centres, flips and rotates it, and Edit
  Transform (Ctrl+T) sets exact numbers. Undo and redo now keep the
  selection.

## 0.47.2

- Auto preview scale plays at Half once a clip is moved, scaled, rotated,
  cropped or flipped, so a transformed overlay plays smoothly at 1080p;
  the dropdown shows what Auto chose, e.g. "Auto (Half)".

## 0.47.1

- Pictures of another size or aspect are fitted and centred in the frame
  (a 4:3 or 1344×768 source no longer sits at the left), and each clip
  now keeps a transform (position, size, rotation, crop, flip) that is
  saved, survives split, trim, ripple and copy, and renders exactly as it
  plays; project format 6, older projects load fitted (M4 F1).

## 0.47.0

- Proxies: Create Proxy / Create Conformed Proxy in the media browser, a
  Proxies toggle beside the preview scale, and an offer once per project for
  footage above 1080p. Made in the background by `u-studio-render --proxy`;
  renders always use the originals.

## 0.46.1

- A video smaller than the project (720p or 640x360 in a 1080p project) now
  fills the frame; it played at its own size in the top-left corner.

## 0.46.0

- Missing media: a project whose files moved opens with those clips striped red
  (playing dark red), a banner, and a Relink dialog (locate each file or
  search a folder); relinking changes nothing else and undoes in one step.
  Renders ask before using missing media.

## 0.45.2

- Saving an untitled project, or quitting with nothing unsaved, no longer
  leaves an autosave behind that the next launch offers to recover (which
  also stopped the last project reopening).

## 0.45.1

- A long status message (an import of many files at another frame rate)
  no longer widens the window past the screen: it's cut short with the full
  text on hover, and rate notes are grouped ("8 are 25 fps; the project is
  30, so frames will repeat").

## 0.45.0

- Import Folder (Ctrl+Shift+I, or drop a folder): every file under it into the
  bin. An import is one undo step however many files it has, and files that
  can't be imported are listed with the reason.

## 0.44.0

- Settings › Drop-ins: see the installed drop-ins and switch each on or off
  (from the next start), how to get the ones not installed, and why any
  couldn't load. u Studio never downloads them.

## 0.43.1

- Help's Keyboard Shortcuts lists "Nudge Selection Left 10 Frames" with its
  shortcuts again (a "<" in the label broke the row).

## 0.43.0

- Change the project's frame rate: click the title (it now shows the
  project's size and rate). Everything keeps its timing; undo reverts it.

## 0.42.0

- Render profiles have a frame rate: render a 30 fps project at 60, 24 at
  23.976 and so on, with every cut kept in time (Settings > Render).

## 0.41.0

- A new project takes its size and frame rate from the first video you
  import (undo reverts both); importing a clip at a different rate notes
  that frames will repeat or be skipped. Rates show as 23.976/29.97/59.94.

## 0.40.2

- Settings now persist when the editor is run straight from its build
  directory (they silently reset on every launch before, so "reopen last
  project" never had anything to reopen); a banner says so if they can't.

## 0.40.1

- Launched from a snap app's terminal (VS Code's), the editor no longer
  inherits that snap's module paths, so "Open Render" starts the player
  cleanly.

## 0.40.0

- Settings > Render > Render threads: how much of the machine a render may
  use (default 80%); renders of composited projects are about 20% faster.

## 0.39.0

- The hover preview is now part of the clip tooltip: the frame under the
  pointer and its timecode above the clip's name, in–out, length and
  source (Settings > Toggles > Thumbnails in clip tooltips).

## 0.38.1

- No more floods of FFmpeg "deprecated pixel format used" warnings with
  full-range (phone/camera) video: MLT's FFmpeg logging is errors-only
  unless debugging.

## 0.38.0

- Hover preview: rest the pointer on a video clip to see the frame under it
  with its timecode (Settings > Toggles to turn it off).

## 0.37.0

- Render queue: the Render button shows progress, a queue count, and
  "Open Render" when done; click it mid-render to cancel or queue another,
  right-click to render or queue with any profile (auto-named). Quitting
  mid-render keeps unfinished renders and offers to restart them next time.

## 0.36.0

- Render profiles (Settings > Render): output height and quality presets,
  or exact bitrates. The Render button uses the default profile, now
  "High quality" (x264 CRF 18); the old fixed bitrates are kept as
  "Draft (legacy)".

## 0.35.0

- Settings: new Toggles tab (reopen last project on startup, snapping,
  follow playhead, timeline thumbnails, waveforms), Performance tab
  (preview scale, worker threads, thumbnail/waveform jobs) and Locations
  tab (default project and export folders). The header bar shows just the
  project name.

## 0.34.0

- Header bar: zoom out / zoom in / fit buttons, and the buttons regrouped
  (Import, Add track, media browser | Undo, Redo on the left; zoom |
  project | Render | Settings, Help on the right).

## 0.33.0

- The Save button saves in place (Save As for an untitled project);
  right-click it for Save As. Each save keeps the previous file in
  `.ustudio-backups/` beside the project (newest 5).

## 0.32.0

- "Sync Tracks (Audio)": with two clips selected, right-click the one to
  keep still and the other moves so their sound lines up (within 5 s).

## 0.31.0

- `A`/`F` also stop at markers.

## 0.30.0

- `Shift+S`/`Shift+D` move a single selected clip up/down to the nearest
  track where it fits (skipping tracks where it would overlap another
  clip); with nothing selected they still jump to the top/bottom track.

## 0.29.0

- `A`/`F` now jump to the previous/next cut on any track; `Shift+A`/
  `Shift+F` keep the old active-track-only jump. `Shift+S`/`Shift+D` make
  the top/bottom track active.

## 0.28.2

- Fixed: playback could crash while thumbnails and waveforms were being
  generated for a project with several video files.
- Thumbnails and waveforms are generated on the shared worker pool, a
  little faster.

## 0.28.1

- Fixed: pressing play right after an edit could leave playback stuck on
  one frame.
- Fixed: a quick run of edits while playing rebuilt playback once per
  edit instead of catching up with the latest one.

## 0.28.0

- Edits no longer freeze the window while playback catches up: rebuilding
  the playback timeline and restarting playback run on their own thread.

## 0.27.1

- Edits on large projects rebuild playback much faster: 5,000 clips on
  one track went from about 20 s per edit to 0.35 s. Projects with many
  dissolves no longer grow to tens of gigabytes of memory after a few
  edits.

## 0.27.0

- Settings has a "Worker threads" option for the background work (import,
  load, save): Automatic by default, applies after a restart.

## 0.26.0

- Opening, reloading and recovering a project read the file in the
  background; opening another project while one is still loading replaces
  it, and an edit made meanwhile brings back the "Discard unsaved
  changes?" question.

## 0.25.0

- Saving and autosaving write in the background, so a big project no longer
  freezes the window while saving; closing or quitting waits for a save
  that's still writing.
- Fixed: undoing past the last save and then making a new edit could show
  the project as saved when it wasn't.

## 0.24.0

- Importing several files probes them in parallel off the main thread, shows
  "Importing 3 of 12…", keeps them in the order picked, and reports and
  skips a file it can't open; Import, timeline drops and media-browser
  drops all work this way.

## 0.23.2

- Release builds log at Info by default instead of Debug (debug builds are
  unchanged); `USTUDIO_LOG_LEVEL` still overrides either way.

## 0.23.1

- Fixed: Shift+Delete crashed after removing a media file whose clips were
  selected.
- Fixed: quitting while a render ran crashed and left a `.part` file; the
  app now asks, stops the render and cleans up.
- Fixed: playback could freeze for good when a consumer was stopped just
  after it started.
- Fixed: a ripple or plain move that put a clip back where it was deleted
  its dissolves; a dissolve could no longer be made as long as a whole clip.
- Zooming no longer leaves thumbnails decoding for levels already left.

## 0.23.0

- Ripple mode (R, or the Ripple button): moving a clip closes the gap it
  leaves and pushes later clips along where it lands.
- Keyboard: Tab / Shift+Tab select the next / previous clip on the active
  track, Up / Down change track, comma / period nudge the selection a frame
  (Shift: ten).
- Pinch to zoom the timeline.

## 0.22.0

- The timeline draws with GTK's scene graph (a custom widget) and stays
  fast with thousands of clips on screen; video clips show a strip of
  thumbnails; a move or copy that would be refused shows red while you
  drag.

## 0.21.1

- Fixed: a head trim could push a clip past the clip it dissolves into,
  leaving them overlapping; a ripple trim on a clip's head left its
  outgoing dissolve partner behind; and redo of an edit made on a copied
  clip failed (the copy came back with a different identity).

## 0.21.0

- Ripple trim (Alt+drag an edge), slip (Shift+drag an edge), copy
  (Ctrl+drag), ripple delete (Shift+Delete), and markers (M adds one at
  the playhead, Shift+M removes it).

## 0.20.0

- Select several clips (Shift+click, Ctrl+click, Shift+drag a box, Ctrl+A;
  Escape clears) and drag them together as one undoable move.

## 0.19.0

- Dragged clips and edges snap to clip edges on every track, markers and
  the playhead, never to their own old position, with a magenta line where
  they snap.
- Trimming the end of a clip works for every clip. It used to go wrong for
  any clip not cut from the start of its file (refused, wrong length, or a
  dissolve instead of a trim).
- Delete removes every selected clip as one undo step.

## 0.18.0

- Zoom the timeline with Ctrl+wheel or +/-, fit it with 0, and scroll it
  sideways (Shift+wheel, touchpad, scrollbar) or vertically when tracks
  don't fit. The view follows the playhead during playback.

## 0.17.2

- The preview scale setting now takes effect: Half and Quarter (and Auto
  on 4K projects) render playback at the smaller size, so large media
  plays much more smoothly. The saved default preview scale is also
  applied at startup.

## 0.17.1

- The app always uses its dark theme; under a light system theme, Help
  and Settings showed white lists.

## 0.17.0

- Hide or mute a track from its right-click menu; the row's name strip
  shows "Hidden"/"Muted".
- Help has a new Controls tab explaining every button, menu item and
  timeline gesture, and every control has a tooltip that names its current
  shortcut.

## 0.16.5

- Rendering works on stock Fedora, which has no libx264: it falls back to
  OpenH264. Before, the MP4 had no video.
- Images import as stills even where the system image loaders can't run.

## 0.16.4

- Closing a gap no longer removes the dissolves between the clips it moves.

## 0.16.3

- Autosave now also runs while you edit continuously, so a crash loses at
  most 2 minutes of work. Before, it waited for 2 minutes with no edits.

## 0.16.2

- Saved projects now play exactly as in the editor when opened by other
  MLT tools (melt): dissolves, track volume and split audio included.
  Previously everything after a dissolve played late and the dissolve was
  a hard cut. Older project files still open normally.

## 0.16.1

- Fixed Split Audio doubling the sound: the video half of a split clip kept
  playing its audio underneath the new audio clip.

## 0.16.0

- Full transport controls next to the play button: go to start, shuttle
  reverse, step back, stop, step forward, shuttle forward, go to end.
- X splits the active track's clip at the playhead.

## 0.15.5

- Stepping frame by frame quickly (holding an arrow key, or clicking the
  step button repeatedly) no longer drops steps.
- Fixed a rare flash of the first frame in the preview right after an edit
  while paused (a regression from 0.15.3's playback change).

## 0.15.4

- Fixed Ctrl+S (and Save As, Render, Import) failing with "failed to write
  /run/user/…/doc/…" when the file picker handed back a sandbox portal
  path: the app now saves to the file's real location.

## 0.15.3

- The app now quits cleanly on logout, shutdown, `kill`, or Ctrl+C in a
  terminal, saving an autosave first if there are unsaved changes.
  Previously it ignored these until forcibly killed.

## 0.15.2

- Fixed a crash on quit, and a possible crash when refreshing the recent
  projects menu, caused by freeing timestamps the recent-files list still
  owned.

## 0.15.1

- Fixed memory growing with every edit: each edit leaked the previous
  timeline's transitions (roughly 100–330 KB per edit), plus a smaller
  per-edit leak in the black background track.

## 0.15.0

- Added a Help dialog (header bar `?` button) with Keyboard Shortcuts and
  About tabs, and a Settings dialog (gear button) with General and
  Playback tabs — autosave delay, recent-projects list size, default
  preview scale, and maximum shuttle speed are now user-configurable and
  persisted via GSettings, instead of hardcoded.

## 0.14.7

- Pausing now stops on the frame that was on screen, instead of jumping
  about a second ahead.

## 0.14.6

- A timecode ruler now runs along the top of the timeline, with ticks
  that adapt to the current zoom level.

## 0.14.5

- Click and drag on empty timeline space to scrub, following the pointer
  continuously instead of only seeking once you release.

## 0.14.4

- Moving or trimming a clip now snaps to nearby clip edges and the
  playhead when within about 8 pixels.

## 0.14.3

- Rendering now shows a live percentage in the status bar instead of
  only a start and finish message.

## 0.14.2

- Import (Ctrl+I) supports selecting multiple files at once.
- Import and Open Project file pickers now filter to media files and
  `.ustudio` projects respectively, instead of showing every file.
- Drag files in from the file manager onto the timeline to import and
  place them, or onto the media browser to just add them to the project.
- Double-click a media browser row to insert it at the playhead on the
  active track.

## 0.14.1

- The window title now shows the current project's name (or "Untitled
  Project") instead of just "u Studio Video Editor" for every project.
- A "Recent projects" button next to Open lists the last 10 projects
  opened or saved, for one-click reopening.

## 0.14.0

- Space bar toggles play/pause.
- Delete removes the selected clip (leaving a gap, same as right-click →
  Delete Clip).
- Ctrl+S saves in place once the project has a file, without opening a
  dialog; Ctrl+Shift+S always opens the Save As dialog. Ctrl+O/N/I open
  a project, start a new one, and import media.

## 0.13.6

- The timeline playhead now updates on its own lightweight overlay
  instead of redrawing every clip, label, and waveform on the timeline
  30 times a second during playback.

## 0.13.5

- Opening a different project now asks first if the current one has
  unsaved changes, matching Reload and New Project — previously it went
  straight to the file picker with no warning.

## 0.13.4

- "Delete File…" in the media browser is now "Move File to Trash…" — it
  moves the file to your desktop's Trash instead of permanently deleting
  it, so it's still recoverable afterward.

## 0.13.3

- Closing the window with unsaved changes now asks first (Save, Discard,
  or Cancel) instead of quitting immediately and losing them. Choosing
  Discard (or any other path to quitting while still dirty) still leaves
  a final autosave behind as a recovery point.

## 0.13.2

- Renaming a track or clip inline no longer triggers transport shortcuts
  (previous/next cut, shuttle, step, loop in/out…) while you're typing —
  bare letters and arrow keys used to be swallowed by those instead of
  going into the name field.

## 0.13.1

- Trimming, moving, or splitting a clip that has a dissolve transition on
  it no longer refuses the edit, or silently drops the transition, when
  the edit doesn't actually touch the transition's own overlap — only
  edits that reach into the overlap strip it now.
- Splitting a clip whose far edge has a dissolve now keeps the
  transition, moved onto whichever half of the split still owns that
  edge, instead of always stripping it.
- A clip drag that ends back exactly where it started no longer strips
  the clip's dissolve transition.
- Deleting a media asset from the media browser no longer leaves a
  dangling dissolve transition on a clip it removed, or on a surviving
  clip that shared the dissolve with a removed one.

## 0.13.0

- Timeline playhead: a vertical cyan line at the current frame, spanning
  every track, updating live during playback and scrubbing — not just
  the seek bar below the preview.

## 0.12.8

- Importing several media files at once no longer rebuilds the entire
  (usually hidden) media browser panel once per finished thumbnail; it
  now catches up in one rebuild whenever the panel is next opened.

## 0.12.7

- Fixed autosave recovery being able to treat a crashed session's owner
  as "still alive" (and so never offer its autosave) if an unrelated
  process later reused the same pid -- the process's own start time is
  now cross-checked too, not just the pid.

## 0.12.6

- Reload and New Project now confirm ("Discard unsaved changes?") before
  replacing the current project if it has unsaved edits, matching Save's
  own guard elsewhere -- both used to replace the model on a single
  click with no way back.

## 0.12.5

- Fixed a video with no audio track getting a waveform decode job and a
  "Split Audio" menu item that produced an empty, silent clip; whether
  media actually has audio is now checked instead of guessed.

## 0.12.4

- Fixed undo of importing or resizing a still image/logo overlay leaving
  its recorded source length extended instead of restoring it, an
  internal inconsistency between the model and what undo is supposed to
  guarantee (harmless to playback, but wrong on inspection or in a saved
  project's `out` point).

## 0.12.3

- Fixed a data-loss bug: recovering an unsaved autosave, then starting a
  New Project (or Open/Reload) without saving the recovered work first,
  then later saving that different project, deleted the recovered
  autosave -- the only copy of the original unsaved work.

## 0.12.2

- Fixed media browser thumbnails for real (non-still) video: they were
  decoded through MLT's default 4:3-ish profile, so every 16:9 clip's
  thumbnail showed black letterbox bars and squashed pixels instead of
  its real shape.

## 0.12.1

- Fixed crashes and corrupted saved projects from editing a clip linked
  by a dissolve transition: deleting, moving, resizing, or splitting a
  linked clip now cleanly removes the dissolve first instead of leaving
  a dangling or mis-sized transition record. Also fixed a crash triggered
  by this fix during testing: some edits restarted the real audio device
  twice in immediate succession, which can segfault deep in the system
  audio stack; those edits are now batched into a single restart.
- Fixed a project being able to save with a corrupted transition that
  would then fail to reopen at all: Save now refuses (with a status
  message) if the project fails its own internal consistency check.
- Fixed adding a dissolve transition that combines with an existing one
  on the same clip to exceed the clip's own length, which laid out the
  rest of the track incorrectly.

## 0.12.0

- Media browser: right-click a row for "Remove from Project" (drops the
  asset and every clip cut from it) or "Delete File…" (confirms, then
  also deletes the file from disk). Drag a row onto the timeline to
  insert a full-length clip at the drop's exact track/frame.

## 0.11.0

- Keyboard shortcuts: `Ctrl+Left`/`Ctrl+Right` jump the playhead 10
  frames; `Alt+Left`/`Alt+Right` jump a minute, clamping to the
  timeline's start/end when less than a full minute remains in that
  direction.

## 0.10.1

- Fixed project recovery on launch picking an arbitrary orphaned autosave
  instead of the most recent one when several qualified at once, which
  could silently surface an older, thinner autosave over a richer one
  from the same crashed/unsaved session. The recovery dialog now also
  loops through every independently orphaned autosave in one launch
  instead of stopping after the first.

## 0.10.0

- Keyboard shortcuts: `A`/`F` jump the playhead to the previous/next cut
  on the active track (a clip's start or end, or the timeline's own
  start/end); `S`/`D` move the active track up/down.

## 0.9.1

- Fixed a crash: a project referencing media that's since been moved,
  renamed, or deleted would segfault the whole app the moment playback
  or a redraw reached that clip, instead of failing gracefully. That
  clip now plays as black and a status message names the missing file.

## 0.9.0

- Collapsible media browser panel, to the left of the video preview:
  thumbnail, name, length, fps, and format for every imported asset.
  Toggle it from the new header-bar button next to "Add track". Import
  now also probes and records fps/dimensions (previously left at 0) and
  the file's format (from its extension).

## 0.8.0

- Dissolve transitions: drag either edge of an existing dissolve to grow
  or shrink it; right-click near where two touching clips meet for "Add
  Transition" (a default ~half-second dissolve) as an alternative to
  dragging one in. Fixed: double-clicking a track's name label could
  instead open the clip name editor if a clip happened to sit under it;
  a clip's own name now takes priority over its track's name in the
  corner badge once set (previously the track's name always won).
  Context menu: added "Edit Track Name"; renamed "Edit/Add/Remove Name"
  to "…Clip Name" to disambiguate from the new track one.

## 0.7.0

- Dissolve transitions between two adjacent clips on the same track.
  Drag a clip's edge past its exactly-touching neighbor (the same
  trim-drag gesture used for ordinary trims) to create a dissolve,
  shown as a diagonal-hatch overlay on the overlap; right-click the
  overlap for "Remove Transition". Creating one grows each clip using
  its own existing source-media handle, so nothing else on the track
  ever needs to move. Undoable, and saved/loaded with the project
  (project file format bumped to v3 for this — see the README).

## 0.6.0

- Tracks and clips can now be named. Double-click a track's name strip
  or a clip to edit its name inline; right-click a clip for "Add
  Name"/"Edit Name" and, once it has one, "Remove Name". Two clips cut
  from the same source can carry different names. A named track shows
  its name in the top-left corner of every clip on it. Hovering a clip
  shows a tooltip with its name, start/end timecodes, length (timecode
  and frame count), and source file.

## 0.5.2

- Fixed Play doing nothing after any seek (scrubbing, clicking the
  timeline, or even just the very first play after importing a clip):
  the playback engine left an internal "show one frame" flag set from
  the last pause/seek, which silently blocked continuous playback from
  ever resuming until the project was reloaded.

## 0.5.1

- Fixed a severe slowdown importing or editing a long clip (measured 18
  seconds on a real ~62-minute recording): computing its waveform used
  to decode every single video frame just for the audio peak display,
  saturating a CPU core decoding the same file the live preview was
  trying to play from at the same time. Waveform decoding is now capped
  to a bounded number of samples regardless of clip length (2.5 seconds
  for that same recording), with no visible loss of detail in the
  drawn waveform.

## 0.5.0

- New header-bar buttons next to Save/Open: Reload (re-opens the current
  project's file from disk) and New Project (resets to a fresh, empty,
  untitled project).
- Debug-level logging is now on by default (`USTUDIO_LOG_LEVEL=info` for
  the old, quieter behavior).

## 0.4.4

- Fixed a crash and "playback stopped working" after the app was
  launched a second time while already running (a second double-click,
  a second terminal invocation): re-launching used to silently open a
  second editing window with its own audio device instead of presenting
  the one already open, leaving two audio consumers fighting over the
  same output.
- Debug-level logging (`USTUDIO_LOG_LEVEL=debug`) now covers every
  user-facing status message and the playback engine's internal timing,
  for diagnosing both bugs and slow operations.

## 0.4.3

- The undo/redo buttons and the title bar's unsaved-changes mark now
  update after every edit (import, split, move, trim, delete, add/remove
  track, lock, volume) instead of staying stale until the first undo.
- Recovering unsaved work after a crash now correctly shows as unsaved,
  and its autosave is kept until a manual Save actually lands the
  recovered content somewhere durable, instead of being deleted right
  after recovery.
- "Close Gap" now closes the whole gap, not just the part after where you
  right-clicked.
- Importing or dragging a clip onto an audio track no longer lets its
  video show through wherever the video tracks above have a gap; dragging
  a video-only clip onto an audio track is refused instead of parking a
  dead clip there.
- A second running instance's own in-progress autosave is no longer
  offered (and possibly deleted) by another window's crash-recovery
  prompt.
- Save and Render both refuse to write over a file that's already one of
  the project's own media sources; a render that fails partway through no
  longer leaves a partial file at the name you asked for.
- Clicking the volume slider, a track's volume slider, or the preview-scale
  dropdown no longer swallows the Left/Right/Home/End playhead shortcuts.

## 0.4.2

- Fixed a still image/watermark stretched past its imported length
  playing shorter than the timeline showed, shifting every clip after it
  once the mismatch resolved itself later.
- Fixed the loop range affecting a seek or frame-step while paused, even
  well past the loop's own out point.
- Fixed the unsaved-changes indicator sometimes reporting "no changes"
  right after a save when a new edit immediately followed one already on
  the undo stack.
- Opening a corrupted or hand-edited project file now fails with a clear
  reason instead of silently loading invalid data.
- Fixed a rare crash opening a project while the previous one's engine
  state was still tearing down.

## 0.4.1

- Fixed playback silently failing to advance after an edit (import,
  split, move, trim, ...): an optimization meant to avoid closing and
  reopening the audio device on every edit was instead corrupting MLT's
  internal playback state. Every edit now does a clean restart of the
  playback consumer, which is slightly more expensive but actually
  correct.

## 0.4.0

- Track locking: right-click empty track space to lock/unlock a track.
  A locked track refuses insert/move/resize/split/remove on its clips
  (and as a move/Split Audio destination) until unlocked; locked rows
  get a subtle tint.
- Per-track volume: a slider in the same right-click menu sets each
  track's level independently (previously only a single, whole-project
  volume control existed).

## 0.3.0

- "Split Audio" on a clip's right-click menu: pulls its audio out to a
  new, independent clip on the nearest audio track with room (creating
  one if none has room). The two halves can then be moved and trimmed
  independently of each other.

## 0.2.0

- Importing a still image (PNG, JPEG, ...) now auto-detects it as such and
  defaults its length to span the rest of the current project from the
  insert point, instead of MLT's fixed ~8-minute default — drop one onto
  an empty top track for an instant full-timeline watermark/logo overlay.

## 0.1.0

- Playback now runs through a real MLT consumer (`sdl2_audio`, falling
  back to `rtaudio`, then `null`) instead of a hand-rolled pull loop —
  removes the ~150ms A/V offset and PulseAudio dependency of earlier
  builds.
- New transport controls: `J`/`K`/`L` shuttle (with speed ramping),
  frame-step (`Left`/`Right`), jump to start/end (`Home`/`End`), loop
  in/out (`I`/`O`), a volume slider, and a preview-scale preference
  (Auto/Full/Half/Quarter).
- Undo/redo for every edit, with labels and `Ctrl+Z`/`Ctrl+Shift+Z`.
- Project files are now MLT XML with `ustudio:` properties instead of a
  bespoke `GKeyFile` format — `.ustudio` files can be played directly by
  `melt` with no editor involved.
- Autosave and crash recovery: unsaved work is offered back on the next
  launch after a crash.
