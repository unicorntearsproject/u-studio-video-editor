# Titles

[Docs home](../README.md) › [User guide](README.md) › Titles

Titles are animated text and shapes, such as lower thirds, name tags and
chapter cards. They play over your video with transparency. Each title is a
`.ustitle` file, and you use it on the timeline like any other clip.

> **Early days.** Titles come with the *Titles* add-on, installed next to
> the app ([Installing › Add-ons](installing.md#add-ons)).

## Putting a title on the timeline

The quickest way: press **Shift+T** (New Title). A new title appears at the
playhead on the active video track, five seconds long, and U-Stu Titles
opens on it with the template gallery. Pick a design, change the text, and
save; the editor updates straight away. The file goes in a **Titles** folder
next to your project (`Title 1.ustitle`, `Title 2.ustitle`, …). If the
project isn't saved yet, it goes in your default project folder (Settings ›
Locations); without one, in a folder named **U Stu Titles** in your Videos folder; and
without a Videos folder, in U-Stu's own data folder
(`~/.local/share/ustudio/U Stu Titles`).
Either way the clip keeps finding it after you save the project.

The **T** button in the header bar opens U-Stu Titles too: on the selected
title clip's title, or, with none selected, on a new title with the
template gallery (it isn't added to the timeline; save it and import it,
or use Shift+T instead).

To use a title file you already have:

1. Click **Import…** (or press **Ctrl+I**) and pick a `.ustitle` file. The
   Import dialog's **Titles** filter shows just those.
2. The title lands on the active track, after its last clip, at the length
   it was designed for.
3. Put it on a track **above** your video. Where the title is transparent,
   the picture below shows through.

## Designing a title

**U-Stu Titles** is the title designer (`u-studio-titles`). It opens a
`.ustitle` file, or starts a new one.

- **Add** (the **+** button) puts text, a rectangle, a rounded rectangle,
  an ellipse, a line or a picture (PNG) in the middle of the frame.
  **Ctrl+T** adds text; **Ctrl+Shift+R** a rounded rectangle.
- **Double-click text** to type on the canvas, in the title's own font.
  **Enter** keeps it, **Shift+Enter** starts a new line, **Escape** cancels.
- **Click** a layer to select it. **Drag** it to move it. It snaps to the
  frame's centre and edges, the safe areas, the thirds and the other
  layers; the line it snapped to shows while you drag. Hold **Alt** to move
  freely.
- **Drag a handle** to resize a shape, or to give text a box to fit.
- **Arrow keys** nudge the selected layer; **Delete** removes it.
- The dashed boxes are the **safe areas**: keep text inside the inner one
  and it won't be cut off on any screen. Toggle them in the main menu.
- The canvas's background is only for designing, and isn't saved in the
  title: a checkerboard (showing where the title is transparent), a colour
  of your choice, or a picture. Pick one in the main menu; the app
  remembers it.
- **Save** writes the `.ustitle` file. If the editor is using it, the
  editor updates straight away.

**Layers** (left) lists every layer, the topmost first. Click one to select
it; the eye hides it; the lock stops it being picked on the canvas (handy
for a background shape). Drag a layer up or down the list, or use the
arrows at the bottom, to change what's in front.

**The inspector** (right) styles the selected layer: its text, font
(family, weight, size, italic, letter spacing, line height, alignment, and
how it fits its box), fill (a colour, or a linear or radial gradient with an
optional middle colour), outline, shadow, and exact position, size,
rotation, scale and opacity. With nothing selected, it shows the title
itself: its background (none, for a transparent overlay; or a colour or
gradient), timing and canvas size.

**The brand kit** is next to every colour and font: the Unicorn Tears
colours, gradients and fonts, one click each. **Apply Brand** (with nothing
selected) restyles the whole title in the brand, keeping its layout.

## Templates

**New from Template…** in U-Stu Titles' main menu (**Ctrl+Shift+N**)
opens the gallery. The built-in templates come first, by kind: lower thirds,
bugs and badges, cards, end screens, countdowns, and live and social. Your
own are under **My Templates**. Click one to use it: in an empty title it
fills that title, otherwise it opens in a new window.

- **Save as Template…** (main menu) keeps the title you're working on in
  My Templates, pictures included, under a name you choose.
- Each template's **⋮** button: for yours, **Edit**, **Rename…**,
  **Duplicate** and **Delete…**; for a built-in, **Edit a Copy**, which
  puts a copy in My Templates and opens it. Built-ins never change.
- A title made from a template is a copy: changing the template later
  doesn't change titles already made from it on its own. When you open
  such a title in U-Stu Titles and its template has changed since (you
  edited your template, a pack was updated, or a new version of U-Stu
  changed a built-in), a bar says so. Click **Update** to take the
  template's new design. Your field text is kept, and a message names
  any field the template no longer has. Changes you made to the title's
  own design are replaced; **Ctrl+Z** takes the update back, and nothing
  is saved until you save. Titles made before version 0.72 don't know
  their template and are never offered an update.

Most templates have fields (a name, a role, a handle), so the same design
works for every guest: fill them in on the editor's **Title** page.

### Template packs

A pack is a `.zip` or `.tar.gz` of templates, to share a set with someone
or move it to another computer.

- **Open Pack…** (top of the gallery) shows what a pack holds (its title,
  author, licence, templates and fonts) before you install it. It then
  appears in the gallery as a section of its own. Opening a newer version
  replaces the old one; an older one, or the same one again, asks first.
- **Save as Pack…** makes one from My Templates: give it a title, author,
  version and licence, pick the templates, and choose where to save it
  (`.zip`, or a name ending in `.tar.gz`).
- A pack's templates are read-only: use **Edit a Copy** to change one.
  **Remove Pack…** next to a pack's name removes it; titles you made from
  it keep working, with its fonts and pictures beside them.
- **Sharing online** (coming): with **U-Stu Share** installed, the
  gallery has **Browse Shared…** (find packs others have shared, download
  and install one) and **Publish…** on each pack (it shows every file it
  would upload, and the licence, before you sign in and send it). U-Stu
  itself never goes online; U-Stu Share does, and only when you ask. The
  shared catalogue isn't online yet.
- U-Stu checks every pack before installing anything: a damaged or
  unsafe pack (files outside the pack, links, fonts it may not share,
  anything that isn't a picture, font or title) is refused, and the
  message says why. Nothing in a pack ever runs.

## Captions

Import a subtitle file (**Import…**, pick an `.srt` or `.vtt`) and each of
its lines becomes a caption on the timeline, at its time, on a new
**Captions** track at the top. Captions that overlap go on **Captions 2**.
The status bar says how many were imported, and which lines of the file
couldn't be read and were left out.

- All the captions play one caption title, `<file name>
  captions.ustitle`, made next to your project from **Caption, plain**.
  Edit it (double-click a caption, or Ctrl+Shift+T) to restyle every
  caption at once; the gallery's Captions section has boxed and top
  styles too.
- Each caption's words are its own: fix one on the **Title** page. The
  timeline names each caption by its first line, and the name follows as
  you fix the words; once you rename a caption clip yourself, it keeps
  your name.
- Line breaks are kept. **Bold**, *italic* and underline from the file
  (`<b>`, `<i>`, `<u>`) show as such; a WebVTT speaker (`<v Name>`) shows
  above the words.
- Colours come through when the file uses one of the eight every player
  knows: white, lime, cyan, red, yellow, magenta, blue and black. Other
  colours are left out and the words stay.
- A caption the file puts at the top of the picture (a WebVTT `line:`
  near the top, or an `.srt` line starting `{\an8}`) goes at the top,
  styled by a second caption title, `<file name> captions (top).ustitle`.
  Other positions and sizes in the file are ignored.
- One **Ctrl+Z** takes the whole import back.
- Files in UTF-8 or UTF-16 read as they are; others are read as Windows
  text (Western European), and the status bar says so. A file with no
  captions, one that isn't text, or one over 10 MB or 20,000 captions is
  refused, and nothing is added.

### Exporting captions

Select a caption and click **Export Captions…** on the **Title** page, then
name the file. A name ending in `.vtt` makes a WebVTT file (for web players
and YouTube); anything else makes an `.srt` file, the most widely read kind.
Every caption in the project goes in, in time order, whichever track it's
on, with its words as they are now: fix the words on the timeline, export,
and the file has your fixes. Bold, italic, underline, colours, speakers and
top placement are written out, and importing the file again gives the same captions at the
same frames.

## Live text: clocks, countdowns and dates

Type one of these into a text layer and it changes as the video plays:

| Type | Shows |
|---|---|
| `{{timecode}}` | Where the frame is in the whole video, as hours:minutes:seconds:frames |
| `{{clip_time}}` | How long the title has been up, as minutes:seconds |
| `{{countdown:05:00}}` | Counts down from five minutes and stops at zero. Write the start as `ss`, `mm:ss` or `hh:mm:ss`, and it counts in the same shape |
| `{{date}}` | Today's date, as 2026-09-27 |
| `{{date:%d %B %Y}}` | Today's date in your own format (`%d` day, `%m` month number, `%B` month name, `%Y` year, `%A` weekday) |

In U-Stu Titles, the title is its own clip, so `{{timecode}}` and
`{{clip_time}}` count from the start of the title. The date is the day the
video is played or exported.

## Animating a title

Under the canvas is the **animation strip**: the title's **Intro**, **Hold**
and **Outro** on a ruler, and a row for each layer.

- **Click or drag** on the strip to move the playhead; the canvas shows
  that moment. Click a layer's row to select it.
- **Drag the dividers** on the ruler to make the intro, the hold or the
  outro longer or shorter.
- Press **Space** (or the play button) to watch the title: the intro, two
  seconds of the hold and the outro, over and over.

**Behaviours** are ready-made animations. Select a layer, then in the
inspector's **Animation** section use **Add In…**, **Add Out…** or
**Add Loop…**. Each thumbnail shows the behaviour on your own layer; click
one to add it.

| Slot | Behaviours |
|---|---|
| In (as the title comes in) | Fade, Rise, Drop, Pop, Typewriter, Word by word, Blur, Wipe, Scramble, Split lines, Kinetic stack |
| Out (as it goes) | The same, plus Collapse |
| Loop (through the hold) | Float, Pulse, Shimmer, Wiggle, Glow breathe |

A layer has one behaviour for each slot; adding another replaces it. Set
its length in frames next to it (a loop's is how often it repeats), or
remove it. On the strip, In is pink, Out cyan and Loop violet.

**Keyframes** animate anything else. Put the playhead where you want a
change, then click **◆** next to Position, Rotation, Scale, Opacity or Blur
in the inspector. Once a value has keyframes, changing it changes it at the
playhead (adding a keyframe there). Keyframes show as diamonds on the
strip. **Detach to Keyframes** turns a layer's in and out behaviours into
keyframes you can adjust one by one.

Keyframes set in the outro stay with the outro, so making the hold longer
never moves the way a title leaves.

### Animated layers

An **animated layer** plays a ready-made animation (a logo sting, an
animated sticker or icon) inside the title, as sharp at 4K as at 720p. In
U-Stu Titles, open **+** and choose **Animation…**, then pick a **Lottie**
file (`.json`), the kind LottieFiles, After Effects (with Bodymovin) and
many design tools export.

- It's added at its own size, centred; move and resize it like a picture.
  It always keeps its own shape inside the layer's box.
- In the inspector, **Plays** is **Over and over** (from the start again
  at its end) or **Once, then holds** (it stops on its last frame), and
  **Speed** makes it faster or slower (25% to 400%).
- It plays from the start of the title, and the canvas, the editor and
  your exports all show the same frame at the same moment.
- Opacity, position, rotation, scale, behaviours, blur and shadow work as
  on any layer.
- The file is kept with the title like a picture: it travels with a title
  saved elsewhere, made into a template, or shared in a template pack.

Two templates start you off: **Lower third with ringing bell** and
**Subscribe with ringing bell** (under Bugs and badges), with a bell that
rings every few seconds.

Some files can't be used, and U-Stu says why when you add one:

- files that use **expressions** (small scripts); export them again with
  expressions baked into keyframes (Bodymovin has an option for it);
- files that load a picture or font from outside themselves (pictures
  must be embedded, as PNG or JPEG);
- files over 8 MB, over ten minutes long, or too complex to be safe.

Text inside an animation is drawn in your system's fonts, so it may look
a little different from the file's own.

## Exporting a title on its own

To use a title outside U-Stu (in OBS, say), use **Export…** in the
designer's main menu (**Ctrl+E**). Pick a format and a length. The hold
stretches to the length, as it does on the timeline.

| Format | Background | Good for |
|---|---|---|
| ProRes 4444 (`.mov`) | Transparent | OBS, and most video apps; big files, best quality |
| WebM VP9 (`.webm`) | Transparent | OBS and browsers; small files, slower to export |
| QuickTime Animation (`.mov`) | Transparent | Apps that don't read ProRes; lossless, big files |
| PNG sequence (a folder) | Transparent | Any app; one picture per frame |
| H.264 (`.mp4`) | The title's own background, or black | Anywhere a transparent video isn't needed |

The title is saved first if it has changes. In OBS, add a **Media
Source** with the exported file; the transparent areas show what's under
it.

From the editor, select a title clip and use **Export on its own** at the
bottom of the inspector's **Title** page: pick a format and click
**Export…**. You get that clip's title with its own fields, as long as the
clip, at the project's frame rate. The project doesn't change.

## From the editor

**Double-click a title clip** on the timeline (or select it and press
**Ctrl+Shift+T**, Edit Title) to open it in U-Stu Titles. The
designer shows the video at the playhead behind the title, without the title
itself, so you design over the real picture. Save there, and the editor
picks up the change.

**Fields** make one title serve many clips: a lower third with `{{name}}`
and `{{role}}` in its text works for every guest. Select a title clip and
open the inspector (the button at the top right of the preview), then the
**Title** page. It has a box for each field. What you type there changes
only that clip, and the preview updates as you type. A box left at the
title's own text (its default) follows the title if you change it later.
**Ctrl+Z** undoes each box's edit in one step.

**Bake** turns a title clip into an ordinary video file, for sharing the
project with programs that can't play titles (stock `melt`, other editors).
Select the clip and click **Bake…** on the Title page. The clip is rendered
with its own fields and transparency to a ProRes 4444 file next to the
title (`Lower Third (baked).mov`), and the clip then plays that file. It
keeps its place, length, effects and position on screen. **Ctrl+Z** brings
the live title back. A baked clip is a picture of the title as it was:
changing the title file, its fields or the date doesn't change it (bake
again for that). For now, a baked title placed over video without moving or
scaling it can show a thin dark edge that the live title doesn't; a fix is
coming.

## Any length you like

Every title has three parts: an **intro** (it animates in), a **hold** (it
stays up) and an **outro** (it animates out). When you make a title clip
longer or shorter, only the hold changes. The intro and outro keep their
timing. So one lower third works for a two-second mention and for a
two-minute segment.

A clip shorter than the intro and outro together plays both, faster, with
no hold.

## Editing a title

A title clip plays whatever the file says now. Save a change to the
`.ustitle` file (in the titles app, or any text editor) and the preview
updates within a second. You don't need to re-import it.

## Fonts

Titles use the fonts installed on your computer. If a title asks for a font
you don't have, it's drawn in a similar font instead, and the log names both
(for example "Space Grotesk isn't installed; using Noto Sans"). To bring
fonts with a title, put them in a `fonts` folder next to the `.ustitle`
file.

## When something's wrong

- **The clip shows as missing.** The `.ustitle` file was moved, deleted or
  can't be read. Put the file back where it was, or fix it, and it plays
  again. **Relink** doesn't work for titles yet.
- **"… isn't supported yet" after importing.** The title uses something
  this version can't draw yet, such as text animators, which arrive with the
  animation tools. The rest of the title still plays.
- **Rendering a project with titles in another program.** Other programs
  (including stock `melt`) can't play `.ustitle` files. Render from U-Stu,
  or **Bake** the title clips first.
- **"Couldn't add the animation: …".** The message says why; see
  [Animated layers](#animated-layers) for what can't be used. For a file
  with expressions, export it again with them baked into keyframes.
- **An animated layer shows nothing.** The animation file was moved or
  deleted (the message names it), or this build of U-Stu has no animation
  support; builds from source need ThorVG (see the developer docs).
- **"No captions to export".** Only captions from an imported subtitle file
  are exported; import a `.srt` or `.vtt` file first.
- **"Couldn't bake …".** The message says why: usually the folder next to
  the title can't be written to. Move the title somewhere you can write to
  and try again.
- **"… the clip changed meanwhile, so it's in the bin only."** The clip was
  edited while it was baking, so it wasn't swapped. The baked file is in the
  media browser; bake again to swap it in.
