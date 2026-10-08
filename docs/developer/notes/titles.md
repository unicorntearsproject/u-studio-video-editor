# Titles (T0 spikes and T1 renderer)

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › Titles

The T0 spikes of [doc 16](../../plans/v2/16-titles-tool.md) (ADR-012), run
2026-09-27 against MLT 7.40, Pango 1.5x and GLib 2.8x (Fedora 44). Each was
a standalone repro: a minimal `ustudio_title` MLT producer module (Pango
and Cairo, about 100 lines of C++) loaded from a curated module directory.
Nothing is wired into `src/`.

| # | Question | Answer |
|---|---|---|
| 1 | A custom MLT module from the curated directory, with YAML metadata | **Yes.** The module exports `mlt_register` with `MLT_REGISTER` and `MLT_REGISTER_METADATA`; symlinked into a curated directory (every module but `qt6` and `glaxnimate-qt6`), `Mlt::Factory::init(dir)` loads it and `Repository::metadata(producer, "ustudio_title")` returns its YAML. The metadata callback reads the file itself (`mlt_properties_parse_yaml`), so the drop-in says where its YAML lives. No Qt mapped in the process. |
| 2 | Pango on the consumer thread with a private font map; 4K cost; leaks | **Yes.** A `thread_local` `pango_cairo_font_map_new()` per rendering thread (Pango's default font map isn't safe across threads). A two-layer lower third (a translucent bar and a line of text) at 3840×2160 takes **12.7 ms a frame** on a worker thread, including the conversion below. **No growth** over 10,000 frames (RSS 144,452 KB before and after). |
| 3 | Premultiplied to straight alpha into MLT `rgba` | **Yes.** Cairo's `ARGB32` is premultiplied BGRA (little-endian); one loop divides by alpha and reorders. A 60% bar of `#FC3CBA` reads back as 253/60/187 at alpha 153. |
| 4 | `ustudio_title` through MLT's `xml` | **Yes** for the `xml` consumer and producer: `mlt_service`, `text` and `font` survive and the reloaded producer renders. Through `u-studio-render`, it needs the module shipped as a drop-in (its module directory joins the curated one, IP4), which is T1's packaging. |
| 5 | `FcConfigAppFontAddDir` visible to Pango | **Yes.** A font directory added with `FcConfigAppFontAddDir(nullptr, dir)` before the first layout is used by Pango's font maps: "UStuTestFnt Bold" (a renamed DejaVu Sans copy, not installed) resolved to that family. Each process adds it itself, so the editor and `u-studio-render` both call it at startup. |
| 6 | A GApplication action invoked from a second process | **Yes.** `gapplication action com.ustudio.T0Spike open-title "'/path/x.ustitle'"` reached the running app's `open-title` action (parameter type `s`) over its session-bus name: the way a titles tool can ask the editor to open or refresh a title. No `.desktop` file or D-Bus activation is needed while the app runs. |

## T1: the renderer

Found while building `drop-ins/titles/render/` (2026-09-27, Pango 1.57,
Cairo 1.18, fontconfig 2.17):

- **Hinting off, or the preview isn't the export.** With Cairo's default
  font options, glyph outlines and metrics snap to the output's pixel grid,
  so a half-size preview lays text out differently from the full-size
  frame. The renderer lays out in canvas pixels with
  `CAIRO_HINT_STYLE_NONE`, `CAIRO_HINT_METRICS_OFF` and
  `pango_context_set_round_glyph_positions(FALSE)`, then scales. A
  960×540 render's ink box is the 1920×1080 one halved, to within 3 px
  (`titles-render`).
- **`FcConfigAppFontAddDir()` returns true for a directory that doesn't
  exist**, so `addFontDirectory()` checks for the directory itself. A font
  map made before the call doesn't see the new fonts. Each rendering
  thread rebuilds its font map when a directory has been added since its
  last render.
- **Which font was really used**: `pango_context_load_font()` then
  `pango_font_describe()` gives the family Pango picked. A missing
  "Space Grotesk" comes back as "Noto Sans" on Fedora 44. Generic names
  ("Sans", "monospace") never match their result and aren't warned about.
- **Cost** (release build, one thread): doc 16's lower third with a
  shadowed gradient name, a subtitle, an outlined bar and a radial dot
  takes 4.3 ms at 1920×1080 and 13.9 ms at 3840×2160. A debug (`-O0`)
  build is about four times slower, mostly in `std::vector` fills and the
  blur's loops, so don't judge speed on one.
- Text goes to Pango with `pango_layout_set_text()`, never as markup, so a
  field value like `<b>` or `&` shows as typed.

## T1: the producer in the editor's graph

- **Make the producer through `loader`, not the factory.** A producer made
  with `mlt_factory_producer(profile, "ustudio_title", path)` has no
  normalising filters, so nothing converts its `mlt_image_rgba` frames to
  the format the compositor asks for. The `composite` transition read the
  RGBA bytes as YUV 4:2:2, and a fully transparent title came out as an
  opaque green frame, (0, 136, 0), which is Y = U = V = 0 converted to RGB.
  `Mlt::Producer(profile, "loader", "ustudio_title:<path>")` splits the
  service off at the first colon (`producer_loader.c`, `create_producer()`)
  and attaches the normalisers like any media file. Repro:
  `titles-engine`, "a title clip plays over the track below it".
- **Metadata without a YAML file.** `mlt_repository_register_metadata()`'s
  callback may build the properties itself. MLT caches the result on the
  service and frees it with `mlt_properties_close`, so the module stays one
  file wherever it's installed.
- **A self-contained module.** `libmltustudio.so` links the titles core,
  the renderer and the parts of `src/core` they use statically, with
  `-Wl,--exclude-libs,ALL`, so it exports only `mlt_register`. It loads into
  the editor, `u-studio-render` or `melt` whatever they export. The drop-in
  module `libustudio-dropin-titles.so` is the opposite: it resolves core
  and engine against the program, which links those libraries whole when
  any drop-in is a module.
- **Time a frame by `mlt_frame_original_position()`.** `get_image` runs
  after the frame has left the producer, and a playlist has by then set the
  frame's position to its own, the sequence frame. So
  `mlt_frame_get_position()` in `get_image` is where the clip sits, not how
  far into it. The first position set on a frame is kept as
  `original_position`, and that's the producer's own. Until 0.61 the
  producer used the other one, and a title clip anywhere but the start of
  the timeline played its intro and outro at the wrong frames (MLT 7.40).
  Repro: `titles-engine`, "a title clip later in the sequence animates
  from its own start".
- **Dynamic fields and the cache.** `{{timecode}}` needs the clip's place
  in the sequence, which the producer can't see. The engine extension sets
  `timeline_start` (the clip's position minus its in point), and EngineSync
  rebuilds clip producers with the graph, so it stays current when a clip
  moves. The frame cache keys on the substituted text of the layers with
  dynamic fields, so a `{{clip_time}}` title redraws once a second, not
  every frame.
- **Baking.** Bake Title runs `exportTitle()` (the `--title-export` code)
  on a titles job thread at the sequence's rate, for source frames
  0..out with the clip's fields and `timeline_start`, then probes the file
  with `EngineSync::probeMediaFile()` and swaps it in with `AddAsset` +
  `SetClipAsset` in one command. The export polls `consumer.is_stopped()`
  so the app's shutdown (`jobsCancelled()`) can stop it. Outside a
  `GraphBuildScope`, `ProducerUse::Graph` is the CPU chain, so a bake
  never gets movit's normalisers. Stock `melt-7` spawned from a VS Code
  snap terminal fails on the snap's `libpthread`; the test runs it with a
  plain environment.
- **Boundless assets grow.** `InsertClip` extends a boundless asset's
  `lengthInSequenceFrames` to its furthest clip, after which it's no longer
  boundless and a longer trim is refused. Stills avoid this with
  `isStillImage`, and so do titles.
- **Watching a file saved atomically.** `g_file_monitor_file()` on the title
  reports a write-then-rename save as several events. A 150 ms settle timer
  turns them into one reload, and the fingerprint (size and mtime in ns)
  decides whether anything changed.

## T2a: the designer app

- **Cairo's `ARGB32` is `GDK_MEMORY_DEFAULT`** (premultiplied, native
  endian), so the renderer's buffer becomes a `GdkMemoryTexture` without
  conversion.
- **`GtkFileDialog` starts in the process's working directory** when it's
  given no folder. A test run launched from a checkout saved into it
  (2026-09-27), so the Save dialog now starts in the title's own folder, or
  in Videos, or home.
- **Driving dialogs on Xvfb.** With no window manager, XTest key events go to
  the window that already has X focus (the main window), not to a new
  dialog, and clicking the dialog doesn't move focus. Set entry text over
  AT-SPI (`Atspi.EditableText.set_text_contents`) and press buttons through
  their actions, with the AT-SPI bus started inside the private D-Bus
  session and `GTK_A11Y=atspi`. Pass `DISPLAY`, `GDK_BACKEND=x11` and
  `GDK_DEBUG=no-portals` into that session, so the dialog is the app's own
  and nothing opens on the real desktop.

## T2d: exporting a title on its own

- **Codec options through MLT's avformat consumer.** Unknown consumer
  properties go to the codec as AVOptions, and a leading `v` is dropped when
  the name isn't an option itself (`consumer_avformat.c`,
  `apply_properties()`): `vprofile=4` sets `prores_ks`'s profile to 4444
  without touching MLT's own `profile`. `ffprobe` confirms `profile=4444`.
- **What the decoders report.** ProRes 4444 written from `yuva444p10le`
  decodes as `yuva444p12le`. VP9 with alpha shows as `yuv420p` with the
  stream tag `alpha_mode=1`: FFmpeg's native VP9 decoder ignores the alpha,
  and `-c:v libvpx-vp9` decodes it. QuickTime Animation stays `argb`, PNG
  `rgba`.
- **No compositing, no fringe.** Feeding the producer straight to the
  consumer (`mlt_image_format=rgba`, `an=1`, `real_time=-1`,
  `terminate_on_pause=1`) gives the renderer's alpha exactly. A 50% bar
  reads back at alpha 128 in its own colour, which the compositing path
  can't do today (see "T1: the producer in the editor's graph").
- **Cost**, 2 s of doc 16's lower third at 1080p30 on one machine: PNG
  2.8 s, ProRes 6.3 s, QuickTime Animation 2.1 s, VP9 19.4 s (libvpx is
  slow), H.264 3.9 s.

## T3a: animation

- **Behaviours are offsets.** Their expansion (`core/animation.cpp`) adds
  to the layer's own values (x, y, rotation, blur, tracking, shift) or
  multiplies them (opacity, scale, reveal, shadow opacity), so "Rise" works
  on a layer that also has its own keyframes. Detach to keyframes samples
  own + offset at each key position, which is exact for the shipped
  behaviours (`titles-animation` compares every frame).
- **MLT's back easings overshoot hard**: `back_out` from 1.8x to 1x dips to
  0.7x mid-way, and `back_in` swells about 37% before it shrinks.
  `easedValue` reproduces MLT exactly, so the catalogue defaults Kinetic
  stack to `cubic_out` and Collapse to `cubic_in`. Pop (0.5x to 1x) keeps
  `back_out`: its overshoot to 1.19x reads as a pop.
- **Seeded order and noise are platform-proof**: splitmix64 and
  Fisher-Yates in our code, not `std::shuffle`/`<random>` distributions,
  whose algorithms differ between standard libraries.
- **Units from Pango clusters.** `pango_glyph_item_iter` over each run
  gives each grapheme cluster's glyphs; a unit is drawn with
  `pango_cairo_glyph_string_path` under its line, word and character
  transforms composed about their own centres. With nothing animated in
  units, text still draws as one layout: existing titles are
  byte-for-byte what they were. A blurred unit is drawn and blurred in a
  small surface around just that unit.
- **The producer's frame cache** keys on layer states. Units, the
  scramble and the typewriter's cursor change the picture with the states
  unchanged, so for such titles the moment is part of the key (found by
  `titles-engine`'s byte-for-byte producer-against-renderer case).
- **Shimmer on a solid fill** turns it into a colour/highlight/colour
  gradient for the Shift loop to slide; on white the highlight is
  invisible, which is expected.

## T4.2: templates

- **The library is folders.** A user template is
  `<library>/<id>/template.ustitle`, with its pictures copied into
  `images/` and referenced relatively, so it moves as one folder. That is
  the shape a template in a doc 20 pack has, so T4b installs packs into it.
  Built-ins are flat files and read-only; "Edit a Copy" duplicates one.
  `core/template_library.cpp`; tests in `titles-core`.
- **The built-ins are generated.** `tools/gen_title_templates.py` writes
  all of them; edit it and rerun rather than editing the files. Sizes at
  1080p: a primary line at least 60 px, a secondary at least 40, a tag
  at least 30 (owner review, 2026-09-28). `titles-render` checks each one
  reads cleanly, fills every field, draws, and stays on the canvas;
  `titles-engine` checks the producer's frame equals the renderer's, byte
  for byte, for each.
- **Compare producer and renderer at the title's own size.** At another
  profile size, the loader's normalisers have the producer draw at its
  native size and scale it with swscale, so the bytes differ from
  `renderTitle()` at the target size (every 1920x1080 built-in differed at
  640x360; all match at 1920x1080). One renderer copy renders a variable
  font (Space Grotesk) identically on several threads; checked with a
  standalone two-thread repro.
- **Gallery thumbnails** render mid-hold on a worker, as the behaviour
  drawer's do, and are never cached on disk: there are no preview images
  in the repository.
- **New Title makes the file first**, then opens the designer with
  `--gallery` (the owner-approved deviation from doc 16's "save sends it
  back"): the clip is on the timeline at once, and the file watch brings
  the picked design back. `ShellHost::projectFolder()` (IP5, API 7) says
  where. An unsaved project's title stays linked after the project is
  saved: saved in that folder, the writer stores it relative and it moves
  with the folder; saved elsewhere, it stays absolute.
- **A drop-in's shortcut without Ctrl, Alt or Super** (Shift+T) joins the
  editor's single-key guard, so it never fires while a text field has
  focus.
- **The header's "T"** (IP5 `addHeaderButton`, API 8) uses an icon from
  the drop-in's own GResource (`editor/titles-editor.gresource.xml`),
  added to the icon theme's resource path in `extendShell()`. In the
  builtin build the drop-in is a static library, and the linker drops the
  generated resource object (and its registering constructor) unless
  something references it: `extendShell()` calls
  `ustudio_titles_editor_get_resource()` for that. Without it the button
  showed GTK's missing-image icon.

## T4b: template packs

- **Validate in memory, then write.** `package/archive.cpp` reads every
  entry into memory, stopping at the limits as it reads (it never trusts
  an entry's declared size), and `package/pack.cpp`'s `validate()` checks
  the whole set before `install()` writes a byte. So a traversal, a link
  or a bomb never reaches the disk, and a refused pack leaves nothing.
- **Only zip, tar and gzip** are enabled in libarchive's reader, so no
  other decoder sees a stranger's bytes.
- **Two path rules.** Entry paths are strict: no `..` at all
  (`normalisePath()`). A template's picture reference is resolved from
  `templates/` and may climb (`../images/logo.png`), but never above the
  pack's root (`resolveInPack()`), and must land in `images/`.
- **An installed pack is My Templates' shape**: `<library>/packs/<id>/`,
  one folder per template, the fonts in `fonts/` and `pack.xml` kept. A
  title made from one gets the fonts in `fonts/` beside it, where the
  producer looks.
- **Save as Pack validates its own output** before writing, so what we
  send out is what another copy accepts.

## T7: sharing

- **libadwaita maps libcurl.** Fedora's libadwaita links libappstream (for
  the About dialog's release notes), and libappstream links libcurl, so
  the editor and U-Stu Titles map libcurl without calling it. "No network
  library" is checked on our own link lines (`readelf` NEEDED) plus
  libsoup anywhere loaded (`titles-no-network`); the Flatpak's missing
  `--share=network` is what keeps the editor offline.
- **The hand-off is a D-Bus action.** The titles drop-in registers
  `app.install-template-pack(s)` on the editor's GApplication; the helper
  asks whether `com.ustudio.VideoEditor` has an owner first and calls with
  `NO_AUTO_START`, so it never starts the editor. Any program in the
  user's session may call it: the pack is validated in full before a byte
  is written, as for Open Pack. Without a running editor the helper
  installs into `pack::templatesLibrary()` itself.
- **Tests must not reach the real library or editor.** GLib caches the
  XDG directories at first use, so setting `XDG_DATA_HOME` inside a test
  process does nothing; a first version of the hand-off test installed
  into the shell's data directory. `handOff()` takes the library and the
  editor's bus name, and the test passes a scratch one and a name nobody
  owns.
- **PKCE verifiers from the OS**: `std::random_device`, not GLib's
  `g_random_*`, which isn't cryptographic.

## T5: captions

- **One title, many clips.** An import makes one caption title and gives
  each cue its own clip with the words in `field.caption` (and
  `field.speaker`), so restyling is one edit and fixing words is the Title
  page. `captions::ImportCaptions` is one command that runs AddAsset,
  AddTrack, then InsertClip, SetClipFields and RenameClip per cue, taking
  each id as it's made; redo re-applies the same steps, so ids come back.
- **Frames at the exact rate, in integers.** A time becomes
  `(2·ms·num + 1000·den) / (2000·den)`: the nearest frame at 30000/1001
  without floating point, and in 64 bits (a day's milliseconds times a
  rate's numerator stays far under 2^63; `__int128` isn't portable).
  Cues that share a time in the file share the frame.
- **`tags="basic"` is not markup.** The renderer still sets plain text;
  for such a layer it takes out exactly `<b>`, `<i>` and `<u>` after the
  fields are filled and adds Pango attributes over those byte ranges. A
  scramble changes the letters, so a scrambled frame drops the styles.
- **An import handler's own status stays.** The shell used to follow
  every handler with "Imported <path>", hiding the captions report; it
  now does so only when the handler said nothing (`m_statusCount`).

## T6: ThorVG (Lottie), slice 1 spikes

Measured 2026-09-28 against Fedora 44's `thorvg-devel` 1.0.6 (built with
`-Dthreads=true`, `extra` at its default, `-Dloaders=all`), with a
standalone repro rendering a generated 200 × 100, 30 fps, 30-frame Lottie
(a red square keyframed from x = 50 to 150) through the C API. ADR-021
builds on these.

- **Size.** `tvg_picture_get_size` gives the file's `w` and `h`.
  `tvg_picture_set_size(w, h)` stretches to exactly that size, not
  uniformly: at 400 × 100 the square was twice as wide and no taller.
  Contain and cover fits are ours to work out (a uniformly scaled size,
  then a translate).
- **Frames.** `tvg_animation_get_total_frame` is `op − ip` (30);
  `get_duration` is that over `fr` (1 s). `tvg_animation_set_frame`
  takes fractional frames and interpolates (15.5 lies between 15 and 16).
  Frames at or past `op` clamp to the last frame, `op − 1`.
- **Threads: not safe, even with separate objects.** Four threads, each
  with its own canvas, animation and picture, crashed in 6 of 10 runs
  (SIGSEGV in `LottieLoader::~LottieLoader` from `LoaderMgr::retrieve`,
  while another thread was in `rasterShape`). Giving each thread
  different bytes (so no loader could be shared) still crashed in 6 of
  10. With one process-wide mutex around every ThorVG call: 10 of 10
  clean, and every frame byte-identical to a single-threaded reference.
  titlerender therefore serialises ThorVG behind one mutex (ADR-021
  decision 6).
- **Loading from memory.** `tvg_picture_load_data(..., copy=false)`
  caches by the data's address, process-wide; we always pass
  `copy=true`.
- **Expressions run** in Fedora's build: an `"x"` expression on the
  position (`$bm_rt = [20, 50]`) moved the square off its keyframes. The
  validator must refuse expressions (ADR-021 decision 3).
- **External images are read from disk, from the root.** With an empty
  resource path, an image asset's `u` + `p` is opened as `"/" + u + p`:
  `p = "../../../../etc/hostname"` opened `/../../../../etc/hostname`,
  that is `/etc/hostname`. Nothing but our validator stops that in a
  build with file access (Fedora's); the Flatpak's ThorVG is built with
  `-Dfile=false` as a second guard (ADR-021 decision 8).

## T6: ThorVG in titlerender (slice 3)

- **One canvas per animation.** Adding an animation's picture to a new
  canvas for each frame and destroying the canvas afterwards drew the
  first frame and then nothing: every later frame came back transparent
  (ThorVG 1.0.6, `test_titles_render`, 2026-09-28). Keeping one canvas per
  loaded animation, with the picture added once, and pointing it at the
  frame's buffer with `tvg_swcanvas_set_target` before
  `tvg_canvas_update`/`draw`/`sync`, draws every frame. The reference
  counts were fine (the animation holds the picture; a canvas adds and
  drops its own reference), so it's render state tied to the first
  canvas, not a lifetime bug.
- **The producer's frame cache.** `ustudio_title` reuses its last image
  while every layer's state, the fields and the size are unchanged. An
  animated layer changes with time alone, so a title with one keys the
  cache on the moment too (`textAnimates`). Without that, the producer
  showed the first frame of the animation for the whole clip, while the
  renderer moved.
- **One copy of the lock per process.** The ThorVG lock lives in
  titlerender, so a process must hold one copy of it that draws
  animations. The editor's side of the drop-in doesn't link titlerender
  (the MLT module does), and the designer links its own. The
  titles-engine test holds two copies but calls them in turn on one
  thread.
