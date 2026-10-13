i18n_discovery_catalogs += custom_target('i18n-simplified-fixture',
  input: '../../zh_CN.po',
  output: gettext_package + '.mo',
  command: [msgfmt, '--check', '--check-format', '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true,
  install: false)
