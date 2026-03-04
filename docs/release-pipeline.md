# Release Pipeline Targets

## Trigger
- Tag push: `vX.Y.Z`

## Desktop Builds workflow
- Linux native: `x64`, `arm64` (`.tar.gz`)
- macOS: `x64` (Intel), `arm64` (Apple Silicon) as unsigned `.pkg`
- Windows: `x64`, `x86`, `arm64` as `.msi`
- Winget: generates ready-to-submit manifest files as artifact (`winget-manifests`)
- GitHub Release: uploads all generated artifacts for tagged builds

## Flatpak workflow
- Flatpak bundles for `x86_64` and `aarch64`
- Uses Flathub remotes and existing Flatpak manifest

## Winget publication
The workflow prepares manifests, but publishing to the official winget source still requires submitting to `microsoft/winget-pkgs`.
Use the generated `winget-manifests` artifact from the tagged build.
