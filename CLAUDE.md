---
applyTo: "**"
---

# u Studio Video Editor — Agent & coding standards

This file governs all AI-assisted development on `u-studio-video-editor`.
Read it in full before writing or reviewing any code.

**`docs/plans/v2/` is the architecture record.** Its ADRs
(`docs/plans/v2/adr/`) are the contract: a change that contradicts an ADR
either supersedes it with a new ADR or does not land. The numbered docs are
guidance and may drift; the milestone acceptance criteria in
`docs/plans/v2/12-roadmap-and-milestones.md` define "done". Decisions already
taken on the open questions are recorded in
`docs/plans/v2/13-risks-and-open-questions.md` — do not re-litigate them.

---

## Project identity

**u-studio-video-editor** is a from-scratch, GNOME-native, multi-track video
editor for Linux: GTK4 + libadwaita for the UI, MLT (via `mlt++`) for
playback and rendering, C++ built with meson/ninja. Licence
**MIT** (`LICENSE`; owner decision 2026-09-27: the most permissive licence
that fits). Binary packages bundle third-party code under its own licence
(FFmpeg with x264 is GPL, so a Flatpak/Snap as a whole carries GPL
obligations: ship those licence texts and the corresponding source). New
dependencies must be MIT-compatible to link, and anything copyleft is only
ever bundled, never copied into `src/`.

It is **not** a port of kdenlive. `~/Repos/kdenlive` is a current checkout
kept as a *reference for MLT usage patterns* (consumer setup, track/transition
wiring, playlist quirks). Learn from it; never copy code from it. The point of
this project is a different, smaller architecture.

**No Qt, no KDE Frameworks, anywhere in the process.** This is a runtime
property, not a link-line property: `Mlt::Factory::init()` with no directory
argument dlopens MLT's Qt6 modules (measured: 32 Qt libraries mapped).
`ldd | grep -i qt` passing proves nothing. The fix is a curated module
directory (ADR-007), implemented in `src/engine/factory_policy.cpp` and
checked by `tests/engine/test_factory_policy`. Never add anything that
depends on a `qt6` MLT service (`qtblend`, `qtext`, `qimage`,
`glaxnimate`); watch for Qt-free services that *default* to one (e.g.
`mask_apply`'s `transition` defaults to `qtblend`, doc 15). Never add Qt
to `meson.build`.

Platform: Fedora (owner's machine, Wayland, PipeWire) first; any modern
GNOME desktop second; Flatpak is the reference shipped artifact from M7.
**Windows 10/11 is a secondary launch target** (ADR-017, 2026-09-25): the
port itself is scheduled later, but all new code must stay portable. No
POSIX/Linux-only calls (`/proc`, `unistd.h`, signals, symlinks, `.so`
names, hard-coded `/tmp` paths) outside `src/platform/`, and move existing
ones there as a small side commit whenever you touch the file anyway (doc
14's migration list). macOS is not a target.

The owner's use case: 1080p/4K livestream and promo editing for the Unicorn
Tears brand. The design system at
`~/projects/unicorn-tears/claude-design-system` is the visual source of
truth (see "Design system" below).

---

## Repository layout

The v2 layered layout (M0 landed; see `docs/plans/v2/02-architecture.md`):

```
meson.build               Project definition, version, dependencies
src/core/                 Pure C++23 + libxml2. No GTK, no GLib, no MLT
  model/                  Project/Sequence/Track/Clip/Asset types, Model mutators, check()
  commands/               Command, primitives, CompositeCommand, Transaction, UndoStack
  xml/                    .ustudio project reader/writer (ADR-004)
  log.{h,cpp}             Thread-safe logger → stderr + $XDG_STATE_HOME/ustudio/logs/
src/engine/               The ONLY code that includes <mlt++/Mlt.h>
  factory_policy.*        Curated MLT module directory, Factory::init/close (ADR-007)
  engine.*                engine::Engine: the engine thread and its main-thread façade (doc 19 MT2)
  engine_sync.*           Model → Mlt::Tractor projection, verifier, media probe, render
  playback_controller.*   Playback over an MLT consumer (ADR-002, doc 05)
  dispatcher.*            MainThreadDispatcher: consumer thread → GLib main thread
  waveform_cache.*, thumbnail_cache.*   Workers with their own throwaway producers
src/app/                  GTK4/libadwaita shell, built imperatively (no .ui files)
  app_window.*            Header bar, preview, timeline, transport, media browser, dialogs
  action_registry.*       Every window action and its default shortcut (one table)
  ui_hints.*              Tooltip and Help text for every control; drop-ins register theirs
  timeline/               Viewport, TimelineController, UsTimelineView widget, renderer (doc 06)
  autosave.*              Autosave and crash recovery (doc 09)
  style/style.css         Unicorn Tears tokens on libadwaita named colours (GResource)
src/render/               u-studio-render headless CLI (placeholder until M6)
src/platform/             (ADR-017) every OS-specific call behind std-only
                          interfaces, one file per OS; std + OS headers only
tests/                    doctest suites: core/, engine/, app/
tools/                    Build-time scripts (gen_tokens.py: style.css -> tokens.h)
data/                     Desktop file, metainfo, icons, GResource manifest
drop-ins/                 (planned, ADR-013) one self-contained folder per drop-in
                          (effects, titles): own code, data, tests, meson.build.
                          src/ never includes from drop-ins/
docs/plans/v2/            v2 architecture, model, roadmap, ADRs (see top of this file)
builddir/                 meson build output — gitignored, per-worktree
```

---

## Language & build standards

- **C++23** (doc 13, Q1), set in `meson.build`; `std::expected` is in use
  (`core/xml/reader.h`).
- Toolchain: GCC 16 on the dev machine; keep it buildable with GCC ≥ 13.
  `ccache` is on the PATH — rebuilds are cheap, don't skip them.
- `warning_level=3` plus `-Wshadow -Wconversion -Wold-style-cast
  -Wnon-virtual-dtor` (`meson.build`); `-Dwerror=true` is the CI setting.
  New code must be warning-clean.
- Build, run, verify:

  ```sh
  meson setup builddir                       # once per worktree
  meson compile -C builddir
  USTUDIO_LOG_LEVEL=debug ./builddir/src/app/u-studio-video-editor
  ```

  Run `meson test -C builddir --print-errorlogs` before every commit.
- Dependencies are: GTK4 ≥ 4.10, libadwaita, GLib/GIO/GObject, MLT 7
  (`mlt-framework-7`, `mlt++-7`), `libxml2`, and doctest for tests
  (ADR-010). `egl` for the GPU context, linked only by `src/platform/`
  (ADR-019); MLT's `movit` module (movit, FFTW) is bundled in packages and
  loaded by MLT, never included by our code (ADR-019). `libarchive` in the
  titles drop-in only, and libsoup 3, json-glib and libsecret in the
  `u-studio-share` helper only (ADR-020). ThorVG (Lottie layers) in the
  titles drop-in only, behind our own validator (ADR-021). `frei0r-plugins` is a
  dependency of the effects drop-in only,
  never of the core editor (ADR-011, ADR-014). **Anything else needs
  an ADR** and the owner's sign-off.

---

## Code style

Match the existing files; `.clang-format` at the repo root is the
authority.

- 4-space indent, braces on their own line for functions/classes, same line
  for control flow (as in `app_window.cpp`). 120-column soft limit.
- `PascalCase` types, `camelCase` functions and variables, `m_` members,
  `k` constants (`kAudioRate`), `snake_case.{h,cpp}` files, `#pragma once`.
- Namespaces: anonymous namespace for file-local helpers; `ustudio::core/
  engine/app/render`. No `using namespace` in headers.
- Ownership: `std::unique_ptr` by default; `std::shared_ptr` only for
  producers shared between cuts and for lifetime tokens; raw pointers are
  non-owning and never outlive the current main-loop iteration (an RAII
  wrapper for GObject pointers is planned, doc 14).
- No exceptions across GTK callback boundaries. Trampolines (`static`
  callbacks forwarding to a member function) are the only place that casts
  `gpointer`; keep them grouped at the bottom of the file under the banner
  comment, as now.
- Logging goes through `Log::{debug,info,warn,error}` (`src/core/log.h`).
  No `printf`/`std::cout`/`g_print` for diagnostics. Prefix messages with a
  bracketed subsystem (`"[engine] …"`, `"[timeline] …"`) so logs grep by
  layer.
- Comments explain *why* and record empirical MLT findings (see next
  section). Don't restate what the code says.

---

## Architecture boundaries

`src/app/`'s ban on MLT headers is enforced by meson (`app-boundary-check`);
the rest by review.

| Layer | May include | May not include |
|---|---|---|
| `src/core/` | std, libxml2 | GTK, GLib, MLT |
| `src/engine/` | core, `mlt++`, GLib (dispatch only) | GTK, libadwaita |
| `src/app/` | core, engine headers, GTK, libadwaita, GIO | any `<mlt…>` header |
| `src/render/` | core, engine | GTK |
| `src/platform/` | std, OS headers | GTK, GLib, MLT |
| everything else | `platform::` for OS-specific work | `/proc`, `unistd.h`, `sys/*`, signals, symlinks (ADR-017) |

- **Model → engine → screen, never backwards** (ADR-003). UI state comes
  from `core::Model`, never from MLT objects.
- **Every user-visible edit is a `core::Command`** run through `UndoStack`
  (doc 04). `apply()` validates everything before mutating and leaves the
  model untouched when it returns false.

### Threading rules

- GTK widgets are touched from the main thread only. Cross from a worker via
  `MainThreadDispatcher` (or `g_idle_add` in the worker caches). If you are
  holding a `GtkWidget*` on a worker thread, that is the bug.
- The main thread never touches an MLT object. `EngineSync` (graph
  builds, master producers, the tractor) and `PlaybackController` (the
  consumer) live on one engine thread behind `engine::Engine` (ADR-016,
  doc 19 MT2). `src/app/` talks only to that façade: it publishes
  `Model::snapshot()`s and sends transport commands, and reads position,
  length and fps from its main-thread mirror, never waiting on the engine
  thread except in `Engine::shutdown()`. The engine never sees the live
  Model, only immutable snapshots.
- There is no project-wide MLT mutex. The live tractor is built and replaced
  on the engine thread (`EngineSync::rebuildAll()` → `PlaybackController::
  setTractor()`, which stops the consumer before dropping the old tractor).
  The consumer's thread enters our code only in `handleFrameShow()`, which
  copies the frame and posts the drain to the engine thread; the frame
  reaches the main thread from there. Work that must not contend with
  playback (waveforms, thumbnails, probing, render) opens its own throwaway
  `Mlt::Profile`/`Producer` — keep that pattern.
- MLT producers are not shared across threads. A worker owns its own
  producer; the live tractor belongs to the engine thread alone.
- Blocking work leaves the main thread: pool jobs (`core::concurrency::
  ThreadPool`) for probing, loading and saving, the engine thread for
  graphs and playback. The stall monitor (`USTUDIO_LOG_LEVEL=debug`) logs
  any main-loop iteration over 16 ms; a new stall with our code in it is a
  bug.
- Never destroy an MLT service a running consumer/pull loop can still reach.
  Stop first, then tear down; `Mlt::Factory::close()` is last, once, after
  `Engine::shutdown()` has joined the engine thread and the worker pool is
  drained.

### MLT empirical-knowledge rule

MLT's documentation is thin and several return values are unreliable. The
team's practice is the rule:

- **Don't trust a return code MLT documents as unreliable** (`split_at`,
  `resize_clip`, `insert_at`). Verify by inspecting state afterwards.
- **Don't guess service or property names.** Check the module's YAML
  metadata (`Mlt::Repository::metadata()`, or the `.yml` files under
  `/usr/share/mlt-7/`) — the `avformat` consumer takes `vcodec`/`ab`/`vb`,
  not ffmpeg CLI flags. Say so in a comment when a name was verified that way.
- **Reproduce before relying.** Anything surprising (cut producers report
  `resource="<producer>"`, `get_frame()` auto-advances, `mix` needs
  `start=1 sum=1`, XML round-trip yields a non-tractor) was confirmed with a
  standalone repro before being built on. Keep doing that, and write the
  finding into `docs/developer/notes/` (the implementation notes) or the
  engine comment where it applies, so the next agent doesn't rediscover it.
- The editor needs these MLT modules, and every package build must include
  them: `core` (`composite`, `luma`, `mix`, `crop`, `mirror`, the `colour`
  producer),
  **`plus`** (the `affine` filter used for clip transforms, ADR-018 — it is
  *not* in `core`), `normalize` (`volume`), `avformat`, `xml`, `sdl2`
  (`sdl2_audio`), `rtaudio`, `gdk` (stills), `resample`, **`xine`** (the
  loader's `deinterlace` normaliser; without it MLT falls back to
  `avdeinterlace`, which converts every frame to BT.601 limited YUV, and the
  GPU probe fails on the shifted colour, ADR-019 G5). `null` is core.
  Check a service's module in `/usr/share/mlt-7/<module>/` before assuming
  where it lives (the Flatpak's first build left out `plus` because this
  line used to call `affine` core). `frei0r` is required by the effects
  drop-in, **not** by the core editor (ADR-011 as narrowed by ADR-014,
  2026-09-24): nothing in `src/` may depend on a frei0r service. The core
  composites every video track onto track 0 (the background) with
  `composite` (`fill=1`), never chained track to track (a chain loses an
  upper clip's alpha), and realises clip transforms (ADR-018) with the
  `crop` and `mirror` filters and the plus module's `affine` filter on
  each cut; an `affine` track compositor is too slow (`docs/developer/notes/
  engine-sync.md`). Effects, titles and drop-in plans: `docs/plans/v2/15-*` to
  `18-*`.

---

## Design system

`src/app/style/style.css` (compiled into the GResource) maps the Unicorn Tears tokens (ink scale, magenta /
cyan / violet, semantic colours) onto libadwaita named colours so the whole
shell reskins from one place.

- Add CSS classes, not per-widget style overrides. New colours go in as
  tokens next to the existing `@define-color` lines.
- Glow is a *selection/focus* treatment only; this app is looked at for
  hours. No glowing static chrome, no animated decoration in the editor.
- The timeline (clips, playhead, ruler) is drawn in C++, where CSS can't
  reach, but takes its colours from the same tokens: `tools/gen_tokens.py`
  turns `style.css`'s `@define-color` lines into a generated `tokens.h`
  (`tokens::kBrandCyan`, ...). Add a colour as a token in `style.css`,
  never as hex in C++.
- Fonts (Space Grotesk, JetBrains Mono, Anton) are not bundled and may not be
  installed; every rule must keep its generic fallback.
- Icons: symbolic GNOME icon names (`media-playback-start-symbolic`), never
  raster art in the chrome.

---

## Testing discipline

Tests are doctest suites under `tests/` (`core/`, `engine/`, `app/`), run
with `meson test`.

- Before claiming a change works, **build it and exercise it**: run the app
  for UI changes, or a standalone repro for engine changes (the render and
  playlist primitives were each validated that way before wiring in). Report
  what you ran and what you saw.
- Behaviour changes ship with a test in the same change when practical.
- **Tests never touch the desktop** (owner rule, 2026-10-08). `meson test`
  (and `just test/asan/tsan`) runs every test through
  `tools/test-headless.sh`, the default test setup: a private Xvfb and D-Bus
  session per test, no Wayland display, in-memory GSettings. Never bypass
  it, and never launch the app on the owner's session to check something:
  use a private Xvfb inside `dbus-run-session` (`docs/developer/testing.md`).
- If any test fails, report the exact test names and output and stop
  claiming success until it is resolved or the owner explicitly defers it.
- **Never implement production code solely to make a pre-existing failing
  test pass.** If you find red tests you didn't write and don't fully
  understand, stop and ask — they may encode a planned API.
- **No binary media in the repo.** Tests generate their inputs with MLT
  generators (`color:`, `noise:`, `tone:`). The owner's real footage is never
  committed, never moved, never overwritten. Renders during verification go
  to a path you name under `/tmp` or the scratchpad, not next to source
  media.
- Sanitiser builds (`-Db_sanitize=address,undefined`) are the standard for
  anything touching threads or MLT lifetime.
- Sanitizer runs are tiered (owner, 2026-09-25). The full suites take
  about 6½ min (`just asan`) and 4½ min (`just tsan`) on the dev machine,
  so they are not run for every landing:
  - **Most thread-touching changes** (pool jobs, worker caches, queues,
    anything that posts across threads): before landing, run the tests
    that exercise the changed code under both sanitizers, e.g.
    `just asan engine-thread` / `just tsan engine-thread` (the recipes take
    meson test names), and report the pass counts.
  - **The full suites, before landing,** only for changes to the engine
    thread (`engine::Engine`), `core/concurrency/`, the dispatcher, or
    `PlaybackController` / MLT object lifetime.
  - **Otherwise, the full suites once per batch** of such changes or at the
    end of the working day, whichever comes first; report the counts then.
  - Run ASan and TSan concurrently, each in its own subshell with its own
    `cd`. Docs, UI-only and pure-core changes need only `meson test`.

---

## Isolated agent worktrees (one checkout per agent)

Several agents and the owner work on this repo at the same time; files under
`src/` changed underneath a review session within minutes on 2026-09-12. A
shared working tree means one agent's build, hook, or stash sees another's
half-written files. So each agent session works in its own git worktree:

- **Use Claude Code's worktree isolation** (`EnterWorktree`, or
  `isolation: "worktree"` when spawning subagents). It creates a worktree
  under `.claude/worktrees/<name>` (gitignored) on its own branch. Manual
  equivalent: `git worktree add ../u-studio-video-editor.worktrees/<name>
  -b agent/<name>`.
- **The main checkout (`~/Repos/u-studio-video-editor`) stays on `main` and
  only ever `git pull --ff-only`s.** It is where the owner runs the app and
  where `builddir/` for the live binary lives. An agent must not edit,
  stage, stash, or commit files there.
- Each worktree needs its own `meson setup builddir` (gitignored; ccache
  makes the first compile cheap). Don't point a worktree at another tree's
  `builddir`.
- Branch names: `agent/<name>` for agent seats, `feature/<name>`,
  `fix/<name>`, `docs/<name>` for owner-directed work.
- **Landing:** commit on your branch, build and test there, then
  `git push origin HEAD:main` (a fast-forward). If rejected as
  non-fast-forward: `git fetch origin && git merge origin/main` (a merge
  commit is fine), rebuild, retest, push again. Never rebase, cherry-pick,
  or force. A pull request is the alternative when the owner asks for review.
- **Clean up after yourself:** once your branch is landed and the owner's
  checkout is fast-forwarded, remove your own worktree (`git worktree
  remove`, `--force` only for experiments you mean to discard) and delete
  its merged branch (`git branch -d`). No permission needed for your own.
- Never `git worktree remove`/`prune` another agent's tree and never pop,
  apply, or drop a stash you did not create. Report a stray worktree or stash
  to the owner instead.
- A session still running in the main checkout is exposed to the
  concurrent-edit hazard. Say so when it bites rather than retrying blind.

---

## Sub-agent usage

**Don't use sub-agents** (owner rule, 2026-09-27): no `Agent`/`Task` tool,
no `Explore` agents, no `Workflow` orchestration. Do the work in your own
session.

- Long-running work (builds, test and sanitizer suites, soaks, renders,
  demo recordings) runs as a **background process** in your session
  (`run_in_background`), and you carry on and report when it finishes.
  That is allowed and preferred to blocking the turn.
- Work that needs another pair of hands goes to another team's session,
  through the VE Strategist, not to a sub-agent.

---

## Git conventions

- Commit messages: imperative subject ≤ 72 chars, blank line, body bullets
  explaining *why* and what was verified (match the existing history, e.g.
  `Add clip lift/close-gap editing, waveforms, and format-matched render`).
  End with the attribution trailer the tooling provides.
- Commit after each substantial, built-and-verified change; no monolithic
  commits, and no PR that both moves files and changes logic.
- Never commit `builddir/`, `logs/`, `.claude/`, media files, or renders.
  `.gitignore` already covers the first three; check before adding new
  output directories.
- Versioning: `version:` in `meson.build` is the single source of truth,
  SemVer 2.0.0, pre-1.0 (never bump MAJOR; user-facing feature bumps MINOR,
  fix bumps PATCH). Bump it in the same commit as the user-facing change
  and add a one-line, newest-first entry to `CHANGELOG.md` (create it on
  the first bump). Internal refactors, tests, docs: no bump.
- Release notes: only a **releasable build** (a tagged beta or release that
  testers or users will install) gets an entry in the `<releases>` block of
  `data/com.ustudio.VideoEditor.metainfo.xml`. That block feeds Help ›
  Release notes and software centres, so write it for testers: what's new,
  what to try, known issues. Every past entry is kept. Everyday version
  bumps go in `CHANGELOG.md` only.

### Git history safety (hard stop)

- Never intentionally detach `HEAD`, and never work in a detached state.
- Never run `git rebase`, `git cherry-pick`, or any force-push variant
  (`--force`, `--force-with-lease`, or equivalent).
- Never rewrite branch history; never check out a different branch in the
  shared main checkout.
- `--no-verify` is never an agent's call; it needs the owner's explicit word
  for that specific commit, and an approval given once does not carry over.
- If an operation would require any of the above, stop and report: the
  exact blocker, the affected branch, and the safe alternatives, then wait
  for explicit instruction.

### Git hook discipline

No hooks are configured yet (CI runs `clang-format` and the tests, but is
parked on manual dispatch; a pre-commit hook may follow). When hooks exist: warnings and errors are
immediate action items — fix, rerun, then push. Run hooks only inside your
own worktree.

---

## Documentation SOP

Docs are part of "done" (owner rule, 2026-09-27, after the docs team's
rewrite): a change that alters what a user sees, how the code is built or
structured, or what we know about MLT/GTK is **not finished** until its docs
are updated in the same commit or the same landing batch. Reviewers treat a
missing doc update like a missing test.

**Checklist for every landing:**

1. **User-visible change?** Update the matching `docs/user/` page (the
   feature's own page, plus `keyboard-shortcuts.md` for new actions and
   `troubleshooting.md` for new failure modes), and README's feature or
   "Not yet" bullets if the list changed.
2. **New action, control or setting?** Its `ui_hints` text (tooltip and
   Help), and the user page that explains it, in the same commit.
3. **Build, packaging, test or architecture change?** Update
   `docs/developer/` (`building.md`, `testing.md`, `architecture.md`,
   `packaging.md`) and, if a design decision changed, the v2 doc or a new
   ADR.
4. **Found out how MLT, GTK or GLib really behaves?** Write it into the
   matching `docs/developer/notes/` file (one file per area), with the
   repro or source reference.
5. **New doc file?** Link it from its section index (`docs/user/README.md`,
   `docs/developer/README.md`, `docs/developer/notes/README.md`, ...) and,
   if it's a new area, from `docs/README.md`. Nothing may be unreachable
   from `README.md`.
6. **Releasable build?** A `<releases>` entry in the metainfo (see Git
   conventions); everyday changes stay in `CHANGELOG.md`.
7. Say in the commit body which docs you updated, or "docs: none needed"
   and why.

**Style and structure** (keep the docs team's pattern):

- `README.md` is the entry point and must stay honest and short: a tl;dr,
  install, feature bullets, roadmap bullets, links. When you land a
  capability or remove a limitation, update its "Features" / "Not yet"
  bullets and the matching `docs/user/` page in the same change.
- `docs/README.md` indexes everything. Every doc must be reachable by
  browsing from `README.md`: link a new doc from its section's index
  (`docs/user/`, `docs/developer/`, `docs/developer/notes/`,
  `docs/audit/`, `docs/plans/v2/`).
- Empirical MLT/GTK/GLib findings live in `docs/developer/notes/` (one
  file per area) until v2's `engine/` comments and tests absorb them.
- `docs/plans/v2/` is the design record. When code makes a v2 doc wrong,
  fix the doc in the same PR. Disagree in place with a `> REVIEW:`
  blockquote (name + one sentence) rather than forking a doc. Superseding an
  ADR means a new ADR plus marking the old one "Superseded by".
- Root-level markdown is reserved for entry docs (`README.md`, `CLAUDE.md`,
  `CHANGELOG.md`). Planning, audits, and debug notes go under `docs/`.
- Don't create new `.md` files for routine code-only changes; use comments
  and commit bodies. Documentation tasks are the exception.
- Headings in sentence case; tables for reference data; fenced code blocks
  with a language tag; backticks for paths, flags, functions, env vars.
- Write `docs/user/` for users: plain language, what to click or press,
  no internal names (no class names, ADR numbers or MLT service names).
  Developer detail belongs in `docs/developer/`.
- One home per fact: link to the page that owns a topic instead of
  repeating it. When something changes, fix it where it lives and delete
  stale copies rather than adding a second version.

---

## Agent autonomy — tool execution

No permission needed for:

- Read-only shell (grep, find, ls, cat, diff, `git status/log/diff/show`,
  `pkg-config`, `ldd`, `ffprobe`, `nm`, `objdump`).
- `meson setup`, `meson compile`, `meson test`, `ninja`, `clang-format
  --dry-run`, compiling a standalone repro in the scratchpad against MLT.
- Launching the app from **your own worktree's** `builddir` to verify a
  change, **headless only** (owner rule, 2026-10-08): a private Xvfb display
  inside a private `dbus-run-session` (`DISPLAY` set, `GDK_BACKEND=x11`,
  `WAYLAND_DISPLAY` unset, `GDK_DEBUG=no-portals`, the portal-safe runtime
  recipe in `docs/developer/testing.md`; Flatpak runs add
  `--nosocket=wayland --socket=x11`). Nothing an agent starts may appear on
  the owner's real desktop unless the owner asks for it explicitly. Close or
  kill what you started; never leave instances running.
- Reading the reference checkouts (`~/Repos/kdenlive`, the design system).
- Installing, with `sudo dnf`, the Fedora packages that approved work needs
  (the `-devel` headers of a dependency an accepted ADR allows, a test or
  packaging tool). Say in your report what you installed (owner,
  2026-09-28).
- Deleting your own stray or merged branches and worktrees, `git branch -D`
  included (owner, 2026-09-28).

Permission **is required** before:

- Anything destructive or irreversible: `rm -rf` outside `builddir/` and
  the scratchpad, `git reset --hard`, `git clean`, `git branch -D` on
  anyone else's branch, removing another agent's worktree, overwriting or moving the owner's media
  or project files.
- Removing system packages, installing one that no approved work needs, or
  adding a `meson.build` dependency without an accepted ADR.
- Pushing to any branch other than your own `agent/<name>` branch or the
  fast-forward landing on `main` described above; creating or modifying
  GitHub repos, releases, or workflows.
- Killing processes you did not start (the owner may have the live editor
  open from the main checkout).

---

## What the agent should NOT do

- Don't refactor working code unless the task asks for it; targeted,
  minimal diffs. Larger reworks belong to a planned milestone with its own
  design (doc 12); a partial rewrite in between costs more than it saves.
- Don't add `.ui` files for the shell; imperative construction is
  deliberate (v2 allows `.ui` for static dialogs only). GResource carries
  the stylesheet.
- Don't add error handling for situations that cannot occur, and don't
  wrap MLT calls in try/catch — `mlt++` does not throw.
- Don't introduce new dependencies, third-party headers, or build systems
  (CMake, autotools) without an ADR and the owner's sign-off. No Qt, KDE
  Frameworks, GStreamer, or direct SDL/PulseAudio use outside what MLT's
  consumer does internally.
- Don't guess MLT property names, profile names, codec strings, or GTK4 API
  shapes from memory — verify against installed metadata/headers, and say
  when you couldn't.
- Don't add sleeps or timers to "fix" A/V timing: pacing belongs to the
  MLT consumer (ADR-002, implemented in `PlaybackController`). That does
  not freeze `PlaybackController`: seek, pause and loop accuracy bugs in it
  are ordinary bugs, fixed there against doc 05 and M2's acceptance
  criteria.
- Don't leave debug logging at `info` level; use `debug`.
- Don't run or edit anything in the sibling `*-stable` or `*.bak-*`
  repositories under `~/Repos`.
- **GitHub Actions CI (`.github/workflows/ci.yml`) is parked, manual
  dispatch only** — every run since it was introduced failed on
  CI-environment gaps (no real audio device in the container, further
  codec/signal-handling differences from a local build), not real
  regressions, confirmed by the same commits building and testing clean
  locally every time (2026-09-23). Don't add `push`/`pull_request` (or
  any other automatic) triggers back, don't add new workflows, and don't
  otherwise make anything in this repo depend on CI running automatically
  — until the owner has fixed the environment gap and explicitly says to
  re-enable it.

---

## Preferred libraries

| Purpose | Library | Notes |
|---|---|---|
| UI toolkit | GTK 4.22 + libadwaita 1.9 | C API from C++; RAII wrapper for refs planned (doc 14) |
| Media engine | MLT 7.40 via `mlt++` | The only media engine. Only `src/engine/` includes it |
| Main loop / IO / settings | GLib, GIO, GSettings | `MainThreadDispatcher` for consumer-thread hops |
| Drawing | GSK snapshot (v2), cairo (current timeline) | Custom widgets, not a widget per clip (ADR-008) |
| Project files | MLT XML + libxml2 (ADR-004) | |
| Audio output | MLT `sdl2_audio` consumer, `rtaudio`/`null` fallbacks (ADR-002) | |
| Tests | doctest, vendored (ADR-010) | |
| Formatting | clang-format (`.clang-format`) | |

Do **not** use: Qt, KDE Frameworks, GStreamer/GES, gtkmm (the codebase uses
the C API deliberately), Boost, CMake, or any GUI framework other than GTK4.

---

## Security & data safety

- The editor (and `u-studio-titles`, which ships with it) makes no
  network requests. Keep it that way; nothing in it phones home, checks
  versions, or fetches fonts. The only network code lives in separate
  helper apps with their own Flatpak ids, and runs only on an explicit
  user action: AI generation (`u-studio-generate`, ADR-015) and template
  sharing (`u-studio-share`, ADR-020).
- Template packages are untrusted archives: validate every entry before
  writing anything (doc 20), and never execute anything from one.
- Project files and imported media are **untrusted input**: they name
  arbitrary resource paths that MLT will open. Never execute, `system()`, or
  shell-interpolate anything read from a project file or media metadata.
- Never overwrite a user's source media. Renders and proxies are written to
  an explicitly chosen output path, atomically (temp file + rename).
- Logs go to `$XDG_STATE_HOME/ustudio/logs/`; never log full file
  contents or environment dumps.
