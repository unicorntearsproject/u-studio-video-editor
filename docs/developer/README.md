# Developer documentation

[Docs home](../README.md) › Developer docs

U-Stu is C++23 on GTK4 + libadwaita and MLT 7, built with meson. Start
with [Building](building.md), then read [Architecture](architecture.md)
before changing code.

| Page | What's in it |
|---|---|
| [Building, running and testing](building.md) | Dependencies, `just` recipes, logging, the Qt check, sanitizers |
| [Architecture overview](architecture.md) | Layers, threading rules, a map of modules to design docs |
| [Testing](testing.md) | Test suites, what they need, the testing rules |
| [Contributing](contributing.md) | Worktrees, landing, commit and version rules, keeping docs current |
| [Packaging and releases](packaging.md) | The Flatpak, package checks and smoke test, Flathub and Snap preparation, drop-in builds, release notes |
| [Effects drop-in](../../drop-ins/effects/README.md) | The effects drop-in's layout, how an effect plays in MLT, plugin curation and the health scan, how to build and test it |
| [Titles drop-in](../../drop-ins/titles/README.md) | The titles drop-in's layout, how to build and test it, and the `.ustitle` format reference |
| [Implementation notes](notes/README.md) | Empirical MLT, GTK and GLib findings, by area. Read before touching that area |

## Design record

- [v2 planning docs](../plans/v2/README.md): architecture, project model,
  playback, timeline, effects, titles, concurrency, and the roadmap.
- [Architecture decision records](../plans/v2/adr/README.md): the binding
  contract. A change that contradicts an ADR supersedes it with a new one,
  or doesn't land.
- [Roadmap and milestones](../plans/v2/12-roadmap-and-milestones.md): what
  ships in what order, and the acceptance criteria that define "done".
- [Risks and open questions](../plans/v2/13-risks-and-open-questions.md):
  decisions already taken. Don't reopen them.
- [Audits](../audit/README.md): bug, sanitizer and MLT audits, with their
  repro scripts.

## Project rules

- [CLAUDE.md](../../CLAUDE.md): coding, testing, git and documentation
  standards for everyone, human or agent.
- [CHANGELOG.md](../../CHANGELOG.md): every user-facing change, newest
  first.
