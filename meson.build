project('smashed-pumpkin', 'c',
  version: '0.6.0',
  meson_version: '>= 1.3.2',
  default_options: [
    'warning_level=2',
    'buildtype=debugoptimized'
  ]
)

# Local fixtures must not load the desktop's proxy, VFS or settings services.
add_test_setup('isolated', is_default: true, env: {
  'GIO_USE_VFS': 'local',
  'GIO_USE_PROXY_RESOLVER': 'dummy',
  'GSETTINGS_BACKEND': 'memory',
})

gnome = import('gnome')
i18n = import('i18n')
gettext_package = 'smashed-pumpkin'
gettext_locale_dir = get_option('prefix') / get_option('localedir')
intl_dep = dependency('intl')
msgfmt = find_program('msgfmt', native: true)

subdir('po')
subdir('src')
subdir('data')
if not meson.is_cross_build() and host_machine.system() == 'linux'
  subdir('tests/i18n')
endif
