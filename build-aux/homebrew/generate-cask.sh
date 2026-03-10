#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 5 ]]; then
  echo "Usage: $0 <version> <repository> <output-dir> <intel-zip> <arm-zip>" >&2
  exit 1
fi

version="$1"
repository="$2"
output_dir="$3"
intel_zip="$4"
arm_zip="$5"

mkdir -p "${output_dir}/Casks"

intel_sha="$(sha256sum "$intel_zip" | awk '{print toupper($1)}')"
arm_sha="$(sha256sum "$arm_zip" | awk '{print toupper($1)}')"

cat > "${output_dir}/Casks/smashed-pumpkin.rb" <<EOF
cask "smashed-pumpkin" do
  version "${version}"

  on_intel do
    sha256 "${intel_sha}"
    url "https://github.com/${repository}/releases/download/v#{version}/smashed-pumpkin-macos-x64.app.zip"
  end

  on_arm do
    sha256 "${arm_sha}"
    url "https://github.com/${repository}/releases/download/v#{version}/smashed-pumpkin-macos-arm64.app.zip"
  end

  name "Smashed Pumpkin"
  desc "Desktop management app for Pumpkin servers"
  homepage "https://github.com/${repository}"

  depends_on macos: ">= :ventura"

  app "Smashed Pumpkin.app"

  livecheck do
    url :url
    strategy :github_latest
  end

  zap trash: [
    "~/Library/Application Support/dev.rotstein.SmashedPumpkin",
    "~/Library/Preferences/dev.rotstein.SmashedPumpkin.plist",
    "~/Library/Saved Application State/dev.rotstein.SmashedPumpkin.savedState",
  ]
end
EOF
