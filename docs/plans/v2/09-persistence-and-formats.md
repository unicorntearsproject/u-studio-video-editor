# 09 — Persistence and formats

## Project file: MLT XML with an `ustudio:` namespace (ADR-004)

A `.ustudio` file is an MLT XML document (`<mlt>` root) that:

1. `melt` (and, from M6, `u-studio-render`) can render **directly**, with no editor
   involved, because the tractor/playlist/filter/transition structure is the
   real MLT graph. This is the same strategy kdenlive uses (`.kdenlive` files
   are MLT XML with `kdenlive:` properties).
2. Carries everything the model needs to round-trip **losslessly** in
   `ustudio:*` properties, so loading never has to infer model structure from
   the MLT graph.

Written by **our serialiser** (`core/xml/writer.cpp`), not by MLT's `xml`
consumer, because the model is the truth and MLT's serialiser emits
implementation details (cached lengths, auto-attached normalisers) that
churn diffs and can't carry our ids cleanly. Read by our reader
(`core/xml/reader.cpp`, libxml2) which looks **only** at `ustudio:*`
properties and ids, then re-derives the graph. The MLT-facing structure is
regenerated on save; it is output, not input.

### Sketch

```xml
<?xml version="1.0"?>
<mlt LC_NUMERIC="C" version="7.40.0" producer="main_bin" root="/home/jj/Videos/promo">
  <profile description="ustudio" width="1920" height="1080" frame_rate_num="30000" frame_rate_den="1001" … />
  <!-- bin: one master producer per asset -->
  <producer id="asset3" in="0" out="7499">
    <property name="resource">clips/intro.mp4</property>       <!-- relative to root -->
    <property name="ustudio:asset_id">3</property>
    <property name="ustudio:folder">/b-roll</property>
    <property name="ustudio:fingerprint">10485760:1757600000123456789</property>
    <property name="ustudio:proxy">…</property>
  </producer>
  <playlist id="main_bin">          <!-- keeps producers referenced; kdenlive convention -->
    <entry producer="asset3" in="0" out="7499"/>
  </playlist>
  <!-- one playlist per track, top track LAST (MLT order) -->
  <playlist id="track_a1"> <property name="ustudio:track_id">11</property> … </playlist>
  <playlist id="track_v1">
    <property name="ustudio:track_id">10</property>
    <property name="ustudio:kind">video</property>
    <property name="ustudio:name">V1</property>
    <blank length="30"/>
    <entry producer="asset3" in="120" out="420">
      <property name="ustudio:clip_id">42</property>
      <filter id="fx7"><property name="mlt_service">affine</property>
        <property name="transition.rect">0=0 0 1920 1080 1;90~=100 50 1720 968 1</property>
        <property name="ustudio:effect_id">7</property></filter>
    </entry>
  </playlist>
  <tractor id="seq1" in="0" out="…">
    <property name="ustudio:sequence_id">1</property>
    <property name="ustudio:next_id">300</property>
    <property name="ustudio:markers">[{"id":5,"at":120,"text":"drop","color":2}]</property>
    <track producer="black"/><track producer="track_a1"/><track producer="track_v1"/>
    <transition …composite…/> <transition …mix…/>
  </tractor>
</mlt>
```

Paths are stored relative to the project file's directory when the asset is
under it, absolute otherwise; `root` is set so MLT resolves relative
resources.

### Versioning

`<property name="ustudio:format_version">N</property>` on the sequence
tractor, plus `ustudio:saved_by` (the app version that wrote the file, from
0.80.3 on), so a build that can't open it can say which update it needs.

**The policy** (owner P0, 2026-10-08: "any changes to file formats is
versioned and handled gracefully"):

1. **Every change to what a project file means bumps the version.** Adding a
   record an older build would misread or silently drop is a change; so is
   renaming or reinterpreting one. A file is written at the *lowest* version
   that holds its content: the base version, raised by
   `xml_detail::requireFormatVersion()` only when it contains something
   newer (format 7: transform keyframes, project-relative effect files). An
   ordinary project stays readable by older builds.
2. **Every older version still opens.** The reader accepts
   `oldestReadableProjectFormat()` to `newestReadableProjectFormat()`
   (`core/xml/reader.h`). So far each bump only added records, so the reader
   reads every version directly; a bump that changes existing records adds
   its migration there, in code.
3. **One fixture per version, from that version's own writer.**
   `tests/core/data/formatN.ustudio`, made by building the last commit that
   writes N and saving a representative project
   (`tests/core/data/generate_format_fixture.cpp.txt`). The core test "every
   past project format still opens" loads each one, checks its content and
   re-saves it. A new version adds its fixture in the same landing.
4. **A newer file is refused, never guessed at**, with a dialog that names
   the version that saved it ("Saved by a newer U Stu … update this one").
   The file is left untouched.
5. **Data this build doesn't understand is kept.** An effect from an add-on
   that isn't installed, or a transition style this build can't play, stays
   in the model and is written back on save (it plays as nothing, or as a
   plain dissolve, meanwhile).
6. **Every failure to open is logged** (`[app] couldn't open a project: <path>:
   <reason>`), and the user sees a dialog for its kind: not found (with a
   pointer to `.ustudio-backups`), saved by a newer version, too old, not a
   U Stu project (including the first prototype's INI files, which this
   version doesn't open), unreadable, or damaged.

History: 3 adds `ustudio:position` and transitions; 4 writes each track
twice (a render playlist the tractor plays, identical to EngineSync's graph,
and a record playlist holding the model), so `melt` plays a saved project
exactly; 5 adds effects as `<filter>`s, clip source parameters, transition
recipes, adjustment blocks and looks (IP2, doc 15); 6 adds clip transforms
(ADR-018); 7 is written only for transform keyframes and project-relative
effect files.

## Save semantics

- Atomic: write to `<name>.ustudio.tmp` in the same directory, `fsync`,
  `rename` over the target.
- The `Document` tracks `isClean()` via the undo stack's clean point; the
  window title shows `•` when dirty; closing prompts (AdwAlertDialog).
- "Save As" re-relativises asset paths against the new location.

## Autosave and recovery

- Timer: 2 minutes after the last command while dirty, and on focus loss.
  Also whenever the oldest unsaved edit is about to be 2 minutes old, so
  steady editing (never idle for 2 minutes) still autosaves. Doc 12's M1
  box is "kill -9 loses at most 2 minutes" (`autosave::autosaveDue()`,
  2026-09-24).
- Location: `$XDG_STATE_HOME/ustudio/autosave/<sha1(path or 'untitled-'+uuid)>.ustudio`
  plus a `.meta` JSON with the original path and timestamp.
- On startup, autosaves newer than their targets (or with no target) are
  offered for recovery in a dialog; discarded ones are deleted.
- Autosave never writes to the user's file.

## Edit journal (debug/testing aid, optional at runtime)

When `USTUDIO_JOURNAL=1`, every executed command's `record()` is appended as
one JSON line to `$XDG_STATE_HOME/ustudio/journal/<session>.jsonl`. The test
harness can replay a journal against a fixture project to reproduce a bug
report deterministically. This is the main reason commands are data
(doc 04).

## kdenlive import (best effort, M7)

> DEFERRED (owner decision, 2026-09-29): not planned; a Lightworks or
> interchange-format importer is under consideration instead
> ([doc 12](12-roadmap-and-milestones.md), M7 and Post-2.0). This section
> is kept as the design if kdenlive import is ever picked up again.

Reader path 2: if the root has `kdenlive:docproperties`, run the *kdenlive
importer*: walk the MLT graph rather than our properties.

Supported: profile, bin producers (`kdenlive:folderid` → folder,
`kdenlive:clipname` → name), tracks (`kdenlive:audio_track` flag, names), clip
entries (in/out/position), the `kdenlive:id` mapping, markers
(`kdenlive:markers` JSON), simple effects whose MLT service is in our
catalogue (params copied through), `mix`-style dissolves (`luma`).

Dropped with a warning list shown after import: sequences other than the
main one, subtitles, groups, guides beyond markers, effects not in our
catalogue (kept as *opaque* effects: shown as "kdenlive: <service>", not
editable, still rendered since MLT knows the service, if the module is
present and Qt-free), `qtblend` compositing (replaced by `composite`),
speed changes (clip flattened at speed 1 with a warning).

Never write `.kdenlive` files.

## Settings

`GSettings` schema `com.ustudio.VideoEditor` (`data/*.gschema.xml`) for
preferences. Today it has four keys: `autosave-delay-minutes`,
`default-preview-scale`, `shuttle-max-speed` and `recent-projects-max`
(`GtkRecentManager` holds the list itself). Planned: audio backend,
real-time drop, thumbnail interval, default still duration, last export
preset. No project-level data in GSettings.
