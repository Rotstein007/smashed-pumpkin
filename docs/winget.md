# Winget Packaging

Windows distribution is prepared in CI as MSI installers:

- `smashed-pumpkin-windows-x64.msi`

The desktop workflow also generates a `winget-manifests` artifact for tagged releases (`vX.Y.Z`), containing:

- `Rotstein.SmashedPumpkin.yaml`
- `Rotstein.SmashedPumpkin.locale.en-US.yaml`
- `Rotstein.SmashedPumpkin.installer.yaml`

## Publish flow

1. Create a release tag (`vX.Y.Z`).
2. Let GitHub Actions publish the GitHub Release, MSI asset, and winget manifests.
3. The `Submit WinGet Update` workflow is triggered from the published release and submits either the initial onboarding PR or a regular update PR to `microsoft/winget-pkgs`.

Intended install flow for users after winget-pkgs merge:

```powershell
winget install Rotstein.SmashedPumpkin
```
