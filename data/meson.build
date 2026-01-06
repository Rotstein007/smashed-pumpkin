app_id = 'dev.rotstein.SmashedPumpkin'

conf = configuration_data()
conf.set('APP_ID', app_id)
conf.set('APP_NAME', 'Smashed Pumpkin')
conf.set('APP_EXEC', 'smashed-pumpkin')

desktop_file = configure_file(
  input: app_id + '.desktop.in',
  output: app_id + '.desktop',
  configuration: conf
)

metainfo_file = configure_file(
  input: app_id + '.metainfo.xml.in',
  output: app_id + '.metainfo.xml',
  configuration: conf
)

install_data(desktop_file, install_dir: join_paths(get_option('datadir'), 'applications'))
install_data(metainfo_file, install_dir: join_paths(get_option('datadir'), 'metainfo'))

install_data(
  join_paths('icons', 'hicolor', 'scalable', 'apps', app_id + '.svg'),
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps')
)

install_data(
  join_paths('icons', 'hicolor', 'symbolic', 'apps', app_id + '-stats-symbolic.svg'),
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'symbolic', 'apps')
)

install_data(
  join_paths('icons', 'hicolor', '512x512', 'apps', app_id + '.png'),
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', '512x512', 'apps')
)
