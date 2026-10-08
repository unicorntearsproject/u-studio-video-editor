#pragma once

#include "core/model/model.h"

#include <expected>
#include <string>

namespace ustudio::core {

// Why a project didn't open: the kind decides what the app tells the user
// (a newer file asks for an update; a missing one says it's missing), and
// `message` is the full reason for the log.
struct ProjectLoadError
{
    enum class Kind
    {
        Missing,     // no file at the path
        Unreadable,  // there, but can't be read (permissions, not XML)
        NotAProject, // readable, but not a U-Stu project
        TooNew,      // a format newer than this build reads
        TooOld,      // a format older than any this build migrates
        Invalid,     // a U-Stu project whose content doesn't hold together
    };
    Kind kind = Kind::Invalid;
    std::string message;
    int formatVersion = 0; // TooNew/TooOld: the file's
    std::string savedBy;   // the "ustudio:saved_by" of the file, when it has one ("0.81.0-beta.1")
};

// Reads a .ustudio project file written by saveProject() (doc 09). Looks only at ustudio:* properties and
// ids to re-derive the model; the MLT-facing structure is regenerated on
// save, so it's output, not input, here. Every format version from
// kOldestReadableProjectFormat to kNewestReadableProjectFormat opens (doc
// 09, "Versioning"); newer ones are refused as TooNew, never guessed at.
std::expected<Model, ProjectLoadError> loadProjectFile(const std::string &path);

// The same, with the error as its message (most callers only need that).
std::expected<Model, std::string> loadProject(const std::string &path);

// The project format versions this build reads (doc 09, "Versioning").
int oldestReadableProjectFormat();
int newestReadableProjectFormat();

} // namespace ustudio::core
