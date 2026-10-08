test_core_exe = executable(
  'test_core',
  files('test_log.cpp', 'test_model.cpp', 'test_commands.cpp', 'test_timeline_edits.cpp', 'test_xml.cpp',
        'test_audio_align.cpp', 'test_backup.cpp', 'test_render_profile.cpp', 'test_profile_match.cpp',
        'test_retime.cpp', 'test_import_batch.cpp', 'test_transform.cpp', 'test_release_notes.cpp', 'test_image_sequence.cpp',
        'test_transition_native.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  include_directories: include_directories('../common'),
  cpp_args: ['-DTEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"',
              '-DUSTUDIO_METAINFO_XML="' + meson.project_source_root() / 'data' / 'com.ustudio.VideoEditor.metainfo.xml' + '"']
            + libustudio_core_args,
  install: false,
)

# The 10k-iteration random-command property test is nearly all of this
# binary's time: 42.6 s of 44 on the dev machine alone (2026-09-27), and it
# hit the old shared 90 s timeout twice under concurrent load. It runs as
# its own test with a timeout sized for a loaded machine (about 3x), so the
# rest of the core suite (under 2 s) keeps a tight timeout that still
# catches a real hang.
kUndoFuzz = 'UndoStack property: random commands*'
test('core', test_core_exe, args: ['--test-case-exclude=' + kUndoFuzz], timeout: 30)
test('core-undo-fuzz', test_core_exe, args: ['--test-case=' + kUndoFuzz], timeout: 180)

# doc 19 MT0: the worker pool, its own binary so `just tsan` can run it on
# its own quickly.
test_thread_pool_exe = executable(
  'test_thread_pool',
  files('test_thread_pool.cpp'),
  dependencies: [doctest_dep, libustudio_core_dep],
  install: false,
)

test('core-thread-pool', test_thread_pool_exe, timeout: 60)

# doc 19 MT0: Model::snapshot() on a 5,000-clip project. A benchmark, not a
# test: `meson test -C builddir --benchmark core-snapshot` (or run it).
bench_snapshot_exe = executable(
  'bench_snapshot',
  files('bench_snapshot.cpp'),
  dependencies: [libustudio_core_dep],
  install: false,
)
benchmark('core-snapshot', bench_snapshot_exe)

# Model's release-build safety: core compiled again, here, with NDEBUG, so
# the precondition asserts are gone and the safe paths behind them run.
test_model_release_exe = executable(
  'test_model_release',
  files('test_model_release.cpp'),
  libustudio_core_sources,
  cpp_args: ['-DNDEBUG'] + libustudio_core_args,
  dependencies: [doctest_dep, libxml2_dep, threads_dep],
  include_directories: [src_inc, include_directories('../common')],
  install: false,
)
test('core-model-release', test_model_release_exe)
