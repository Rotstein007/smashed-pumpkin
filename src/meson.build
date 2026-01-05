adw_dep = dependency('libadwaita-1', version: '>= 1.5')
Gtk_dep = dependency('gtk4', version: '>= 4.12')
soup_dep = dependency('libsoup-3.0', version: '>= 3.4')

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

sources = [
  'app-config.c',
  'app-config.h',
  'main.c',
  'app.c',
  'app.h',
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
]

executable(
  'smashed-pumpkin',
  sources,
  include_directories: inc,
  dependencies: [adw_dep, Gtk_dep, soup_dep],
  install: true
)
