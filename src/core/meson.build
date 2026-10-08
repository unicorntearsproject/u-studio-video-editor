libustudio_core_sources = files(
  'log.cpp',
  'audio/align.cpp',
  'concurrency/thread_pool.cpp',
  'model/model.cpp',
  'model/mlt_order.cpp',
  'model/track_segments.cpp',
  'model/profile_match.cpp',
  'model/retime.cpp',
  'model/animation.cpp',
  'model/effect_native.cpp',
  'model/transition_native.cpp',
  'model/transform.cpp',
  'commands/primitives.cpp',
  'commands/undo_stack.cpp',
  'commands/timeline_edits.cpp',
  'xml/writer.cpp',
  'xml/reader.cpp',
  'xml/backup.cpp',
  'xml/effect_io.cpp',
  'xml/release_notes.cpp',
  'render/render_profile.cpp',
  'media/fingerprint.cpp',
  'media/frame_hash.cpp',
  'media/image_sequence.cpp',
  'media/missing_media.cpp',
)

threads_dep = dependency('threads')

# The writer records which version saved a project (ustudio:saved_by), so a
# build too old to open it can say which update it needs. Shared with
# tests/core's second compile of these sources.
libustudio_core_args = ['-DUSTUDIO_VERSION="' + meson.project_version() + '"']

libustudio_core = static_library(
  'ustudio_core',
  libustudio_core_sources,
  dependencies: [libxml2_dep, threads_dep],
  include_directories: src_inc,
  cpp_args: libustudio_core_args,
  install: false,
)

# Consumers link against this, not against libustudio_core directly, so the
# include path (and, once core/ actually uses libxml2 from M1 onward) the
# dependency propagate automatically -- a plain static_library()'s own
# dependencies: do NOT auto-propagate to whatever links it;
# declare_dependency() is the mechanism that does.
libustudio_core_dep = declare_dependency(
  link_with: libustudio_core,
  include_directories: src_inc,
  dependencies: [libxml2_dep, threads_dep],
)
