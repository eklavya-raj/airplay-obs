#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OBS_APP="${OBS_APP:-/Applications/OBS.app}"
OBS_SOURCE_DIR="${OBS_SOURCE_DIR:-${ROOT_DIR}/vendor/obs-studio}"

if [[ ! -d "${OBS_SOURCE_DIR}/libobs" || ! -f "${ROOT_DIR}/vendor/simde/simde/simde-common.h" ]]; then
  "${ROOT_DIR}/scripts/fetch-obs-headers.sh"
fi

cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_APP_PATH="${OBS_APP}" \
  -DOBS_SOURCE_DIR="${OBS_SOURCE_DIR}"

cmake --build "${ROOT_DIR}/build" --config RelWithDebInfo

echo "Built: ${ROOT_DIR}/build/obs-airplay-source.plugin"
