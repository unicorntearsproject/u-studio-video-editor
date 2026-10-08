#pragma once

#include <string>

namespace ustudio::app::portal {

// A path from a file dialog, a drop, or the recent-projects list, resolved
// out of the xdg-document-portal's FUSE mount ($XDG_RUNTIME_DIR/doc/<id>/
// <name>) to the real file whenever the portal says where that is and it's
// reachable. Any other path is returned unchanged.
//
// The portal hands out those paths whenever it considers the caller
// confined (a launch from VS Code's snap terminal was enough, and Flatpak
// from M7 always will). They break anything that writes beside the file:
// the mount exposes only the one file, so Save's atomic temp + rename
// ("failed to write .../doc/<id>/<name>.ustudio.tmp", owner report
// 2026-09-23) and Render's ".part" had nowhere to go, and a project that
// stores the doc path references an id rather than the file. Verified
// against xdg-desktop-portal 1.22 with a Documents.Add repro (and
// tests/app/test_portal_path.cpp): the FUSE entry carries a
// "user.document-portal.host-path" xattr naming the real file. Falls back
// to the portal path when the host path isn't reachable, which is correct
// inside a genuine sandbox.
//
// `createIfMissing` (Save As, Render): a chooser path for a file that
// doesn't exist yet has no xattr to read, so this creates it through the
// portal first, which that portal entry permits for its own named file.
// Unverified: the file chooser registers these via Documents.AddNamed,
// which only the chooser backend may call, so it couldn't be reproduced
// directly.
std::string resolveHostPath(const std::string &path, bool createIfMissing = false);
// The same, against the portal mounted at `portalRoot` ("<dir>/doc/"):
// tests ask the portal where it is (a private one, under the headless test
// wrapper, isn't at $XDG_RUNTIME_DIR/doc).
std::string resolveHostPath(const std::string &path, bool createIfMissing, const std::string &portalRoot);

} // namespace ustudio::app::portal
