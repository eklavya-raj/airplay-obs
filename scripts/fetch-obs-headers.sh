#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OBS_APP="${OBS_APP:-/Applications/OBS.app}"
OBS_VERSION="${OBS_VERSION:-}"

if [[ -z "${OBS_VERSION}" ]]; then
  OBS_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${OBS_APP}/Contents/Info.plist")"
fi

mkdir -p "${ROOT_DIR}/vendor"

if [[ -d "${ROOT_DIR}/vendor/obs-studio/.git" ]]; then
  git -C "${ROOT_DIR}/vendor/obs-studio" fetch --tags --depth 1 origin "refs/tags/${OBS_VERSION}:refs/tags/${OBS_VERSION}"
  git -C "${ROOT_DIR}/vendor/obs-studio" checkout -q "${OBS_VERSION}"
else
  git clone --depth 1 --branch "${OBS_VERSION}" https://github.com/obsproject/obs-studio.git "${ROOT_DIR}/vendor/obs-studio"
fi

if [[ -d "${ROOT_DIR}/vendor/simde/.git" ]]; then
  git -C "${ROOT_DIR}/vendor/simde" fetch --depth 1 origin main
  git -C "${ROOT_DIR}/vendor/simde" checkout -q FETCH_HEAD
else
  git clone --depth 1 https://github.com/simd-everywhere/simde.git "${ROOT_DIR}/vendor/simde"
fi

echo "OBS headers ready at ${ROOT_DIR}/vendor/obs-studio"
echo "SIMDe headers ready at ${ROOT_DIR}/vendor/simde"
