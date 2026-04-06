adw_dep = dependency('libadwaita-1', version: '>= 1.5')
Gtk_dep = dependency('gtk4', version: '>= 4.12')
soup_dep = dependency('libsoup-3.0', version: '>= 3.4')
psapi_dep = dependency('psapi', required: false)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: false)
shell32_dep = cc.find_library('shell32', required: host_machine.system() == 'windows')
user32_dep = cc.find_library('user32', required: host_machine.system() == 'windows')
dwmapi_dep = cc.find_library('dwmapi', required: host_machine.system() == 'windows')

app_id = 'dev.rotstein.SmashedPumpkin'

inc = include_directories('.')

conf = configuration_data()
conf.set_quoted('APP_ID', app_id)
conf.set_quoted('APP_NAME', 'Smashed Pumpkin')
conf.set_quoted('APP_VERSION', meson.project_version())

config_h = configure_file(
  output: 'config.h',
  configuration: conf
)

resources_xml = configure_file(
  input: join_paths('..', 'resources', 'smashed-pumpkin.gresource.xml.in'),
  output: 'smashed-pumpkin.gresource.xml',
  configuration: conf
)

resources = gnome.compile_resources(
  'smashed-pumpkin-resources',
  resources_xml,
  source_dir: join_paths('..', 'resources'),
  c_name: 'smashed_pumpkin',
  extra_args: '--manual-register'
)

windows_resources = []
if host_machine.system() == 'windows'
  rc_conf = configuration_data()
  rc_conf.set_quoted('ICON_PATH', meson.project_source_root() / 'resources' / 'windows' / 'smashed-pumpkin.ico')
  windows_rc = configure_file(
    input: 'windows-resource.rc.in',
    output: 'smashed-pumpkin.rc',
    configuration: rc_conf
  )
  windres = find_program('windres')
  windows_resources = custom_target(
    'smashed-pumpkin-windows-resources',
    input: windows_rc,
    output: 'smashed-pumpkin-windows-resources.o',
    command: [windres, '@INPUT@', '@OUTPUT@']
  )
endif

sources = [
  'app-config.c',
  'app-config.h',
  'main.c',
  'app.c',
  'app.h',
  'window-internal.h',
  'window-lifecycle.c',
  'window-lifecycle.h',
  'window-console.c',
  'window-console.h',
  'window-networks.c',
  'window-networks.h',
  'window-player-data.c',
  'window-player-data.h',
  'window-players.c',
  'window-players.h',
  'window-parse.c',
  'window-parse.h',
  'window-protocol.c',
  'window-protocol.h',
  'window.c',
  'window.h',
  'server.c',
  'server.h',
  'server-store.c',
  'server-store.h',
  'download.c',
  'download.h',
  config_h,
  resources,
  windows_resources,
]

install_data(join_paths('..', 'resources', 'icons', 'hicolor', 'scalable', 'apps',
                        'dev.rotstein.SmashedPumpkin.svg'),
             install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'))
install_data(join_paths('..', 'resources', 'icons', 'hicolor', 'symbolic', 'apps',
                        'dev.rotstein.SmashedPumpkin-stats-symbolic.svg'),
             install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'symbolic', 'apps'))
install_data(join_paths('..', 'resources', 'icons', 'hicolor', '512x512', 'apps',
                        'dev.rotstein.SmashedPumpkin.png'),
             install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', '512x512', 'apps'))

executable(
  'smashed-pumpkin',
  sources,
  include_directories: inc,
  dependencies: [adw_dep, Gtk_dep, soup_dep, psapi_dep, math_dep, shell32_dep, dwmapi_dep],
  win_subsystem: host_machine.system() == 'windows' ? 'windows' : 'console',
  install: true
)

gtk3_dep = dependency('gtk+-3.0', required: false)
appindicator_dep = dependency('ayatana-appindicator3-0.1', required: false)
if not appindicator_dep.found()
  appindicator_dep = dependency('appindicator3-0.1', required: false)
endif

if host_machine.system() == 'linux' and gtk3_dep.found() and appindicator_dep.found()
  executable(
    'smashed-pumpkin-tray',
    ['tray.c', 'app-config.c', 'app-config.h', config_h, resources],
    include_directories: inc,
    dependencies: [gtk3_dep, appindicator_dep],
    install: true
  )
elif host_machine.system() == 'linux'
  warning('Tray helper disabled: missing gtk+-3.0 and/or appindicator (ayatana-appindicator3-0.1 or appindicator3-0.1)')
elif host_machine.system() == 'windows'
  executable(
    'smashed-pumpkin-tray',
    ['tray-win.c', config_h, windows_resources],
    include_directories: inc,
    dependencies: [shell32_dep, user32_dep],
    win_subsystem: 'windows',
    install: true
  )
endif
