# Homebrew Support

Smashed Pumpkin should be distributed through Homebrew as a cask, not a formula.
The project ships native macOS app bundles (`.app.zip`), which matches Homebrew Cask conventions.

## Current CI output

Tagged releases generate a `homebrew-cask` workflow artifact containing:

- `Casks/smashed-pumpkin.rb`

The generated cask points to the permanent GitHub Release URLs for:

- `smashed-pumpkin-macos-x64.app.zip`
- `smashed-pumpkin-macos-arm64.app.zip`

## Recommended publication model

Use a separate tap repository named like `homebrew-tap` or `homebrew-smashed-pumpkin`.
That repository should contain the generated `Casks/smashed-pumpkin.rb`.

Typical install flow after the tap exists:

```bash
brew tap Rotstein007/tap
brew install --cask smashed-pumpkin
```
