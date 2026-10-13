i18n_discovery_catalogs += custom_target('i18n-traditional-fixture',
  input: '../../zh_TW.po',
  output: gettext_package + '.mo',
  command: [msgfmt, '--check', '--check-format', '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true,
  install: false)
