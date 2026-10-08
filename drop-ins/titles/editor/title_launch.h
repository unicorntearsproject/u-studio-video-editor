#pragma once

// Opening a title in U-Stu Titles from the editor (doc 16, "Editor
// integration"): a separate process, started with GSubprocess (portable,
// ADR-017), on the title's file, over the editor's current frame. The
// editor's file watch brings the saved result back.

#include <gdk/gdk.h>

#include <functional>
#include <string>

namespace ustudio::titles {

// The designer's executable: next to this program (installed together, and
// in the Flatpak's one bundle), else this build's, else from PATH. Empty if
// none is found.
std::string titlesAppPath();

// Starts the designer on `title` (empty: a new, untitled one) with
// `backdrop` (may be null) behind it,
// and with its template gallery open when `gallery` (New Title). Empty on
// success, else why not.
std::string launchTitlesApp(const std::string &title, GdkTexture *backdrop, bool gallery = false);

// Tests only: what launchTitlesApp() does instead (null: the real launch).
// While one is set, Edit Title skips rendering the backdrop.
using TitlesLauncher = std::function<std::string(const std::string &title, GdkTexture *backdrop, bool gallery)>;
void setTitlesLauncherForTesting(TitlesLauncher launcher);
bool titlesLauncherIsForTesting();

} // namespace ustudio::titles
