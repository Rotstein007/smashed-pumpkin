# Smashed Pumpkin

GTK4 + libadwaita GNOME-style manager for PumpkinMC servers.

## What it does
- Create and manage multiple Pumpkin server instances
- Install/update Pumpkin without touching worlds/plugins
- Start/stop/restart with live console + logs
- Manage plugins/worlds/players folders
- Quick settings for download URL, ports, RCON, auto-restart

## Build (Meson)
```bash
meson setup buildDir
meson compile -C buildDir
./buildDir/src/smashed-pumpkin
```

## Flatpak
```bash
flatpak-builder --disable-rofiles-fuse --user --install --force-clean buildDir/flatpak \
  build-aux/flatpak/dev.rotstein.SmashedPumpkin.json
flatpak run dev.rotstein.SmashedPumpkin
```

## Data layout
Server instances are stored in:
```
~/PumpkinServer/<id>/
  bin/pumpkin
  data/
    plugins/
    worlds/
    players/
  server.ini
```

## Notes
- Updates are resolved from Pumpkin’s download page (no GitHub release fallback).
- Worlds/plugins are kept in `data/` so updates only replace `bin/pumpkin`.
- Global config is stored at `~/.config/smashed-pumpkin/config.ini`.
