# Platform Support Notes (Pumpkin + Smashed Pumpkin CI)

Pumpkin nightly release assets currently provide these binaries:

- `pumpkin-X64-Linux`
- `pumpkin-ARM64-Linux`
- `pumpkin-X64-macOS`
- `pumpkin-ARM64-macOS`
- `pumpkin-X64-Windows.exe`
- `pumpkin-ARM64-Windows.exe`

Source used:

- GitHub API: `https://api.github.com/repos/Pumpkin-MC/Pumpkin/releases/tags/nightly`

CI mapping in this repository:

- Linux desktop build job (`ubuntu-latest`, x64)
- macOS desktop build jobs (`macos-13` Intel + `macos-14` Apple Silicon)
- Windows desktop build job (`windows-latest`, x64 via MSYS2, bundle + Winget metadata)

Windows packaging details:

- Installs the app as a real prefix-style bundle (`bin`, `lib`, `share`) inside the MSI
- Includes `smashed-pumpkin-tray.exe` and required GLib helper executables
- Bundles required GTK/libadwaita runtime DLLs, schemas, icon themes, and runtime modules
- Publishes WinGet metadata and a ready-to-submit manifest artifact for tagged releases

Note:

- Windows ARM64 hosted runners are not generally available in GitHub-hosted CI yet, so the pipeline currently publishes x64 Windows artifacts.
