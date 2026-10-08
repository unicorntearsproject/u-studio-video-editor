# 16 — Titles: a separate text design and animation tool

**Status:** proposal, 2026-09-23. Owner request the same day: an easy,
robust text editing and animation tool, separate from the editor, tackled
as soon as possible. Effects and transitions are planned in
[doc 15](15-effects-and-transitions.md); this doc shares its easing model
and its rule that only reusable, mature libraries do the heavy lifting.

## Why a separate tool, and why not an existing MLT text service

| Option | Verdict |
|---|---|
| `kdenlivetitle`, `qtext` producers | Rejected: Qt (ADR-007). |
| `glaxnimate` producer | Rejected: Qt. |
| MLT `text` / `dynamictext` filters, `pango` producer | Present and Qt-free, but static styling (one font, outline, background box) and no per-character animation. Good enough for burn-in timecode, not for a title designer. |
| `avfilter.drawtext` | Present; per-frame expressions exist but styling is limited and authoring it is not something to expose to users. |
| Our own renderer with Pango + Cairo, exposed as an MLT producer | **Chosen** ([ADR-012](adr/012-titles-mlt-module.md)). Pango does shaping, bidi, font fallback and line breaking; Cairo does paths, strokes and gradients. Both are mature and already in the process via GTK. We write layout of layers, animation evaluation and a thin producer. |

A separate application (not a dialog inside the editor) because:

- title design is a different activity with its own canvas, inspector and
  timeline, and would crowd the editor's window;
- a separate process isolates the editor from font and text-shaping
  crashes and lets the tool be used on its own for thumbnails and stills;
- it can be launched from the editor on a specific file and report back
  over GApplication's D-Bus actions, with no new dependency.

Working name: **u Studio Titles**, executable `u-studio-titles`, app id
`com.ustudio.Titles`.

> REVIEW (VE Text, 2026-09-28): owner decision: U-Stu Titles stays
> in-app only for now. It's reached from the editor (the header's T,
> New Title, Edit Title), with no separate app ID in the packages and no
> menu entry of its own; the Flatpak ships it inside the Titles
> extension, which can't export a desktop entry anyway. The drafts in
> `drop-ins/titles/data/` (desktop file, MIME type, metainfo) stay for a
> later standalone release.

## Architecture

All of it lives in `drop-ins/titles/` (see "Drop-in structure" below):

```
core/        TitleDocument model, .ustitle XML reader/writer, animation
             evaluator. std + libxml2 only (doc 02's core rules).
render/      libustudio-titlerender: draws a TitleDocument at time t into an
             RGBA buffer. Pango, PangoCairo, Cairo, fontconfig. No GTK, no MLT.
mltmodule/   libmltustudio.so: MLT module with one producer, `ustudio_title`,
             wrapping render/. MLT framework C API only. Linked into the
             curated module directory by FactoryPolicy (IP4).
app/         u-studio-titles: GTK4 + libadwaita app. Uses core/ and render/
             directly for its canvas. No MLT.
editor/      The editor side: treats a .ustitle file as an asset played by
             the `ustudio_title` producer.
```

One renderer, three consumers: the titles app's canvas, the editor's
preview (through the MLT producer) and export (the render CLI loads the same
module). What you see while designing is byte-for-byte what exports.

Dependencies: Pango, PangoCairo, Cairo and fontconfig are already loaded
via GTK4, but linking them into non-GTK layers is new, so ADR-012 records
it.

## Drop-in structure

Titles follow the same drop-in rules as effects
([ADR-013](adr/013-effects-and-titles-as-drop-in-modules.md); the
integration points IP1–IP6 are defined in
[doc 15](15-effects-and-transitions.md), "Drop-in structure"). The titles
*app* is already separate by design; this section is about the editor side.

```
drop-ins/titles/
  meson.build  README.md  register.{h,cpp}
  core/        TitleDocument, .ustitle XML, evaluator
  render/      titlerender (Pango/Cairo)
  mltmodule/   libmltustudio.so, `ustudio_title`
  app/         u-studio-titles executable
  editor/      .ustitle import handler, file watch, fields page,
               New Title / Bake title actions
  data/        templates/, brand.json
  tests/       core/, render/, engine/, editor/
```

The whole folder builds only when `dropin_titles` isn't `disabled` (ADR-014), and follows the same
self-containment rules as effects (dependencies point from `drop-ins/`
into `src/`, never back; deleting the folder removes the feature).

| IP | Titles use |
|---|---|
| IP1 | `Clip::sourceParams` (per-clip field values); a title asset is an ordinary `Asset` whose path ends in `.ustitle` |
| IP2 | `ustudio:field.*` on clip entries; the `ustudio_title` producer written like any other resource |
| IP3 | `makeProducer()` returns a per-clip `ustudio_title` producer with `length` and `field.*` set |
| IP4 | `libmltustudio.so` linked into the curated module directory |
| IP5 | import handler for `.ustitle`; inspector page with the clip's fields; action contributions (New Title, Bake title) |
| IP6 | none |

Gating: with `dropin_titles=disabled`, or if the module fails to load, a title clip
plays as the black missing-media placeholder with a status notice, and its
data (asset, fields, timing) still round-trips unchanged.

Sequencing matches effects: `core/`, `render/`, `mltmodule/` and `app/`
are all new files inside `drop-ins/titles/` and can be built and tested now
(T0–T3 need no integration point). Editor-side integration (T1's editor
import, T4) waits for the integration points that land after the post-M3
audit.

## The document: `.ustitle`

XML via libxml2, like projects (ADR-004), versioned from 1. A title is a
small canvas with layers and a timeline split into three zones:

```xml
<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15" hold-mode="elastic"/>
  <field name="name" label="Name" default="Jay Doe"/>
  <field name="role" label="Role" default="Host"/>
  <layer id="bar" kind="shape" shape="rounded-rect" x="96" y="820" w="620" h="140" radius="12">
    <fill color="#1b1230" opacity="0.92"/>
    <behavior slot="in" id="wipe-right" duration="12" easing="cubic_out"/>
  </layer>
  <layer id="name" kind="text" x="128" y="838" w="560" fit="shrink">
    <text>{{name}}</text>
    <font family="Space Grotesk" weight="700" size="64" tracking="0.02"/>
    <fill gradient="linear" from="#ff3cc7" to="#19e3ff" angle="0"/>
    <shadow dx="0" dy="4" blur="12" color="#000000" opacity="0.5"/>
    <animator unit="character" order="forward" stagger="1">
      <key at="0" opacity="0" dy="24" easing="back_out"/>
      <key at="10" opacity="1" dy="0"/>
    </animator>
  </layer>
</ustitle>
```

### Elastic timing

Every title has an **intro**, a **hold** and an **outro**. On the editor's
timeline the clip can be any length: intro and outro keep their designed
timing and the hold stretches or shrinks to fit. Loop behaviours (pulse,
shimmer, float) repeat through the hold. This makes one lower third usable
for a two-second mention and a two-minute segment without re-animating
anything, and it is the main thing that makes the tool feel easy.

### Fields and dynamic text

- **Fields** (`{{name}}`, `{{role}}`) turn a title into a template. Each
  clip on the timeline stores its own field values, so one lower-third file
  serves every guest in a stream.
- **Dynamic fields** are evaluated per frame: `{{timecode}}`,
  `{{clip_time}}`, `{{countdown:mm:ss}}`, `{{date:%Y-%m-%d}}`.

### Layers and styling (v1)

| Layer | Features |
|---|---|
| Text | Pango attributes per run (family, weight, style, size, colour), tracking, line height, alignment, box with wrap or shrink-to-fit, fill (solid, linear or radial gradient), stroke, shadow, glow, background box with padding and corner radius |
| Shape | rectangle, rounded rectangle, ellipse, line; fill, stroke, gradient |
| Image | PNG (Cairo loads it natively); SVG later, since librsvg is another dependency |
| Group | transform and animate several layers together |

Cairo has no blur, so shadow and glow use a small separable box blur
(three passes approximate a Gaussian). It is the only image-processing code
the titles tool writes, and it is about forty lines.

## Animation model

- **Keyframes** on every layer property (position, scale, rotation,
  opacity, blur, tracking, colour), with the same `Easing` set doc 15 adds
  to the core model, so a "feel chip" means the same thing in both tools.
- **Text animators**, the After Effects idea that makes kinetic type
  practical: a range of units (character, word or line), an order (forward,
  reverse, centre-out, random with a stored seed), a stagger in frames, and
  a small keyframed curve of offsets (`dx`, `dy`, scale, rotation, opacity,
  blur, colour) applied to each unit on its own clock.
- **Behaviours** are named presets that generate keyframes and animators.
  They are what most users touch; "Detach to keyframes" converts one into
  plain keyframes for hand editing.

| Slot | Behaviours shipped in T3 |
|---|---|
| In | Fade, Rise, Drop, Pop (back easing), Typewriter (with cursor), Word by word, Blur in, Wipe (mask), Scramble (random glyphs resolving to the text, seeded), Split lines, Kinetic stack |
| Out | Mirrors of the In set, plus Collapse |
| Loop (hold) | Float, Pulse, Shimmer (gradient sweep), Wiggle (seeded noise), Glow breathe |

The evaluator lives in `drop-ins/titles/core/` as a pure function of the document,
time, field values and the unit count, so it is unit-tested without Pango
or a display. The renderer asks Pango for cluster positions
(`PangoLayoutIter`) and draws each unit with its own Cairo transform.

## Rendering and the MLT producer

- `ustudio_title` takes `resource` (the `.ustitle` path) and properties
  `field.<name>` for per-clip field values, and renders RGBA with alpha at
  the consumer's requested size.
- **Per-clip producers.** Today EngineSync shares one master producer per
  asset. Title clips differ per clip (field values, stretched hold), so each
  title clip gets its own producer with `length` set to the clip's length;
  the renderer maps clip frame to title time through the elastic timing.
- **Caching.** Layouts are rebuilt only when text, fields or fonts change.
  Static layers are cached as surfaces. During a hold with no loop
  behaviour, consecutive frames are identical and the previous buffer is
  returned without drawing, which is most frames of a long lower third.
- **Threading.** The producer runs on the consumer thread. Pango font maps
  and contexts must not be shared across threads, so the producer creates
  its own `PangoCairoFontMap`. T0 verifies this under the render CLI's
  threading as well.
- **Alpha.** Cairo's ARGB32 is premultiplied BGRA in native byte order; MLT's
  `rgba` is straight RGBA. The conversion is one loop, verified in T0 with
  a half-transparent test pixel.
- **Fonts.** fontconfig finds system fonts. A project may carry a `fonts/`
  folder next to the project file, registered with
  `FcConfigAppFontAddDir` in both the titles app and the producer. A missing
  font shows a warning in both tools and names the substituted family; it
  never fails silently. The Flatpak bundles the brand fonts (doc 12, M7).
- **Portability.** Stock `melt` does not know `ustudio_title`. "Bake title"
  in the editor renders a title clip to a QuickTime Animation or ProRes 4444
  file with alpha, or a PNG sequence, and swaps the clip's asset in one
  undoable command. The render CLI always loads our module, so exports never
  need baking.

## The titles app UX

Layout:

- **Canvas** in the centre, showing a **backdrop**: the editor's frame at
  the playhead when launched from the editor (passed as a PNG, so the titles
  app never needs MLT), otherwise a neutral checkerboard. Title-safe and
  action-safe guides, thirds, and snapping to guides, the canvas centre and
  other layers.
- **Layers** list on the left; drag to reorder; eye and lock toggles.
- **Inspector** on the right: style for the selected layer, with brand kit
  swatches and fonts at the top.
- **Animation strip** along the bottom: the three zones (intro, hold,
  outro) with draggable dividers, one bar per layer, behaviour chips on the
  bars. Expanding a bar shows its keyframes.

Interactions that make it easy:

1. **Type on the canvas.** Double-click text to edit it in place: an entry
   overlay sits exactly over the layer in the same font while typing; the
   canvas re-renders on commit.
2. **Drag a behaviour onto a layer.** The behaviour drawer shows each
   behaviour animating *your* text (rendered on a worker), not a generic
   sample.
3. **Loop preview.** Space plays intro, two seconds of hold, then outro, on
   repeat, so timing is judged in context without scrubbing.
4. **Brand kit.** Colours and fonts from the Unicorn Tears design tokens
   (copied into `drop-ins/titles/data/brand.json`, values only) are one click away;
   "Apply brand" restyles a whole title.
5. **Templates first.** New Title opens a gallery: lower third (one and two
   lines), name tag, "Live now" bug, chapter card, end card with call to
   action, countdown, quote card, social handle. Each is an ordinary
   `.ustitle` with fields.
6. **Shortcuts scoped to the canvas**, not application accelerators, so
   typing in any field never triggers them (audit A1, 2026-09-23).

## Editor integration

- **New Title** (header bar and `Shift+T`) opens the template gallery in the
  titles app. Saving there sends the file back through the editor's
  exported GApplication action `app.insert-title`, and the editor inserts it
  at the playhead on the active track.
- **Double-click a title clip** to open it in the titles app. The editor
  watches the file (`GFileMonitor`) and rebuilds just that asset's producers
  when it changes, reusing the missing-media reset path.
- **Fields in the editor.** Selecting a title clip shows its fields in the
  editor's Rack (doc 15), so changing a guest's name never requires opening
  the titles app. Field values are stored per clip in the model
  (`Clip::sourceParams`, a string map written as `ustudio:field.*`).
- Title clips are drawn with their own colour and a text snippet on the
  timeline, and are boundless (like stills), with the elastic hold making any
  length valid.
- Effects from doc 15 apply to title clips like any other clip, so glow,
  glitch or grain on a title is free.

## Phases

The titles track runs alongside the M3 wrap-up, M4 and the FX track.
Everything up to T3 is module-internal; only the editor-side parts need
integration points (listed per phase).

### T0 — Spikes (about 3–5 days)

Integration points: none.


- A custom MLT module loaded from the curated directory, with YAML metadata
  that `Mlt::Repository::metadata()` returns.
- Pango rendering on the consumer thread with a private font map;
  4K frame time for a two-layer lower third; no leaks across 10k frames.
- Premultiplied-to-straight alpha into MLT `rgba`.
- `ustudio_title` round-trip through MLT's `xml` producer and the render CLI.
- `FcConfigAppFontAddDir` visible to Pango in both processes.
- GApplication action invoked from a second process.

Acceptance: each item has a recorded finding and a kept repro.

> REVIEW: VE Core, 2026-09-27: T0 spikes run, all yes; findings in
> [the titles notes](../../developer/notes/titles.md). A 4K two-layer lower
> third costs 12.7 ms a frame on a worker thread with its own font map, no
> leak over 10,000 frames. The render-CLI half of the `xml` item waits for
> T1's drop-in packaging.

### T1 — Format, renderer, producer (about 2 weeks)

Integration points: IP1, IP2, IP3 `makeProducer()`, IP4, IP5 import
handler. The format, renderer and producer can be built and tested before
those land; only "the editor imports a `.ustitle`" waits for them.


`drop-ins/titles/core` model, XML reader/writer, evaluator (keyframes only, no
animators yet); `titlerender` with text, shapes, fills, stroke, shadow;
`libmltustudio` producer; the editor imports a `.ustitle` as an asset,
with per-clip producers, elastic timing and file-watch reload.

Acceptance:

- [x] A hand-written `.ustitle` plays in the editor and renders through
      `u-studio-render` with identical frame hashes (`titles-engine`: the
      render tool's `--title-frames` against the editor's graph, built in
      and as a module; seen in the editor on 2026-09-27).
- [x] Stretching the clip changes only the hold (`titles-engine`: intro and
      outro frames hash equal on a 93- and a 400-frame clip).
- [x] Editing the file on disk updates the editor within a second
      (`titles-shell`; 0.31 s from save to rebuilt graph in the editor).

> REVIEW: VE Text, 2026-09-27: T1 as built. (1) The producer is made
> through MLT's `loader` (`ustudio_title:<path>`), not the factory: only
> the loader attaches the normalising filters, and without them the
> compositor read our RGBA as YUV (`docs/developer/notes/titles.md`).
> (2) A title clip's producer spans source frames 0..out, so the right half
> of a split still ends in the outro; the left half fits the whole title
> into its length. (3) Title assets are boundless through
> `MediaInfo::isStillImage` (as stills are): `InsertClip` otherwise grows a
> boundless asset's length to its clips, and it also keeps titles out of
> proxies and profile matching. The media browser shows them with the still
> badge until T4 gives title clips their own look. (4) The file watch
> reports through a new IP5 method, `ShellHost::assetChangedOnDisk()`
> (doc 15), not a command, so a save isn't an undo step. (5) Relink can't
> probe a `.ustitle` yet (it probes through MLT's loader without the
> service prefix); a missing title plays again when its file comes back.
> (6) `.ustitle` keyframes carry a `zone` (intro, hold, outro) so outro
> keys stay with the outro when the hold changes.

### T2 — The titles app (about 2–3 weeks)

Integration points: none (separate executable).


Canvas with backdrop, guides and snapping; layers; inspector; on-canvas
typing; shapes and images; brand kit; save and open; launch from the
editor on a file.

**Backgrounds and export** (owner requirement, 2026-09-27: "set the
background to any colour we like, and titles either include the background
or drop it for a proper alpha channel"; they go into OBS as overlays):

- **The canvas's background** while designing: a checkerboard, **any
  colour** (a colour picker), or a picture (`--backdrop`, the editor's
  frame). A view setting remembered per user, never part of the title.
- **A title's own background** (`<background>`, the document's
  `background`): none by default, so the title has a real alpha channel
  over the video; or a colour or gradient baked in (an image with T2c's
  image layers). Same format version: `<background>` is optional.
- **Export a title on its own** (T2d) from the designer, and **Export
  Title…** in the editor if it's cheap: with alpha, as a PNG sequence and
  at least one alpha video codec (ProRes 4444 or QuickTime Animation, and
  WebM VP9 with alpha); or flattened onto its background as H.264. Codec
  and pixel-format names checked against the installed avformat, and the
  alpha proved with `ffprobe` (a pix_fmt with an alpha plane).

Phases: T2a (document editing and undo, the canvas with backdrops, guides,
snapping, move/resize, save and open, the title background), T2b (layers
list, inspector, brand kit), T2c (on-canvas typing, images, launch from the
editor), T2d (export).

Acceptance:

- [x] A lower third can be designed from a blank canvas without touching
      the XML (`tools/titles-smoke/run.sh`, 2026-09-27).
- [x] Nothing typed into any text field triggers a shortcut (only modifier
      shortcuts are window-wide; checked by typing Delete, arrows, Escape
      and letters into the inspector's text and the canvas text box).
- [x] The canvas background can be any colour, and the choice is
      remembered.
- [x] A title with no background composites over coloured video with clean
      anti-aliased edges (no dark fringes); one with a background covers the
      frame. Fixed in the engine by VE Core (9b3ddd2: composite's 4:2:2
      blend gave each pixel of a pair its own alpha); `titles-engine`'s
      fringe case now must pass.
- [x] A title exports as a PNG sequence and as an alpha video that `ffprobe`
      reports with an alpha plane, and flattened as H.264 (2026-09-27:
      ProRes 4444 `yuva444p12le`, QuickTime Animation `argb`, VP9
      `alpha_mode=1`, PNG `rgba`; decoded back, a 92% bar reads 0.92 and an
      empty corner 0; the smoke test exports through the dialog).

> REVIEW: VE Text, 2026-09-27: T2a as built. (1) Undo keeps whole
> documents (`TitleHistory`), not inverse commands: a title is a few
> kilobytes, and a drag merges into one step by key. (2) The canvas
> renders on one worker thread, newest request wins, at the canvas's size
> on screen; the texture goes to GTK as is (Cairo's ARGB32 is
> `GDK_MEMORY_DEFAULT`). (3) Only modifier shortcuts are window accels;
> Delete, arrows and Escape are the canvas's own key controller, so no
> text field can lose a key to them. (4) A press becomes a drag after 3 px,
> so a click never edits. (5) While designing, the canvas shows the first
> frame of the hold; T3 adds scrubbing. (6) Each launch is its own process
> (`G_APPLICATION_NON_UNIQUE`), one window per file.

> REVIEW: VE Text, 2026-09-27: T2b as built. (1) The brand kit is
> `drop-ins/titles/data/brand.xml`, not `brand.json`: the drop-in has
> libxml2 and no JSON parser, and the kit is compiled into the app's
> GResource. (2) Fills gained an optional middle stop (`via`), for the
> brand's three-colour "tears" gradient; layers gained `locked`. Same
> format version: both are optional attributes. (3) Apply Brand keeps the
> layout: the largest text gets the first gradient (and the display font
> from 72 px), other text the sans font in pink-white, filled shapes ink-700
> at 92%, outlines cyan. (4) The inspector rebuilds only on a selection
> change or an edit made elsewhere; an edit made in it doesn't, so a field
> keeps focus while you type, and rows that depend on a choice (a fill's
> kind, the shadow switch) are rebuilt from an idle callback.

> REVIEW: VE Text, 2026-09-27: T2c as built. (1) Typing on the canvas is
> a text view overlaid on the layer in its family, weight and size at the
> canvas's scale, with the layer hidden meanwhile; not a GtkEntry (text can
> have several lines). (2) Image layers are PNG only (Cairo reads PNG
> without another library), relative to the title's folder, cached per
> thread and re-read when the file changes; Save As to another folder
> rewrites relative paths. (3) Edit Title renders the backdrop itself: the
> frame at the playhead with the title's video off, on a worker thread with
> its own EngineSync (as an export does), joined at shutdown; the
> `ustudio_title` producer honours `video_index=-1` for that. (4) The
> designer is found next to the editor (one bundle in the Flatpak), else
> the build tree, else PATH, and started with GSubprocess.

> REVIEW: VE Text, 2026-09-27: T2d as built. (1) Export is
> `u-studio-render --title-export` (IP6), run by the designer in a child
> process: the designer stays MLT-free. (2) Alpha formats take the
> `ustudio_title` producer's frames straight to the encoder with no
> compositing, so the alpha is the renderer's own; H.264 is flattened by the
> producer drawing the background itself (its `background` property), not
> by a compositor. (3) Formats: ProRes 4444 (`prores_ks`, `vprofile=4`,
> `yuva444p10le`), VP9 (`yuva420p`, `auto-alt-ref=0`), QuickTime Animation
> (`argb`), PNG (`rgba`), H.264 (`libx264`, `yuv420p`, CRF 18). (4) Export
> Title… in the editor isn't done: it's the same subcommand, so it's cheap,
> but it waits for T4's clip workflow.

### T3 — Animation (about 2 weeks)

Integration points: none.


Keyframes with the shared easing set, text animators, the behaviour
library, the animation strip with elastic zones, loop preview, animated
behaviour thumbnails.

Acceptance:

- [x] Every shipped behaviour renders identically in the titles app, the
      editor and export (T3a, 2026-09-27: one renderer; `titles-engine`
      checks the producer's frames equal `renderTitle`'s byte for byte;
      `titles-render` renders every behaviour in every slot).
- [x] Evaluator unit tests cover every easing and every animator order
      (`titles-animation`).

> REVIEW: VE Text, 2026-09-27: T3a as built (core and renderer; the
> designer's animation strip is T3b). Behaviours are stored as
> `<behavior slot id duration easing seed amount>` and expanded when drawn
> into offsets, animators and loops (`core/animation.h`); Detach to
> keyframes converts in and out behaviours, while loops, the scramble and
> the typewriter's cursor stay behaviours. Animatable properties grew:
> blur, tracking, reveal (the wipe), shift (the shimmer), fill colour per
> channel, shadow opacity. Animators take a `spread` as well as a stagger,
> so a typewriter fits its duration whatever the text's length.

> REVIEW: VE Text, 2026-09-27: T3b as built. The animation strip draws the
> zones, a row per layer with behaviour chips and keyframe diamonds, and
> the playhead; dragging a divider retimes the title (one undo step). Space
> plays the loop preview from GTK's frame clock. The inspector keys a
> property at the playhead (◆) and edits an animated property's value
> there; keys land in the zone the playhead is in. The behaviour drawer is
> a popover per slot whose thumbnails animate the selected layer itself,
> cropped to it, rendered on a worker thread and cycled on the main loop.
> One behaviour per slot per layer: adding one replaces the slot's.

### T4 — Templates and editor workflow (about 1–2 weeks)

Integration points: IP5 inspector page and action contributions.


Fields and dynamic fields, the template gallery, fields in the editor's
Rack, New Title and insert-at-playhead over D-Bus, Bake title.

> REVIEW (VE Text, 2026-09-27): T4 runs as T4.1 (editor workflow) then
> T4.2 (templates), so the names don't collide with doc 20's T4b. T4.1 is
> done in 0.59.1–0.65.0: the editor's text-entry shortcut guard, dynamic
> fields, the Title inspector page (fields per clip), Bake Title, and
> Export Title from the editor. T4.2 is done in 0.66.0: the template
> library and gallery, 29 built-ins, and New Title, which makes the file
> first and opens the designer with its gallery (approved deviation from
> "save sends it back"). "Update from template" is left for later: a
> title doesn't record its template, and merging a changed design into
> edited text isn't cheap.

**User templates and a bigger gallery** (owner request, 2026-09-27):

- **Save as Template** from any title, fields included, into a user
  template library under the app's data directory. Paths go through
  `src/platform/` (ADR-017).
- The gallery has **Built-in** and **My Templates** sections. User
  templates can be edited, renamed, duplicated and deleted. Built-ins are
  read-only: editing one saves a copy to My Templates.
- **New Title** from any template. Title clips are copies, so changing a
  template never rewrites existing clips silently. **Update from template**
  on a clip, if it's cheap.
- **About 20 built-ins** beyond the eight above, across lower thirds,
  bugs and badges, cards, end screens, countdowns and social. They use the
  brand kit (`brand.json`) with generic font fallbacks. Each is an ordinary
  `.ustitle` with fields, and none uses raster art unless it's generated.

Acceptance:

- [x] Ten lower thirds with different names come from one template file.
      (Per-clip field values from the Title inspector page, 0.62.0;
      tests `titles-engine` "field values are the clip's own" and
      `titles-shell` "the Title page edits the selected clip's fields".)
- [x] A baked title plays in stock `melt`. (Bake Title, 0.64.0; test
      `titles-engine` "a bake: ... stock melt plays it with its alpha".
      Over video, a baked file needs VE Core's `MediaInfo::hasAlpha`
      pairing to avoid the 4:2:2 fringe; until then it's a known limit.)
- [x] A user template survives an app restart and appears in New Title.
      (My Templates are folders on disk, listed afresh each time the
      gallery opens; `titles-core` "template library", live on Xvfb.)
- [x] Editing a built-in leaves it unchanged and adds a copy to My
      Templates. (Edit a Copy; `titles-core` and live on Xvfb.)
- [x] Every built-in renders identically in the titles app, the editor's
      preview and export, and passes the render tests. (`titles-render`
      and `titles-engine`: 29 built-ins, the producer's frames byte for
      byte the renderer's.)

### T4b — Template packages, and T7 — sharing

Owner request, 2026-09-27: save and open template packs as `.zip` or
`.tar.gz`, and publish and download them through a shared catalogue.
Planned in [doc 20](20-template-packages-and-sharing.md) and
[ADR-020](adr/020-template-packages-and-sharing.md); the service is
[doc 21](21-template-sharing-backend.md). T4b (local packages) follows
T4; T7 (the `u-studio-share` helper) follows T4b.

### T5 — Captions (planned 2026-09-28, VE Text)

Import a subtitle file (`.srt`, `.vtt`) as title clips: one clip per cue,
all playing **one caption title** made for the import from a caption
template, the cue's text in the clip's `{{caption}}` field. Restyling that
one title (Edit Title) restyles every caption; fixing a cue's words is the
Title page's field, like any lower third.

**Import.** Import… (or a drop) takes `.srt` and `.vtt` through the titles
drop-in's import handler. One undoable step:

- the caption title: a copy of "Caption, plain" (restyle it, or edit it
  to another look from the gallery's Captions section), written to the
  project's `Titles/` folder as `<subtitle name> captions.ustitle` (the
  New Title folder rules when the project isn't saved);
- a new video track, "Captions", above the others, with a clip per cue at
  the cue's time from the sequence's start;
- cues that overlap in time go on a second track, "Captions 2" (and so on),
  since a track never overlaps.

**Caption templates.** Built-ins in a new category, Captions: "Caption,
plain" (text with an outline and shadow, bottom centre; the import's
default), "Caption, boxed" (a band across the bottom behind the text: text
layers have no box of their own) and "Caption, top". Each has a
`caption` field (wrapping text box, up to three lines at 1080p sizes) and
a `speaker` field shown only when a cue has one. A short fade in and out
(4 frames each), so a cue of any length plays (elastic timing; one shorter
than both fades plays them faster). Any title with a `{{caption}}` field
can be a caption template.

**Timing.** A cue's start and end become sequence frames by rounding to
the nearest frame at the sequence's exact rate (for example 30000/1001,
not 29.97). Every clip is at least one frame. Rounding never makes two
adjacent cues overlap or leave a one-frame gap: when neighbouring cues
share a boundary in the file, they share it on the timeline. Cues that
really overlap move to the next captions track.

**Text.**

- Line breaks within a cue are kept; the caption box wraps long lines.
- `<b>`, `<i>` and `<u>` (SRT and VTT) keep their style. The renderer never
  takes Pango markup from text (a field could hold anything); a text layer
  with `tags="basic"` has exactly these three tags turned into Pango
  attributes by our own code, and everything else stays literal text.
  Caption templates' text layers set it.
- VTT `<v Speaker>` fills the clip's `speaker` field. Other tags (`<font>`,
  `<c.class>`, `<ruby>`, `<lang>`, timestamps inside cues) are dropped,
  keeping their text. VTT cue settings (position, align, line) and
  `STYLE`, `NOTE` and `REGION` blocks are ignored.
- Entities `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&nbsp;` are decoded.

**Malformed files.** A subtitle file is untrusted input.

- Text: UTF-8 (with or without a byte-order mark); UTF-16 with a BOM is
  converted; anything else is read as Windows-1252 with a warning.
- A cue with a bad timestamp, an end before its start, or no text is
  skipped. The import still happens, and the status says how many cues
  were skipped and the first one's line.
- The file is refused, with the reason, when it has no usable cue, has a
  NUL byte (not text), is over 10 MB, or has over 20,000 cues. Nothing is
  imported then.
- Times past 24 hours are refused per cue; an SRT index that isn't a
  number, or out of order, is ignored (only the timing line counts).

Acceptance:

- [x] An `.srt` and a `.vtt` with the same cues import to the same clips:
      same tracks, positions, lengths and field values. (`titles-captions`:
      the same cues; placement and import are the same code, 0.70.0.)
- [x] At 23.976, 25, 29.97, 30 and 59.94 fps every cue starts on the frame
      nearest its time, no clip is shorter than a frame, and cues that
      touch in the file touch on the timeline (no rounding gaps or
      overlaps). (`titles-captions`, exact rates in 64-bit integers.)
- [x] Overlapping cues go to a second captions track; no track overlaps.
      (`titles-captions`, `titles-shell`; `check()` clean.)
- [x] Line breaks survive; `<b>`, `<i>` and `<u>` render bold, italic and
      underlined; `<v Speaker>` fills the speaker field; other tags and
      VTT settings disappear without losing words; markup-like text in a
      field that isn't one of those three stays literal. (`titles-captions`,
      `titles-render` "tags=basic"; live on Xvfb.)
- [x] One caption title per import: restyling it restyles every cue.
      (Every caption clip plays the one asset; `titles-shell`.)
- [x] A file with some bad cues imports the rest and reports them; a
      file with none, binary content, or over the limits is refused with
      a reason and imports nothing; an import is one undo step.
      (`titles-captions`, `titles-shell`; the status keeps the report.)
- [x] A 1,000-cue file imports in under two seconds and plays. (About
      0.5 s to read, place and import; the engine builds the 1,000-clip
      graph in about 0.3 s and shows a caption mid-way: `titles-engine`.)

Not in T5: exporting subtitles back out, word-level (karaoke) timing, and
per-cue positions from VTT settings.

#### T5.1 — Subtitle export (landed 2026-09-28, 0.71.0-beta.1)

**Export Captions…** (the `titles-export-captions` action, a button on a
caption's Title page) writes the project's captions to an `.srt` or
`.vtt`, chosen by the file name. Captions are the clips that play a title
with a `caption` field, on any track, in time order (overlapping ones as
overlapping cues, which both formats allow; importing puts them back on
lanes).

- Times: a clip's first frame and the frame after its last, as
  milliseconds rounded to the nearest (a frame is at least 16 ms, so
  importing gives the same frames back).
- Words: the `caption` field. `<b>`, `<i>` and `<u>` stay. In `.vtt`, a
  literal `&`, `<` or `>` is escaped; `.srt` is written as it is.
- Speaker: `.vtt`'s `<v Name>`. `.srt` has no speaker, so it gets the same
  `<v Name>` (our import reads it; players that don't know it skip the
  tag or show it).
- UTF-8, `\n` line ends, written to a temporary file and renamed.

Acceptance:

- [x] Export then import gives the same captions (positions, lengths,
      words, speakers, lanes) at 23.976, 25, 29.97, 30 and 59.94 fps, in
      both formats. (`titles-captions`; and in the editor: an imported
      `.srt` exported as `.srt` and `.vtt`, identical cues.)
- [x] A `.vtt` starts with `WEBVTT` and uses `HH:MM:SS.mmm`; an `.srt`
      numbers its cues and uses `HH:MM:SS,mmm`.
- [x] Words with a literal `<`, `&` or `>` survive a `.vtt` round trip.
- [x] Only caption clips are exported; a project without any says so and
      writes nothing. (`titles-captions`, `titles-shell`.)

#### T4.3 — Update from template (landed 2026-09-28, 0.72.0-beta.1)

A title made from a template remembers which one, and which revision of
it, so that when the template changes (a new app version's built-in, an
updated pack, or the user editing their own template), U-Stu Titles
offers to bring the title up to date without losing its text.

- **What's recorded.** Two root attributes of the `.ustitle`:
  `template="builtin:<id>"`, `"user:<id>"` or `"pack:<pack folder>/<id>"`,
  and `template-revision`, a 64-bit FNV-1a hex digest of the template's
  design: its file as `writeTitle()` writes it with the name and category
  cleared (so renaming a template isn't a change). Pictures are part of
  the design only by file name: a picture replaced under the same name
  isn't noticed. Set by `templateDocument()` (Use Template, New Title,
  the editor's gallery); cleared by Save as Template and Duplicate (a
  template doesn't point at a template). Titles made before T4.3 have
  none, and are never offered an update. No format version bump: older
  readers ignore unknown root attributes.
- **Resolving.** `builtin:` in the built-ins folder, `user:` and `pack:`
  in the template library. The reference comes from an untrusted file:
  each part must be a plain name (letters, digits, `-`, `_`, `.`, not
  starting with a dot), else it resolves to nothing. A template that's
  gone (deleted, pack removed) offers nothing.
- **The offer.** Opening a title whose template's revision differs from
  the recorded one shows a banner: "Its template “<name>” has changed"
  with **Update**. Nothing happens on its own.
- **The merge.** The title becomes the template's current design
  (layers, timing, background, size, fields), with the title's own
  field default text kept for every field the template still has (by
  name), and the new revision recorded. One undo step ("Update from
  Template"), not saved until the user saves. The toast names any field
  the template no longer has. Per-clip field values in the editor are on
  the clips, keyed by field name, so they survive untouched. Direct
  edits to the title's design (moved layers, text typed over a field)
  are replaced: that's what updating means, and undo brings them back.

Acceptance:

- [x] A title made from a built-in, a user template and a pack's
      template records the right reference and revision; Save as
      Template and Duplicate record none; the attributes round-trip.
- [x] Renaming a template doesn't change its revision; changing a
      layer does.
- [x] Hostile references (`user:../x`, `pack:a/../../b`, absolute
      paths, empty parts) resolve to nothing.
- [x] The merge keeps field text by name, takes the template's design,
      reports dropped fields, and records the new revision; undo restores
      the title exactly.
- [x] In U-Stu Titles: open a title, change its user template, reopen:
      the banner shows; Update applies it; the banner goes; saving and
      reopening shows no banner. (`titles-core`; the designer on a
      private Xvfb with a stale title: banner, Update, Ctrl+S, reopen.)

#### T5.2 — Caption colour and top placement (landed 2026-09-28, 0.73.0-beta.1)

The cheap subset of a subtitle file's own styling (Strategist: "if
cheap"): colour by the names every player knows, and top or bottom.

- **Colour.** WebVTT's eight default colour classes, `<c.white>`,
  `<c.lime>`, `<c.cyan>`, `<c.red>`, `<c.yellow>`, `<c.magenta>`,
  `<c.blue>` and `<c.black>` (other classes on the same `<c>` are
  ignored), and SRT's `<font color="…">` when it names one of them or
  gives its exact hex (`#ffff00`). Kept in the caption's words as
  `<c.yellow>…</c>`, a fourth basic tag: a `tags="basic"` layer draws
  the run in that colour (the fill is clipped to the run's glyphs and
  painted solid over the layer's own fill; the stroke and shadow stay
  the layer's). Any other colour is dropped with its words kept, as now.
  Export writes `<c.yellow>` in `.vtt` and `<font color="yellow">` in
  `.srt`.
- **Top or bottom.** A VTT cue with `line:` as a percentage under 50,
  or as a line number 0 or more (counted from the top), and an SRT cue
  starting `{\an7}`, `{\an8}` or `{\an9}`, goes at the top: its clip
  gets the field `placement=top` and plays a second caption title made
  from **Caption, top** (`<file name> captions (top).ustitle`), made
  only when a cue needs it. Everything else stays at the bottom.
  Horizontal position, size and alignment settings stay ignored. Export
  writes `line:0` (`.vtt`) or `{\an8}` (`.srt`) for a clip with
  `placement=top`.

Acceptance:

- [x] Each named colour, in `.vtt` and `.srt`, imports to the same
      `<c.name>` run and exports back; other colours and classes are
      dropped, words kept.
- [x] The renderer draws a `<c.yellow>` run yellow and the rest in the
      layer's fill; a scrambled or animated-unit layer ignores colour
      runs as it does the others.
- [x] Top cues go on the top caption title with `placement=top`; a file
      without any makes no second title; export then import keeps them
      at the top in both formats. (`titles-captions`, `titles-render`;
      in the editor: a `.vtt` with a yellow top cue and a cyan bottom
      one imported, shown in the preview, exported as `.srt` and `.vtt`.)

#### T6 — Lottie layers (planned 2026-09-28)

Lottie animations as a layer of a title, drawn by ThorVG
([ADR-021](adr/021-lottie-layers-via-thorvg.md), which owns the
decisions: the validator's limits, timing, threads, format version,
packs, packaging). Built in slices, each landed on its own:

1. **Spikes (no product code).** Standalone repros against the system
   `thorvg-devel`: render a generated Lottie at two sizes and aspects
   (how `tvg_picture_set_size` fits), fractional `set_frame`, two threads
   each with its own canvas (ADR-021 decision 6), and load from memory
   with an empty resource path. Findings go into
   `docs/developer/notes/titles.md`.
2. **Core** (landed 2026-09-28). `core/lottie_check`: the std-only JSON
   scanner and validator (ADR-021 decision 3), and the file's facts
   (size, `fr`, `ip`, `op`, whether it has text layers). `Layer` gains
   `LayerKind::Lottie` with `src`, `loop` (`loop`/`once`) and `speed`;
   `.ustitle` format 2 read and written (written only with a Lottie
   layer); `lottie::frameAt()`, the frame maths. Until slice 3 the
   renderer draws an animated layer as nothing, with a warning.
3. **Render** (landed 2026-09-28). titlerender draws the layer through
   ThorVG (`render/lottie_layer.cpp`, the only ThorVG caller) with the
   per-thread cache and the process-wide lock, composited like an image
   layer; the MLT producer keys its frame cache on time for it. Meson: ThorVG is an
   optional dependency of the titles render library (`dependency('thorvg-1',
   required: false)`), with `TITLES_HAVE_THORVG`. Without it the layer
   draws nothing and warns.
4. **Designer** (landed 2026-09-28). **Add Animation…** (in the Add menu; the UI says
   "animated layer" and names Lottie only as the file type, owner
   2026-09-28) picks a `.json`, validates it, copies it beside the title,
   and adds a layer sized to the animation and centred. The inspector has
   Loop/Once and Speed; the animation keeps its aspect in the layer's box.
   The animation strip shows the layer's run, and play and scrub move the
   animation with the title. Refusals are toasts naming the reason.
5. **Editor and packs** (landed 2026-09-28). Titles with Lottie layers
   play in the editor (through the module) and bake. Packs accept
   `lottie/*.json` (ADR-021 decision 10), validated like an added file.
   Built-in animated templates followed once the Flatpak carried ThorVG
   (slice 6): **Lower third with ringing bell** and **Subscribe with
   ringing bell**, with a Lottie bell that `tools/gen_title_templates.py`
   generates (`data/templates/animations/ringing-bell.json`).
6. **Packaging request.** The ThorVG module snippet for VE Installers
   (ADR-021 decision 8); not built by Text.

Acceptance:

- [x] The validator refuses every file in a hostile corpus (deep
      nesting, a huge number, bad UTF-8, an external image, a font path,
      `..`, an expression, a self-referencing precomposition, an
      oversized or mis-typed embedded picture, a bad header, too long,
      too many layers) with a reason, and accepts the good corpus; its
      time grows linearly with the file (load-proof: 7.5 MB against 1 MB;
      about 250 ms for 7.5 MB in a debug build). (`titles-lottie`, 40
      hostile files.)
- [x] `lottie::frameAt()` matches the formula at 23.976, 25, 29.97, 30
      and 59.94 fps title rates, speeds 0.5, 1 and 2, looping and once.
      (`titles-lottie`.)
- [x] The editor's producer and the designer's renderer give
      byte-identical frames for a title with a Lottie layer, at several
      frames and on two threads. (`titles-engine` at six frames;
      `titles-render` on four threads at once.)
- [x] A title without Lottie is still written as format 1; one with it
      as format 2, and it round-trips. (`titles-lottie`.)
- [x] Add Animation…, Plays (loop or once) and Speed work in the
      designer; the strip plays and scrubs it (checked on a private Xvfb
      with screenshots: added centred at its size, turning while playing).
- [x] A pack with a Lottie template installs; one with a hostile Lottie
      file is refused whole. (`titles-pack`.)
- [x] Built and tested with and without ThorVG; docs: titles.md
      (users), notes (findings), building.md (the optional dependency).
      (`-Dtitles_thorvg=disabled`: builds, titles tests pass.)

### Later


## Decisions needed from the owner

All decided by the owner on 2026-09-24:

1. **A separate process**, integrated as described under "Editor
   integration".
2. The name **u Studio Titles** (`u-studio-titles`, `com.ustudio.Titles`).
3. The brand fonts **may be bundled** in the Flatpak once their licences
   are checked (Space Grotesk, JetBrains Mono and Anton, all believed OFL).
4. **FX0 and T0 spikes first, then alternate** between the two tracks.
