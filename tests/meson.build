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
# can see the desktop's display fails, so a setup that stops applying (a
# renamed setup, a wrapper bypassed) shows at once.
test('headless-guard', find_program('sh'),
  args: ['-c', 'if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${DISPLAY:-}" = ":0" ] || [ -z "${USTUDIO_HEADLESS:-}" ]; then ' +
               'echo "tests can see the desktop (WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} DISPLAY=${DISPLAY:-}): ' +
               'run them through the headless test setup"; exit 1; fi'])
