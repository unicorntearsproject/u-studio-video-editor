# Projects and saving

[Docs home](../README.md) › [User guide](README.md) › Projects and saving

## Project files

Projects are saved as `.ustudio` files. A project file only records your
edit and refers to your media, so the media files themselves stay where
they are. If you move media later, the project still opens and you can
relink it ([Missing media](importing-media.md#missing-media)).

A `.ustudio` file is also valid MLT XML, so MLT's `melt` player can play it
directly.

## Commands

| Action | Shortcut | Notes |
|---|---|---|
| New Project | `Ctrl+N` | Starts an empty, untitled project |
| Open Project… | `Ctrl+O` | Only shows `.ustudio` files |
| Recent projects | clock button | The last 10 projects you opened or saved (change the number in Settings) |
| Reload | refresh button | Re-reads the current project from disk |
| Save | `Ctrl+S` | Saves to the project's own file; asks for a name the first time |
| Save As… | `Ctrl+Shift+S`, or right-click Save | Always asks for a name |

- The window title shows the project name, with a `•` while there are
  unsaved changes.
- New, Open, Recent and Reload ask before discarding unsaved changes.
- Closing the window offers **Save**, **Discard** or **Cancel**.
- Opening and saving happen in the background, so the window never freezes
  on a big project. Closing while a save is still writing waits for it to
  finish.
- By default, the last project reopens when you start the app (Settings ›
  Toggles).

## Backups

Before a save overwrites a project file, the previous version is copied to
a `.ustudio-backups/` folder beside it, named
`<project>-YYYYMMDD-HHMMSS.ustudio`. The newest five are kept. Autosaves
don't make backups.

## Autosave and crash recovery

- The project is autosaved **2 minutes** after your last edit (you can
  change the delay in Settings), whenever you switch away from the window,
  and while you keep editing. So a crash loses at most about two minutes
  of work.
- If the app closes unexpectedly, the next launch offers to recover your
  work. If there's more than one unsaved project, you're asked about each.
- Recovered work stays marked unsaved until you save it somewhere, and the
  recovery copy is kept until then.

## Safety

- U-Stu never overwrites your source media. Save and Render refuse any
  path that is one of the project's own media files.
- Renders are written to a temporary file and only renamed into place once
  they've finished.

See also: [v2 doc 09: Persistence and formats](../plans/v2/09-persistence-and-formats.md) ·
[Project file notes](../developer/notes/project-files.md) (technical)
