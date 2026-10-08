titles_test_core = executable(
  'test_titles_core',
  files('test_core.cpp'),
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-core', titles_test_core)

# T5: subtitle files read into cues, placed on frames, imported as clips.
titles_test_captions = executable(
  'test_titles_captions',
  files('test_captions.cpp'),
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-captions', titles_test_captions)

# T6 (ADR-021): the Lottie check, frames, and the animated layer's format.
titles_test_lottie = executable(
  'test_titles_lottie',
  files('test_lottie.cpp'),
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-lottie', titles_test_lottie)

# Doc 16's absolute time for 1,000 cues: a benchmark, not a test, since a
# loaded machine fails any fixed limit (`meson test --benchmark`).
titles_bench_captions = executable(
  'bench_titles_captions',
  files('bench_captions.cpp'),
  dependencies: [titles_core_dep],
  install: false,
)
benchmark('titles-captions-bench', titles_bench_captions)

# T4b: the pack manifest, the validator against a corpus of malicious packs,
# install and remove.
titles_test_pack = executable(
  'test_titles_pack',
  files('test_pack.cpp'),
  dependencies: [titles_doctest_dep, titles_package_dep],
  install: false,
)
test('titles-pack', titles_test_pack)

titles_test_packs_app = executable(
  'test_titles_packs_app',
  files('test_packs_app.cpp', '../app/packs.cpp', '../app/gallery.cpp'),
  cpp_args: ['-DTITLES_TEMPLATES_DIR="' + meson.current_source_dir() / '..' / 'data' / 'templates' + '"',
             '-DTITLES_TEMPLATES_SOURCE_DIR="' + meson.current_source_dir() / '..' / 'data' / 'templates' + '"'],
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_render_dep, titles_package_dep, libustudio_platform_dep, gtk4_dep,
                 adwaita_dep],
  install: false,
)
test('titles-packs-app', titles_test_packs_app)

# The designer's window closing with a selection (a demo blocker, 2026-09-29).
titles_test_window = executable(
  'test_titles_window',
  files('test_titles_window.cpp'),
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_app_dep],
  install: false,
)
test('titles-window', titles_test_window, timeout: 120)

# The designer's headless --install-pack, as the packaging smoke test uses it.
test('titles-install-pack', find_program('sh'),
     args: [meson.current_source_dir() / 'install_pack.sh', titles_app_exe,
            meson.project_source_root() / 'tools' / 'make_test_pack.py',
            meson.current_build_dir() / 'install-pack-scratch'],
     depends: titles_app_exe)

titles_test_render = executable(
  'test_titles_render',
  files('test_render.cpp'),
  cpp_args: ['-DTITLES_TEST_FONT_DIR="' + meson.current_build_dir() / 'fonts' + '"',
             '-DTITLES_TEMPLATES_DIR="' + meson.current_source_dir() / '..' / 'data' / 'templates' + '"'],
  dependencies: [titles_doctest_dep, titles_render_dep],
  install: false,
)
test('titles-render', titles_test_render, timeout: 120)

# IP3/IP4/IP6: the producer module in real graphs, and u-studio-render's
# --title-frames (defined after drop-ins/, so named by path; `meson test`
# builds everything first).
titles_engine_args = ['-DTITLES_MLT_BUILD_DIR="' + titles_mlt_build_dir + '"',
                      '-DTITLES_TEMPLATES_DIR="' + meson.current_source_dir() / '..' / 'data' / 'templates' + '"']
# Stock melt, for "a baked title plays in stock melt" (doc 16 T4); skipped
# without it.
titles_melt = find_program('melt-7', 'melt', required: false)
if titles_melt.found()
  titles_engine_args += '-DTITLES_MELT="' + titles_melt.full_path() + '"'
endif
if get_option('render_tool')
  titles_engine_args += '-DTITLES_RENDER_TOOL="' + meson.project_build_root() / 'src' / 'render' / 'u-studio-render' + '"'
  if get_option('dropin_titles') == 'module'
    # An uninstalled render tool finds the module only through the
    # development override (ADR-014).
    titles_engine_args += '-DTITLES_DROPIN_MODULE_DIR="' + meson.current_build_dir() / '..' + '"'
  endif
endif
titles_test_engine = executable(
  'test_titles_engine',
  files('test_engine.cpp', '../engine/backdrop.cpp', '../engine/title_export.cpp', '../engine/title_extension.cpp',
        '../engine/title_frames.cpp'),
  cpp_args: titles_engine_args,
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_render_dep, libustudio_engine_dep, libustudio_platform_dep, glib_dep,
                 titles_cairo_dep],
  install: false,
)
test('titles-engine', titles_test_engine, depends: titles_mlt_module, timeout: 180)

# IP5: import and the file watch, with a fake ShellHost.
titles_test_shell = executable(
  'test_titles_shell',
  titles_editor_resources, files('test_shell.cpp', '../editor/jobs.cpp', '../editor/title_bake.cpp', '../editor/title_shell.cpp', '../editor/title_launch.cpp', '../editor/title_page.cpp', '../engine/backdrop.cpp',
        '../engine/title_export.cpp', '../engine/title_extension.cpp'),
  cpp_args: ['-DTITLES_APP_BUILD_PATH="' + titles_app_exe.full_path() + '"',
             '-DTITLES_APP_INSTALL_DIR="' + get_option('prefix') / get_option('bindir') + '"',
             '-DTITLES_MLT_BUILD_DIR="' + titles_mlt_build_dir + '"',
             '-DTITLES_TEMPLATES_INSTALL_DIR="/nonexistent"',
             '-DTITLES_TEMPLATES_SOURCE_DIR="' + meson.current_source_dir() / '..' / 'data' / 'templates' + '"'],
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_core_dep, titles_package_dep, libustudio_engine_dep,
                 libustudio_platform_dep, gtk4_dep],
  install: false,
)
test('titles-shell', titles_test_shell, depends: titles_mlt_module, timeout: 120)

titles_test_edit = executable(
  'test_titles_edit',
  files('test_edit.cpp'),
  cpp_args: ['-DTITLES_BRAND_XML="' + meson.current_source_dir() / '..' / 'data' / 'brand.xml' + '"'],
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-edit', titles_test_edit)

# The app's render worker (the app's only thread), without a display.
titles_test_worker = executable(
  'test_titles_worker',
  files('test_worker.cpp', '../app/render_worker.cpp', '../app/behavior_drawer.cpp'),
  include_directories: [src_inc, titles_inc],
  dependencies: [titles_doctest_dep, titles_render_dep, gtk4_dep],
  install: false,
)
test('titles-worker', titles_test_worker)

# The packaging drafts stay valid (when the validators are installed).
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('titles-desktop-file', desktop_file_validate, args: [meson.current_source_dir() / '..' / 'data' / 'com.ustudio.Titles.desktop'])
endif
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('titles-metainfo', appstreamcli,
       args: ['validate', '--no-net', meson.current_source_dir() / '..' / 'data' / 'com.ustudio.Titles.metainfo.xml'])
endif

# T3: animators, behaviours, loops, the scramble, the cursor, detaching.
titles_test_animation = executable(
  'test_titles_animation',
  files('test_animation.cpp'),
  dependencies: [titles_doctest_dep, titles_core_dep],
  install: false,
)
test('titles-animation', titles_test_animation)

# T7: the sharing client against tools/share_mock.py (doc 21's API, local).
if titles_share_enabled
  titles_test_share = executable(
    'test_titles_share',
    files('test_share.cpp'),
    cpp_args: ['-DTITLES_SHARE_MOCK="' + meson.project_source_root() / 'tools' / 'share_mock.py' + '"',
               '-DTITLES_MAKE_TEST_PACK="' + meson.project_source_root() / 'tools' / 'make_test_pack.py' + '"'],
    dependencies: [titles_doctest_dep, titles_share_dep],
    install: false,
  )
  test('titles-share', titles_test_share, timeout: 120)
endif

# ADR-020: no network library in the editor, the render tool or U-Stu Titles
# (and what this build loads into them); the helper alone has one.
titles_no_network_args = [meson.current_source_dir() / 'no_network.sh', meson.project_source_root(),
                          titles_app_exe, titles_mlt_module,
                          meson.project_build_root() / 'src' / 'app' / 'u-studio-video-editor']
titles_no_network_depends = [titles_app_exe, titles_mlt_module]
if titles_share_enabled
  titles_no_network_args += ['--', titles_share_exe]
  titles_no_network_depends += titles_share_exe
endif
test('titles-no-network', find_program('sh'), args: titles_no_network_args, depends: titles_no_network_depends)
