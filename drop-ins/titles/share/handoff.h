#pragma once

// Handing a downloaded pack to U-Stu (ADR-020, the pattern of ADR-015):
// the editor's `install-template-pack` action over its session bus name
// when the editor is running (it installs and says so in its status bar),
// else installed here straight into the user's template library, the same
// one the editor and U-Stu Titles read. A running editor is never started
// by this: only its name's owner is asked.

#include "package/pack.h"

#include <expected>
#include <string>

namespace ustudio::titles::share {

constexpr const char *kEditorBusName = "com.ustudio.VideoEditor";

// What happened, for the user, or why not. `library` and `editor` are the
// real ones but for tests (which must reach neither a running editor nor
// the user's library).
std::expected<std::string, std::string> handOff(const std::string &packPath,
                                                const std::string &library = pack::templatesLibrary(),
                                                const std::string &editor = kEditorBusName);

} // namespace ustudio::titles::share
