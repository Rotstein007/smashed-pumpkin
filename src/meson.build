android_host = host_machine.system() == 'android'

if android_host and not meson.version().version_compare('>=1.9.0')
  error('Android builds require Meson >= 1.9.0 for android_exe_type support')
endif

if android_host
  adw_dep = dependency('libadwaita-1',
    version: '>= 1.5',
    fallback: ['libadwaita', 'libadwaita_dep'])
  Gtk_dep = dependency('gtk4', version: '>= 4.12')
  soup_dep = dependency('libsoup-3.0',
    version: '>= 3.4',
    fallback: ['libsoup', 'libsoup_dep'])
else
  adw_dep = dependency('libadwaita-1', version: '>= 1.5')
  Gtk_dep = dependency('gtk4', version: '>= 4.12')
  soup_dep = dependency('libsoup-3.0', version: '>= 3.4')
endif
psapi_dep = dependency('psapi', required: false)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: false)
shell32_dep = cc.find_library('shell32', required: host_machine.system() == 'windows')
user32_dep = cc.find_library('user32', required: host_machine.system() == 'windows')
dwmapi_dep = cc.find_library('dwmapi', required: host_machine.system() == 'windows')
crypt32_dep = cc.find_library('crypt32', required: host_machine.system() == 'windows')
gmodule_dep = dependency('gmodule-2.0')
platform_credential_deps = []
if host_machine.system() == 'darwin'
  platform_credential_deps += dependency('appleframeworks', modules: ['Security'])
endif

app_id = 'dev.rotstein.SmashedPumpkin'

inc = include_directories('.')

# cJSON uses strtod internally; without locale support JSON decimals fail in
# locales such as de_DE. Keep the upstream default enabled for every consumer.
cjson_library = static_library('smashed-pumpkin-json',
  'vendor/cjson/cJSON.c',
  c_args: ['-DENABLE_LOCALES'],
  install: false)
cjson_dep = declare_dependency(link_with: cjson_library, dependencies: math_dep)

conf = configuration_data()
conf.set_quoted('APP_ID', app_id)
conf.set_quoted('APP_NAME', 'Smashed Pumpkin')
conf.set_quoted('APP_VERSION', meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
conf.set_quoted('LOCALEDIR', gettext_locale_dir)
# Uninstalled development builds fall back to build-tree catalogs so running
# ./buildDir/src/smashed-pumpkin discovers translations without meson install.
# Installed catalogs always win when present; the fallback is only consulted
# when the configured LOCALEDIR has no catalog for this domain.
conf.set_quoted('BUILD_LOCALEDIR', meson.project_build_root() / 'po')

# glibc suppresses gettext in C-family locales. Ship a messages-only UTF-8
# locale so explicit language choices do not require a matching system locale.
message_locale = []
private_message_locale = false
message_locale_root = get_option('prefix') / get_option('datadir') / 'smashed-pumpkin/locales'
if host_machine.system() == 'linux' and not meson.is_cross_build() and cc.compiles('#include <features.h>\n#ifndef __GLIBC__\n#error not glibc\n#endif\nint main(void) { return 0; }')
  message_locale = custom_target('message-locale',
    output: 'SYS_LC_MESSAGES',
    command: [find_program('python3'), meson.project_source_root() / 'build-aux/build-message-locale.py',
              find_program('localedef', native: true).full_path(), '@OUTPUT@'],
    depend_files: '../build-aux/build-message-locale.py',
    build_by_default: true,
    install: true,
    install_dir: message_locale_root / 'smashed.UTF-8/LC_MESSAGES')
  private_message_locale = true
  conf.set('HAVE_PRIVATE_MESSAGE_LOCALE', 1)
  conf.set_quoted('MESSAGE_LOCALE_ROOT', message_locale_root)
  # The locale script mirrors the installed hierarchy under the build tree for
  # uninstalled runs; use it when the installed private locale is absent.
  conf.set_quoted('BUILD_MESSAGE_LOCALE_ROOT', meson.current_build_dir() / 'message-locales')
endif

startup_deps = []
if host_machine.system() == 'linux'
  startup_x11_dep = dependency('x11', required: false)
  if startup_x11_dep.found() and cc.has_header('gdk/x11/gdkx.h', dependencies: Gtk_dep)
    conf.set('HAVE_STARTUP_X11', 1)
    startup_deps += startup_x11_dep
  endif
endif

config_h = configure_file(
  output: 'config.h',
  configuration: conf
)

resources_conf = configuration_data()
bridge_dependencies = []
bridge_sample = meson.project_source_root() / 'resources/plugins/map-bridge.wasm'
if get_option('build_minimap_bridge')
  bridge_cargo = find_program('cargo', native: true)
  bridge_target = custom_target('minimap-bridge',
    input: ['../plugins/map-bridge/Cargo.toml', '../plugins/map-bridge/Cargo.lock',
            '../plugins/map-bridge/src/lib.rs'],
    output: 'map-bridge.wasm',
    command: [find_program('env'), 'CARGO=' + bridge_cargo.full_path(),
              'CARGO_TARGET_DIR=' + meson.current_build_dir() / 'bridge-target',
              find_program('bash'), meson.project_source_root() / 'build-aux/build-map-bridge.sh',
              '--output', '@OUTPUT@', '--frozen'],
    depend_files: '../build-aux/build-map-bridge.sh')
  bridge_dependencies += bridge_target
  bridge_sample = meson.current_build_dir() / 'map-bridge.wasm'
  resources_conf.set('MAP_BRIDGE_RESOURCE', 'map-bridge.wasm')
else
  resources_conf.set('MAP_BRIDGE_RESOURCE', 'plugins/map-bridge.wasm')
endif

resources_xml = configure_file(
  input: join_paths('..', 'resources', 'smashed-pumpkin.gresource.xml.in'),
  output: 'smashed-pumpkin.gresource.xml',
  configuration: resources_conf
)

resources = gnome.compile_resources(
  'smashed-pumpkin-resources',
  resources_xml,
  source_dir: [meson.current_build_dir(), join_paths('..', 'resources')],
  dependencies: bridge_dependencies,
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
  'i18n.c',
  'i18n.h',
  'language.c',
  'language.h',
  'release-channel.c',
  'release-channel.h',
  'release-metadata.c',
  'release-metadata.h',
  'build-metadata.c',
  'build-metadata.h',
  'minimap-model.c',
  'minimap-model.h',
  'minimap-store.c',
  'minimap-store.h',
  'minimap-bridge-control.c',
  'minimap-bridge-control.h',
  'minimap-terrain.c',
  'minimap-terrain.h',
  'minimap-cache.c',
  'minimap-cache.h',
  'minimap-memory-cache.c',
  'minimap-memory-cache.h',
  'minimap-motion.c',
  'minimap-motion.h',
  'minimap-render.c',
  'minimap-render.h',
  'window-minimap.c',
  'window-minimap.h',
  'world-store.c',
  'world-store.h',
  'window-worlds.c',
  'window-worlds.h',
  'refresh-button.c',
  'refresh-button.h',
  'app-config.c',
  'app-config.h',
  'window-startup.c',
  'window-startup.h',
  'window-dialogs.c',
  'window-dialogs.h',
  'main.c',
  'app.c',
  'app.h',
  'window-internal.h',
  'window-lifecycle.c',
  'window-lifecycle.h',
  'window-server-actions.c',
  'window-server-actions.h',
  'window-console.c',
  'window-console.h',
  'window-networks.c',
  'window-networks.h',
  'window-player-data.c',
  'window-player-data.h',
  'window-players.c',
  'window-players.h',
  'window-player-events.c',
  'player-protocol.c',
  'player-protocol.h',
  'player-heads.c',
  'player-heads.h',
  'window-parse.c',
  'window-parse.h',
  'window-protocol.c',
  'window-protocol.h',
  'window.c',
  'marketplace-request.c',
  'marketplace-request.h',
  'window.h',
  'server.c',
  'server.h',
  'server-maintenance.c',
  'server-maintenance.h',
  'server-command-tracker.c',
  'server-command-tracker.h',
  'server-store.c',
  'server-store.h',
  'download.c',
  'download.h',
  'marketplace-credentials.c',
  'marketplace-credentials.h',
  'plugin-store.c',
  'plugin-store.h',
  'marketplace-auth.c',
  'marketplace-auth.h',
  'vendor/tomlc17/tomlc17.c',
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

if android_host
  executable(
    'smashed-pumpkin',
    sources,
    include_directories: inc,
    dependencies: [adw_dep, Gtk_dep, soup_dep, psapi_dep, math_dep, shell32_dep, user32_dep, dwmapi_dep, crypt32_dep, gmodule_dep, cjson_dep, intl_dep] + platform_credential_deps + startup_deps,
    android_exe_type: 'application',
    link_depends: message_locale,
    install: true
  )
else
  executable(
    'smashed-pumpkin',
    sources,
    include_directories: inc,
    dependencies: [adw_dep, Gtk_dep, soup_dep, psapi_dep, math_dep, shell32_dep, user32_dep, dwmapi_dep, crypt32_dep, gmodule_dep, cjson_dep, intl_dep] + platform_credential_deps + startup_deps,
    win_subsystem: host_machine.system() == 'windows' ? 'windows' : 'console',
    link_depends: message_locale,
    install: true
  )
endif

gtk3_dep = dependency('gtk+-3.0', required: false)
if not meson.is_cross_build()
  test('release-notes', find_program('python3'),
    args: [meson.project_source_root() / 'tests/test-release-notes.py'],
    env: {'PYTHONDONTWRITEBYTECODE': '1'})
  release_download_tests = executable('test-release-download',
    ['../tests/test-release-download.c', 'release-channel.c', 'release-metadata.c', 'download.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), soup_dep, math_dep, cjson_dep], install: false)
  test('release-download', release_download_tests)
  build_metadata_tests = executable('test-build-metadata',
    ['../tests/test-build-metadata.c', 'build-metadata.c'],
    include_directories: inc, dependencies: [dependency('glib-2.0'), intl_dep], install: false)
  test('build-metadata', build_metadata_tests, env: {'LC_ALL': 'C', 'LANGUAGE': 'en'})
  download_http_tests = executable('test-download-http',
    ['../tests/test-download-http.c', 'download.c', 'release-channel.c', 'release-metadata.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), soup_dep, math_dep, cjson_dep], install: false)
  test('download-http', download_http_tests)
  minimap_tests = executable('test-minimap',
    ['../tests/test-minimap.c', 'minimap-model.c', 'minimap-store.c', 'minimap-terrain.c', 'minimap-cache.c', 'minimap-motion.c', 'minimap-render.c', 'vendor/tomlc17/tomlc17.c', resources],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), dependency('cairo'), math_dep, cjson_dep], install: false)
  test('minimap', minimap_tests,
    env: {'SMASHED_BUNDLED_BRIDGE_SAMPLE': bridge_sample}, depends: bridge_dependencies)
  terrain_tests = executable('test-minimap-terrain',
    ['../tests/test-minimap-terrain.c', 'minimap-terrain.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), dependency('cairo'), math_dep], install: false)
  test('minimap-terrain', terrain_tests)
  memory_cache_tests = executable('test-minimap-memory-cache',
    ['../tests/test-minimap-memory-cache.c', 'minimap-memory-cache.c'],
    include_directories: inc,
    dependencies: [dependency('glib-2.0')], install: false)
  test('minimap-memory-cache', memory_cache_tests)
  world_store_tests = executable('test-world-store',
    ['../tests/test-world-store.c', 'world-store.c', 'vendor/tomlc17/tomlc17.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), math_dep], install: false)
  test('world-store', world_store_tests)
  player_protocol_tests = executable('test-player-protocol',
    ['../tests/test-player-protocol.c', 'player-protocol.c', 'window-protocol.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), cjson_dep],
    install: false)
  test('player-protocol', player_protocol_tests)
  player_heads_tests = executable('test-player-heads',
    ['../tests/test-player-heads.c', 'player-heads.c'],
    include_directories: inc,
    dependencies: [Gtk_dep, soup_dep, cjson_dep], install: false)
  test('player-heads', player_heads_tests)
  startup_config_tests = executable('test-startup-config',
    ['../tests/test-startup-config.c', 'app-config.c', 'release-channel.c', 'language.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0')],
    install: false)
  test('startup-config', startup_config_tests)
  language_tests = executable('test-language',
    ['../tests/test-language.c', 'language.c', 'app-config.c', 'release-channel.c'],
    include_directories: inc, dependencies: [dependency('gio-2.0')], install: false)
  test('language', language_tests)
  plugin_store_tests = executable('test-plugin-store',
    ['../tests/test-plugin-store.c', 'plugin-store.c', 'marketplace-auth.c',
     'vendor/tomlc17/tomlc17.c'],
    include_directories: inc,
    dependencies: [dependency('gio-2.0'), math_dep, cjson_dep],
    install: false)
  test('plugin-store', plugin_store_tests)
  marketplace_request_tests = executable('test-marketplace-request',
    ['../tests/test-marketplace-request.c', 'marketplace-request.c'],
    include_directories: inc,
    dependencies: [soup_dep, dependency('gio-2.0')],
    install: false)
  test('marketplace-request', marketplace_request_tests)
  marketplace_json_tests = executable('test-marketplace-json',
    '../tests/test-marketplace-json.c',
    include_directories: inc,
    dependencies: [dependency('glib-2.0'), cjson_dep],
    install: false)
  test('marketplace-json', marketplace_json_tests)
  command_tracker_tests = executable('test-server-command-tracker',
    ['../tests/test-server-command-tracker.c', 'server-command-tracker.c'],
    include_directories: inc,
    dependencies: [dependency('gobject-2.0')], install: false)
  test('server-command-tracker', command_tracker_tests)
  if host_machine.system() == 'linux'
    server_store_tests = executable('test-server-store',
      ['../tests/test-server-store.c', 'server-store.c', 'server.c', 'server-command-tracker.c',
       'app-config.c', 'language.c', 'release-channel.c'],
      include_directories: inc,
      dependencies: [dependency('gio-2.0')], install: false)
    test('server-store', server_store_tests)
    managed_server_fixture = executable('fake-managed-server',
      '../tests/fake-managed-server.c',
      dependencies: [dependency('glib-2.0')], install: false)
    maintenance_tests = executable('test-server-maintenance',
      ['../tests/test-server-maintenance.c', 'server-maintenance.c', 'server.c', 'server-command-tracker.c', 'release-channel.c'],
      include_directories: inc,
      c_args: ['-DPUMPKIN_STOP_TIMEOUT_SECONDS=1'],
      dependencies: [dependency('gio-2.0')], install: false)
    test('server-maintenance', maintenance_tests, args: [managed_server_fixture])
    bridge_control_tests = executable('test-minimap-bridge-control',
      ['../tests/test-minimap-bridge-control.c', 'minimap-bridge-control.c', 'minimap-store.c',
       'minimap-model.c', 'server.c', 'server-command-tracker.c', 'release-channel.c', 'vendor/tomlc17/tomlc17.c', resources],
      include_directories: inc,
      dependencies: [dependency('gio-2.0'), dependency('cairo'), math_dep, cjson_dep], install: false)
    test('minimap-bridge-control', bridge_control_tests, args: [managed_server_fixture],
      env: {'SMASHED_BUNDLED_BRIDGE_SAMPLE': bridge_sample}, depends: bridge_dependencies)
  endif
endif

appindicator_dep = dependency('ayatana-appindicator3-0.1', required: false)
tray_c_args = appindicator_dep.found() ? ['-DHAVE_AYATANA_APPINDICATOR'] : []
if not appindicator_dep.found()
  appindicator_dep = dependency('appindicator3-0.1', required: false)
endif

if host_machine.system() == 'linux' and gtk3_dep.found() and appindicator_dep.found()
  executable(
    'smashed-pumpkin-tray',
    ['tray.c', 'i18n.c', 'i18n.h', 'language.c', 'language.h', 'app-config.c', 'app-config.h', 'release-channel.c', config_h, resources],
    include_directories: inc,
    dependencies: [gtk3_dep, appindicator_dep, intl_dep],
    c_args: tray_c_args,
    link_depends: message_locale,
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
    link_depends: message_locale,
    install: true
  )
endif
