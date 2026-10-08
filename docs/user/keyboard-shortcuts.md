# Keyboard shortcuts

[Docs home](../README.md) › [User guide](README.md) › Keyboard shortcuts

The app's own list is always current: **Help › Keyboard Shortcuts**. This
page matches `src/app/action_registry.cpp` as of 0.50.0-beta.2.

While you type in a text box (a clip or track name, a field in Settings or
the inspector), keys without `Ctrl` type into the box instead: Space types
a space, it doesn't play. Click the timeline or the preview to use them again.

## Playback

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `J` / `K` / `L` | Shuttle reverse / stop / forward (repeat to speed up) |
| `Left` / `Right` | Step one frame |
| `Ctrl+Left` / `Ctrl+Right` | Step 10 frames |
| `Alt+Left` / `Alt+Right` | Step one minute |
| `Home` / `End` | Go to start / end |
| `I` / `O` | Set loop in / out |

## Editing

| Key | Action |
|---|---|
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `X` | Split clip at playhead |
| `Delete` | Delete selected clips (leave a gap) |
| `Shift+Delete` | Ripple delete (close the gap) |
| `R` | Ripple mode on / off |
| `A` / `F` | Previous / next cut or marker |
| `Shift+A` / `Shift+F` | Previous / next cut on the active track |
| `S` or `Up` / `D` or `Down` | Active track up / down |
| `Shift+S` / `Shift+D` | Move the selected clip a track up / down (with nothing selected: active track to top / bottom) |
| `M` / `Shift+M` | Add / remove marker at playhead |
| `Tab` / `Shift+Tab` | Select next / previous clip |
| `,` / `.` | Nudge selection one frame left / right (add `Shift` for ten) |
| `Ctrl+A` | Select all clips |
| `Escape` | Clear selection |

## Timeline view

| Key | Action |
|---|---|
| `+` or `=` / `-` | Zoom in / out |
| `0` | Zoom to fit |

## Project

| Key | Action |
|---|---|
| `Ctrl+N` | New project |
| `Ctrl+O` | Open project |
| `Ctrl+S` | Save |
| `Ctrl+Shift+S` | Save as |
| `Ctrl+I` | Import |
| `Ctrl+Shift+I` | Import folder |
| `Ctrl+Q` | Quit (asks first if there are unsaved changes) |

## Transform

| Key | Action |
|---|---|
| `Ctrl+T` | Edit Transform (exact values) |

## Effects

With the Effects add-on ([Effects](effects.md)). Single keys don't fire
while you're typing in a text field.

| Key | Action |
|---|---|
| `E` | The Add page: find, try and add effects and looks |
| `Enter` (in its search) | Add the best match to the selected clips |
| `Esc` (on the Add page) | Stop trying an effect on the picture |
| `P` | Pin the value you last changed at the playhead (a keyframe) |
| `Ctrl+Shift+C` | Copy the effects on the Effects page |
| `Ctrl+Shift+V` | Paste effects onto the selected clips (after or instead of theirs) |
| `\` (hold) | See the picture without the selected clip's effects |
| `T` | Add a dissolve at the cut nearest the playhead, and pick its style |
| `C` | Show or hide the selected clip's curve lanes |

## Titles (in the editor)

| Key | Action |
|---|---|
| `Shift+T` | New Title: a new title at the playhead, opened in U Stu Titles with the template gallery |
| `Ctrl+Shift+T` | Edit Title: open the selected title clip in U Stu Titles (or double-click it) |

## U Stu Titles (the title designer)

Keys that work on the canvas only act while the canvas has focus (click
it). Typing in a text box never triggers them.

| Key | Action |
|---|---|
| `Ctrl+N` | New title (in a new window) |
| `Ctrl+Shift+N` | New from Template: the template gallery |
| `Ctrl+O` | Open a title |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save as |
| `Ctrl+E` | Export the title on its own (with alpha, or flattened) |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / Redo |
| `Ctrl+T` | Add text |
| `Ctrl+Shift+R` | Add a rounded rectangle |
| Double-click text | Type on the canvas (`Enter` keeps it, `Shift+Enter` a new line, `Escape` cancels) |
| `Ctrl+;` | Show or hide the safe areas |
| `Delete` / `Backspace` | Delete the selected layer (canvas) |
| Arrow keys | Nudge the selected layer 1 pixel; with `Shift`, 10 (canvas) |
| `Escape` | Clear the selection (canvas) |
| `Space` | Play or stop the title: intro, two seconds of hold, outro (canvas) |
| `Alt` + drag | Move without snapping |

## Mouse modifiers

| Gesture | Action |
|---|---|
| `Ctrl` + wheel | Zoom the timeline |
| `Shift` + wheel | Scroll sideways |
| `Shift` + click / `Ctrl` + click | Add to / toggle selection |
| `Shift` + drag on empty space | Box-select |
| `Ctrl` + drag a clip | Copy |
| `Alt` + drag a clip edge | Ripple trim |
| `Shift` + drag a clip edge | Slip |
| `Ctrl` while dragging | Don't snap |
| `Shift` on a preview corner / knob | Free aspect / 15° rotation steps |
| `Alt` on a preview handle | Crop |

Shortcuts can't be changed yet. The Settings tab for that is a
placeholder.
