# Effects

[Docs home](../README.md) › [User guide](README.md) › Effects

Effects change how a clip looks or sounds: colour, blur, glow, grain,
keying, distortion, audio clean-up and hundreds more. You can put them on
one clip, several clips, a whole track, or the whole finished picture, make
them change over time, and undo anything.

> **Early days.** Effects come with the *Effects* add-on. Builds made from
> source include it; for the Flatpak it's a separate add-on
> ([Installing › Add-ons](installing.md#add-ons)). Without it, a project
> that uses effects still opens and saves; it just plays without them.

## Opening the Effects pages

Click the round **Inspector** button at the top right of the picture, or
press **F9**. Two pages appear on the right: **Effects** (the effects on
what you've selected) and **Add** (every effect you can add). On a wide
window the panel sits beside the picture; on a narrow one it slides over
it.

To give the space back to the picture, click the panel button at the
right end of its tabs, or press **F9** again. On a wide window U-Stu
remembers whether the panel was open and opens it the same way next time.

Press **E** to jump straight to **Add**.

## Adding an effect

1. Select a clip on the timeline (or several; see below).
2. Press **E**. The **Add** page shows a tile for each effect: your clip's
   current frame with that effect on it.
3. Point at a tile to try it: the picture shows your clip with that effect,
   without changing anything yet. Move away, or press **Esc**, and it goes
   back.
4. Click the tile, or press **Enter**, to add it.

You can also:

- **Search.** Type in **Search effects**: "blur", "glow", "key", "warm".
  **Enter** adds the best match.
- **Browse by section.** **Featured** picks, **Recent** (what you've added
  lately), **Looks**, **All**, or a category (Colour, Light, Stylise, …).
- **Drag a tile** onto the picture (it goes on the clip at the playhead) or
  onto the **Effects** page (it goes on what that page shows).

Each tile has a badge: **light**, **medium** or **heavy** is how much work
the effect is for each frame, and a heavy one may slow playback on a busy
project. **checking…** means U-Stu hasn't finished checking that effect is
safe on this computer yet; it can't be tried on the picture until then
(see [Unstable effects](#unstable-effects)).

## Changing an effect

The **Effects** page shows one card per effect, top to bottom in the order
they apply. At the top, choose what the page is about: **Selected clip**,
**Its track** (everything on that track), or **Whole sequence** (the
finished picture).

On each card:

- The **switch** turns the effect off and on, keeping its settings.
- **↑ / ↓** move it earlier or later in the order.
- The **bin** removes it.
- **Mix** blends the effect with the picture without it: 100% is the full
  effect, 50% half of it.
- Below that, a control for each setting: drag a slider, type a number,
  pick a colour or choose from a list.

Every change can be undone with **Ctrl+Z**. A drag of a slider is one undo
step, however long you drag.

**On the picture:**

- **Colours:** click the **pipette** beside a colour, then click the
  picture. It takes the colour of the selected clip at that spot, as the
  clip is before its effects: what you want for keying out a background.
  **Esc** cancels.
- **Rectangles** (an area to fix or to work on): click the **selection**
  button (a dashed rectangle) beside the numbers to show the rectangle over
  the picture, then drag it, or drag its corners. Click the button again to
  hide it.

### Limiting an effect to part of the picture (masks)

Each effect's **Mask** chooses where it applies: **None** (everywhere),
a **Rectangle** or an **Ellipse**. With a shape:

- the **selection** button beside it shows the shape on the picture: drag
  it, or its corners, to place it;
- **Invert** applies the effect everywhere *but* inside the shape (blur
  everything except a face);
- **Soft edge** fades the effect out gradually at the shape's edge.

**Mix** still applies, inside the shape.

## Making an effect change over time (keyframes)

Beside each number there are three small buttons: **‹**, a **star** (the
pin), and **›**.

1. Move the playhead to where the change should start and set the value.
2. Click the **star** (or press **P**) to pin that value there. The star
   lights up: this setting now changes over time.
3. Move the playhead to where the change should end and set the new value.
   It's pinned there automatically.

Now the effect moves from the first value to the second as the clip plays.
**‹** and **›** jump to the previous and next pinned point. Click a lit
star to remove that point; removing the last one keeps the value as it is.

When the playhead is on a pinned point, **Feel** chooses how the value
moves to the next point: **Linear** (steady), **Smooth**, **Ease in**,
**Ease out**, **Snap**, **Bounce**, **Elastic**, or **Hold** (it stays put,
then jumps), plus every other curve in the list.

### Moving, sizing and turning a picture over time

With one clip selected, the **Effects** page starts with a **Transform**
card: the picture's **X** and **Y** (its centre, in pixels from the
frame's top left), **Width**, **Height** and **Rotation** (degrees
clockwise). They pin, step and feel exactly like an effect's values, and
record the same way, so a picture can glide in, grow, or spin over the
clip.

- A picture that's fitted or stretched to the frame is placed where it
  shows the first time you change its position or size, or pin any value.
- Crops don't change over time.
- On a picture that changes over time, dragging it or its handles on the
  preview, nudging it, and **Edit Transform** (`Ctrl+T`) change the
  keyframe at the playhead (or add one there) for the values you move,
  just as the card does.

### Seeing and shaping the curve (curve lanes)

Select a clip and press **C**: a lane opens under it on the timeline for
each value that changes over time (its transform's first, then its
effects'), showing its curve, with a dot for each keyframe.

- **Drag a dot** left or right to move the keyframe in time, up or down to
  change its value. The whole drag is one undo step.
- **Double-click** a lane to add a keyframe there, on the curve.
- Press **C** again to close the lanes.

### Recording a change as you play (touch-record)

For a change that follows the music or the action, perform it instead of
pinning points one by one:

1. Click the **record** button (the dot) beside the setting: it turns red.
2. Play, and move the setting's slider while it plays.
3. Let go. U-Stu keeps just enough keyframes to follow what you did, so
   the curve stays easy to adjust afterwards. It replaces the keyframes
   over the stretch you recorded; the rest stay.

One performance is one undo step. Click the red button again to stop
recording that setting. Recording works without playing too: each
position of the playhead you move the slider at is recorded.

## Several clips at once

Select several clips (click one, then **Ctrl**-click or **Shift**-click
others). The **Effects** page then shows the effects they all have, and a
change applies to every one of them as one undo step. Adding an effect or a
look adds it to all of them. (Moving effects and keyframes are one clip at
a time.)

## Adjustment blocks (the FX lane)

An adjustment block puts effects on everything beneath it for a stretch of
time: a grade for one scene, a blur behind a title, a glow for the chorus.
Blocks live in the **FX** lane, the thin lane above the tracks.

1. **Drag across the FX lane** to draw a block over the time it should
   cover. It's selected, and the **Effects** page shows it.
2. **Add effects** to it as to a clip: press **E** and pick, or search.
3. **Drag the block** to move it, **drag its ends** to lengthen or shorten
   it, and **drag the small dots at its top corners** inwards to fade its
   effects in and out.
4. On the **Effects** page, **Affects** chooses what it changes: **Every
   track**, or a track and those below it (so a title on a higher track
   stays untouched). The **bin** removes the block.

Clicking a clip on the timeline takes the **Effects** page back to clips.

## Copy and paste

**Ctrl+Shift+C** copies the effects on the **Effects** page.
**Ctrl+Shift+V** pastes them onto the selected clips: **after these**
(added to what's there) or **instead of these** (replacing them). Both are
also in the page's **⋮** menu.

## Looks

A look is a set of effects saved together, applied in one go.

- **Brand looks** come with U-Stu: *Unicorn Glow*, *Neon Night*,
  *Stream Punch*, *Pastel Dream* and *Film Grain*.
- **Save your own:** set up a clip's effects, then **⋮ › Save as a look…**
  and give it a name. It's saved in the project.

Find looks under **Looks** on the **Add** page (or by name in the search).
Try one by pointing at it, and apply it by clicking or dragging, like an
effect.

## Transition styles

With the add-on, a dissolve between two clips can play in another style:
a **dip to black**, a **flash**, a **blend** (**Additive**, **Screen** or
**Lighten**: both clips at once half-way, brighter than a plain
dissolve), a **slide** or **push** (the next clip
slides in over this one, or pushes it out, from any side), a **zoom** or
**spin** (it grows in from the middle, turning as it comes), or one of 20
**wipes** (left, right, up, down and the diagonals, circle, clock,
diamond, barn doors, blinds, checkerboard, blocks, star, sparkle and the
unicorn horn).

1. Put the playhead on a dissolve, or double-click one on the timeline.
   Or press **T** to add a dissolve at the cut nearest the playhead on the
   active track (about half a second, like **Add Transition**).
2. The **Transitions** page opens in the inspector, and the dissolve it's
   about is outlined on the timeline. Each tile shows its style with your
   own two clips, half-way through the transition (a drawing, the outgoing
   clip in pink and the incoming one in blue, until the picture is ready).
3. Click a tile to play the dissolve that way. **Ctrl+Z** puts the last
   style back.

A wipe has two settings above the tiles: **Softness** (how blurred its
edge is) and **Reverse** (run it the other way). A slider drag is one undo
step.

**Sound** chooses how the two clips' sound crosses: **Even crossfade**
(the default), **Equal power**, which keeps the level up through the
middle, better for music and ambience, or **Cut**: the first clip's sound
plays to the middle of the transition and the second's from there, with
no blend (for dialogue, or a beat). It stays when you change the style.

The style is saved with the project, and a render plays it exactly as the
preview does. Opened where the add-on isn't installed, a styled
dissolve plays as a plain one; the style comes back with the add-on. A saved project with wipes gets a small `ustudio-wipes`
folder beside it: keep it with the project file.

With GPU acceleration on, a wipe can stutter in the preview at **Full**
quality while it plays; set the preview to **Half** to watch it smoothly.
The render is unaffected.

## LUTs

A LUT (a `.cube` file) is a ready-made colour grade, from a camera maker
or a colourist. U-Stu keeps a library of them:

1. On the **Add** page, click the **open** button beside **Unstable** and
   choose one or more `.cube` files. They're copied into a `luts` folder
   beside your saved project (or into your own library, when the project
   isn't saved yet), so the project doesn't depend on where they came from.
2. Choose **LUTs** in the section list: a tile for each. Point at one to
   try it, click to add it, as with any effect.

**LUT (.cube)** among the effects does the same with any file you choose.
Keep a project and its `luts` folder together: move or copy the whole
folder and the project finds its LUTs where they are now.

## Before and after

- **Hold \\** (backslash): the picture without the selected clip's effects,
  until you let go.
- **Compare** (the two-panes button on the **Effects** page): the picture
  is split, without the effects on the left and with them on the right.
  Drag the dividing line. Click **Compare** again to turn it off.

Both show the frame the playhead is on; pause to compare a moment.

## More effect families

Besides the effects U-Stu comes with, it can use audio plugins you
install:

- **LADSPA** audio plugins are found by themselves. For many more audio
  effects (equalisers, compressors, limiters, noise gates), install
  **LSP Plugins**' LADSPA set (on Fedora: `lsp-plugins-ladspa`); the **Add**
  page suggests it under **Audio** when it's missing.
- **VST2** plugins and **OpenFX** plugins are off unless you turn them on:
  the **⋯** button on the **Add** page. They're loaded when U-Stu starts,
  so a change applies the next time you start it.

Every plugin is checked like the other effects, and a plugin that would
load Qt is never loaded at all.

## Unstable effects

When you first run U-Stu with the Effects add-on (and after installing new
effects), it checks each effect in the background, safely apart from your
project: that it doesn't crash, hang or ruin the picture. This takes a few
minutes and doesn't need you. Most effects pass.

An effect that fails is **turned off**: it's hidden from the **Add** page,
and a project that already uses it plays without it (its card says so).
Tick **Unstable** on the **Add** page to see them anyway; they may take the
editor down. See [Troubleshooting](troubleshooting.md#an-effect-is-missing-or-turned-off).

Effects that only run on a graphics card's own compute interfaces (FFmpeg's
Vulkan, OpenCL, CUDA, VAAPI and similar versions, such as "Gblur Vulkan")
aren't offered at all: the editor hands effects ordinary pictures, which
they can't use. Their ordinary versions ("Gblur") are there.
