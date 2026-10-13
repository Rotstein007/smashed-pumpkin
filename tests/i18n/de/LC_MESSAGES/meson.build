i18n_test_catalog = custom_target('i18n-test-catalog',
  input: '../../de.po',
  output: gettext_package + '.mo',
  command: [msgfmt, '--check', '--check-format', '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true,
  install: false)
