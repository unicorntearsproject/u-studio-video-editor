doctest_dep = dependency(
  'doctest',
  required: get_option('tests'),
  fallback: ['doctest', 'doctest_dep'],
)

if doctest_dep.found()
  subdir('core')
  subdir('engine')
  subdir('app')
  subdir('dropins')
endif

# The guard for the headless default (meson.build's test setup): a test that
# can see the desktop's display, or a key or token from the shell that ran
# meson test, fails, so a setup that stops applying (a renamed setup, a
# wrapper bypassed) shows at once.
test('headless-guard', find_program('python3'),
  args: ['-I', files('headless_guard.py'), meson.project_source_root() / 'tools'])
