# Release Pipeline Targets

## Trigger
- Tag push: `vX.Y.Z`

## Desktop Builds workflow
- macOS: `x64` (Intel), `arm64` (Apple Silicon) as native `.app.zip`
- Windows: `x64` as `.msi`
- Winget: generates ready-to-submit manifest files as artifact (`winget-manifests`) and triggers the WinGet submission workflow from published releases
- Homebrew: generates a ready-to-publish cask artifact (`homebrew-cask`) for a separate tap repository
- GitHub Release: uploads all generated artifacts for tagged builds

## Flatpak builds
Flatpak bundles for `x86_64` and `aarch64` are built inside the `Desktop Builds` workflow so release publishing and platform artifacts stay in one pipeline.

## Winget publication
Published GitHub Releases now trigger the `Submit WinGet Update` workflow automatically.
The workflow resolves the permanent GitHub Release MSI URL, handles initial onboarding if the package is not yet present in `microsoft/winget-pkgs`, and otherwise submits a regular update.

## Homebrew publication
Homebrew support is prepared as a cask, not a formula, because the project ships signed macOS app bundles rather than CLI binaries.
The tagged build generates a `homebrew-cask` artifact that can be copied into a separate `homebrew-*` tap repository.
