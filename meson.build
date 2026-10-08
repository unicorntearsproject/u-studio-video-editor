project(
  'u-studio-video-editor', 'cpp',
  version: '0.82.2-beta.1',
  license: 'MIT',
  license_files: ['LICENSE'],
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
  ],
)

cpp = meson.get_compiler('cpp')

# include_type: 'system' makes meson pass these via -isystem instead of -I,
# so GCC's default suppression of warnings from system headers applies --
# needed because -Wold-style-cast etc. otherwise fire on GTK/GLib's own
# macros (G_TYPE_CHECK_INSTANCE_CAST, G_CALLBACK, G_DEFINE_AUTOPTR_CLEANUP_FUNC)
# at every call site, which we don't control and can't fix upstream.
gtk4_dep = dependency('gtk4', version: '>=4.10', include_type: 'system')
adwaita_dep = dependency('libadwaita-1', include_type: 'system')
mlt_dep = dependency('mlt-framework-7', include_type: 'system')
mltpp_dep = dependency('mlt++-7', include_type: 'system')
glib_dep = dependency('glib-2.0', include_type: 'system')
gio_dep = dependency('gio-2.0', include_type: 'system')
gobject_dep = dependency('gobject-2.0', include_type: 'system')
libxml2_dep = dependency('libxml-2.0', include_type: 'system')

# Extra warnings beyond warning_level=3 (doc 11). -Werror is deliberately
# NOT part of default_options -- it's gated behind -Dwerror so local dev
# builds don't hard-fail the first time these flags surface pre-existing
# warnings; CI sets -Dwerror=true.
extra_warning_flags = cpp.get_supported_arguments(
  '-Wshadow',
  '-Wconversion',
  '-Wold-style-cast',
  '-Wnon-virtual-dtor',
)
add_project_arguments(extra_warning_flags, language: 'cpp')

# 1 for debug and debugoptimized builds, 0 for release and plain. Not
# NDEBUG: this project leaves b_ndebug at meson's default (false), so NDEBUG
# is never defined, even in a release build. A build with -Db_ndebug=true is
# safe too (2026-09-27): no assert() guards anything a release needs (the
# model's preconditions and UndoStack::redo() take a safe path), and the
# test suites pass that way.
add_project_arguments('-DUSTUDIO_DEBUG_BUILD=' + (get_option('debug') ? '1' : '0'), language: 'cpp')

# -Werror itself is meson's own built-in option (-Dwerror=true); no need to
# add it manually -- meson already does when that's set. CI sets it; local
# dev builds default to false.

subdir('src')
# Drop-ins (ADR-013/014) build against src/'s libraries, and the programs in
# src/app and src/render link the built-in ones, hence this order.
subdir('drop-ins')
subdir('src/app')
if get_option('render_tool')
  subdir('src/render')
endif
subdir('data')
# Every test runs on a private X display and D-Bus session, never on the
# developer's desktop (owner rule, 2026-10-08: plain `meson test` opened GTK
# windows on the real desktop). The default, so nobody has to remember it;
# tools/test-headless.sh says how.
add_test_setup('headless', exe_wrapper: find_program('tools/test-headless.sh'), is_default: true)

subdir('tests')
