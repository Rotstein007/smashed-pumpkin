subdir('de/LC_MESSAGES')
i18n_discovery_catalogs = [i18n_test_catalog]
subdir('zh_CN/LC_MESSAGES')
subdir('zh_TW/LC_MESSAGES')

# Discovery edge cases: a truncated catalog, one with an invalid UTF-8 header
# and a valid big-endian catalog. The first two must be skipped silently, the
# third must be discovered and usable for lookups.
i18n_synthetic_catalogs = custom_target('i18n-synthetic-catalogs',
  output: ['xx_TRUNC', 'xx_BADHDR', 'xx_BE'],
  command: [translation_python, files('make-synthetic-catalogs.py'), meson.current_build_dir()],
  depend_files: files('make-synthetic-catalogs.py'),
  build_by_default: true)
i18n_discovery_catalogs += [i18n_synthetic_catalogs]

i18n_tests = executable('test-i18n',
  ['../test-i18n.c', '../../src/i18n.c', '../../src/language.c', '../../src/build-metadata.c'],
  include_directories: inc,
  dependencies: [dependency('glib-2.0'), intl_dep, cjson_dep],
  install: false)

# Each environment gets a fresh process: gettext and GLib cache locale data.
# Use an available non-C UTF-8 locale: some libc versions suppress LANGUAGE
# even for C.UTF-8. The German catalog does not need a German system locale.
foreach scenario : [
  ['translated', 'en_US.UTF-8', 'de', 'translated'],
  ['english', 'C', 'de', 'english'],
  ['fallback', 'C.UTF-8', 'zz_ZZ', 'english'],
  ['fallback-chain', 'en_US.UTF-8', 'zz_ZZ:de', 'translated'],
  ['invalid-locale', 'smashed_invalid_locale', 'de', 'english'],
]
  test('i18n-' + scenario[0], i18n_tests,
    args: [scenario[3]],
    env: {'LC_ALL': scenario[1], 'LANGUAGE': scenario[2], 'TZ': 'UTC',
          'SMASHED_PUMPKIN_LOCALEDIR': meson.current_build_dir()},
    depends: i18n_test_catalog)
endforeach

# Missing-catalog fallback must run with translations enabled, not a C locale
# that would disable lookups regardless of the catalog directory.
test('i18n-missing-catalog', i18n_tests,
  args: ['missing-catalog'],
  env: {'LC_ALL': 'en_US.UTF-8', 'LANGUAGE': 'de', 'TZ': 'UTC',
        'SMASHED_PUMPKIN_LOCALEDIR': meson.current_build_dir() / 'missing'},
  depends: i18n_test_catalog)
test('i18n-relative-path', i18n_tests,
  args: ['english'],
  env: {'LC_ALL': 'C', 'LANGUAGE': 'de', 'TZ': 'UTC',
        'SMASHED_PUMPKIN_LOCALEDIR': 'relative-catalog-directory'})

language_startup_tests = executable('test-language-startup',
  ['../test-language-startup.c', '../../src/i18n.c', '../../src/language.c'],
  include_directories: inc, dependencies: [dependency('gio-2.0'), intl_dep], install: false)
foreach scenario : [
  ['first-launch', 'absent', 'C.UTF-8', 'de', 'de', 'Übersicht', 'private'],
  ['system', 'system', 'C.UTF-8', 'de', 'de', 'Übersicht', 'private'],
  ['manual-c', 'de', 'C', 'en', 'de', 'Übersicht', 'private'],
  ['manual-posix', 'de', 'POSIX', 'en', 'de', 'Übersicht', 'private'],
  ['manual-invalid-locale', 'de', 'smashed_invalid_locale', 'en', 'de', 'Übersicht', 'private'],
  ['manual-en', 'en', 'en_US.UTF-8', 'de', 'en', 'Overview', 'normal'],
  ['manual-zh', 'zh_CN', 'C', 'de', 'zh_CN', 'Overview', 'private'],
  ['first-traditional', 'absent', 'C.UTF-8', 'zh_TW.UTF-8', 'zh_TW', 'Traditional catalog fixture', 'private'],
  ['system-traditional', 'system', 'C.UTF-8', 'zh-Hant-TW', 'zh_TW', 'Traditional catalog fixture', 'private'],
  ['manual-traditional', 'zh_TW', 'C', 'en', 'zh_TW', 'Traditional catalog fixture', 'private'],
  ['unsupported', 'unsupported-id', 'C', 'de', 'en', 'Overview', 'normal'],
  ['system-posix', 'system', 'C', 'de', 'en', 'Overview', 'normal'],
  ['first-unsupported', 'absent', 'C.UTF-8', 'fr', 'en', 'Overview', 'normal'],
  # Uninstalled development builds must resolve the build-tree fallbacks
  # without SMASHED_PUMPKIN_*_LOCALEDIR overrides: the compiled production
  # catalog under the build-tree po directory and the mirrored private
  # messages-only locale. A "-" label expectation skips translation content
  # so the scenario stays independent of catalog text and stale installs.
  ['dev-fallback-first', 'absent', 'C.UTF-8', 'zh_CN.UTF-8', 'zh_CN', '-', 'private'],
  ['dev-fallback-manual', 'zh_CN', 'C', 'en', 'zh_CN', '-', 'private'],
  # LANGUAGE entries that match nothing fall back to the locale language
  # instead of persisting an English detection (gettext semantics).
  ['fallback-to-locale', 'absent', 'de_DE.UTF-8', 'it_IT', 'de', 'Übersicht', 'normal'],
  # C/empty LANGUAGE entries are skipped, not terminal; whitespace is tolerated.
  ['language-list-c-entry', 'absent', 'C.UTF-8', 'C:de', 'de', 'Übersicht', 'private'],
  ['language-list-spaces', 'absent', 'C.UTF-8', 'fr: de', 'de', 'Übersicht', 'private'],
  # Malformed catalogs must be skipped by discovery without crashing.
  ['corrupt-catalog-skip', 'absent', 'C.UTF-8', 'xx_TRUNC', 'en', 'Overview', 'normal'],
  ['bad-header-catalog-skip', 'absent', 'C.UTF-8', 'xx_BADHDR', 'en', 'Overview', 'normal'],
  # A big-endian catalog is discovered and its translations are used.
  ['big-endian-catalog', 'absent', 'C.UTF-8', 'xx_BE', 'xx_BE', 'BE-Übersicht', 'private'],
]
  if scenario[6] != 'private' or private_message_locale
    scenario_env = {'LC_ALL': scenario[2], 'LANGUAGE': scenario[3]}
    scenario_deps = i18n_discovery_catalogs + [message_locale]
    if scenario[0].startswith('dev-fallback')
      scenario_deps += smashed_pumpkin_gmo
    else
      scenario_env += {'SMASHED_PUMPKIN_LOCALEDIR': meson.current_build_dir(),
                       'SMASHED_PUMPKIN_MESSAGE_LOCALEDIR': meson.project_build_root() / 'src/message-locales'}
    endif
    test('language-startup-' + scenario[0], language_startup_tests,
      args: [scenario[1], scenario[4], scenario[5], scenario[6]],
      env: scenario_env,
      depends: scenario_deps)
  endif
endforeach

test('list-translations', translation_python,
  args: files('../test-list-translations.py'),
  env: {'PYTHONDONTWRITEBYTECODE': '1'})

xgettext = find_program('xgettext', native: true)
test('i18n-sources', find_program('python3'),
  args: [meson.project_source_root() / 'tests/test-i18n-sources.py',
         meson.project_source_root(), xgettext.full_path()],
  env: {'PYTHONDONTWRITEBYTECODE': '1'})
