#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/obs-airplay-source.plugin" >&2
  exit 1
fi

BUNDLE_PATH="$1"
GST_PLUGIN_DIR="${BUNDLE_PATH}/Contents/MacOS/gstreamer-1.0"
FRAMEWORKS_DIR="${BUNDLE_PATH}/Contents/Frameworks"
MAIN_BINARY="${BUNDLE_PATH}/Contents/MacOS/obs-airplay-source"
FRAMEWORK_RPATH='@loader_path/../../Frameworks'
MAIN_BINARY_FRAMEWORK_RPATH='@loader_path/../Frameworks'

find_longest_matching_basename() {
  local dir="$1"
  local prefix="$2"
  local best=""

  for path in "${dir}"/"${prefix}"*.dylib; do
    [[ -e "${path}" ]] || continue
    local base
    base="$(basename "${path}")"
    if [[ ${#base} -gt ${#best} ]]; then
      best="${base}"
    fi
  done

  echo "${best}"
}

if [[ -f "${MAIN_BINARY}" ]]; then
  changed=0

  if ! otool -l "${MAIN_BINARY}" | grep -q "${MAIN_BINARY_FRAMEWORK_RPATH}"; then
    install_name_tool -add_rpath "${MAIN_BINARY_FRAMEWORK_RPATH}" "${MAIN_BINARY}"
    changed=1
  fi

  avformat_basename="$(find_longest_matching_basename "${FRAMEWORKS_DIR}" "libavformat")"
  avcodec_basename="$(find_longest_matching_basename "${FRAMEWORKS_DIR}" "libavcodec")"
  avutil_basename="$(find_longest_matching_basename "${FRAMEWORKS_DIR}" "libavutil")"
  swscale_basename="$(find_longest_matching_basename "${FRAMEWORKS_DIR}" "libswscale")"

  if [[ -n "${avformat_basename}" ]]; then
    install_name_tool -change "/opt/homebrew/opt/ffmpeg/lib/libavformat.62.dylib" "@rpath/${avformat_basename}" "${MAIN_BINARY}" 2>/dev/null || true
  fi
  if [[ -n "${avcodec_basename}" ]]; then
    install_name_tool -change "/opt/homebrew/opt/ffmpeg/lib/libavcodec.62.dylib" "@rpath/${avcodec_basename}" "${MAIN_BINARY}" 2>/dev/null || true
  fi
  if [[ -n "${avutil_basename}" ]]; then
    install_name_tool -change "/opt/homebrew/opt/ffmpeg/lib/libavutil.60.dylib" "@rpath/${avutil_basename}" "${MAIN_BINARY}" 2>/dev/null || true
  fi
  if [[ -n "${swscale_basename}" ]]; then
    install_name_tool -change "/opt/homebrew/opt/ffmpeg/lib/libswscale.9.dylib" "@rpath/${swscale_basename}" "${MAIN_BINARY}" 2>/dev/null || true
  fi

  if otool -L "${MAIN_BINARY}" | grep -q "/opt/homebrew/opt/ffmpeg/lib/"; then
    echo "Failed to rewrite FFmpeg dependencies for ${MAIN_BINARY}" >&2
    exit 1
  fi

  codesign --force --sign - "${MAIN_BINARY}" >/dev/null 2>&1 || true
fi

if [[ ! -d "${GST_PLUGIN_DIR}" ]]; then
  exit 0
fi

for dylib in "${GST_PLUGIN_DIR}"/*.dylib; do
  [[ -f "${dylib}" ]] || continue
  desired_id="@rpath/$(basename "${dylib}")"
  current_id="$(otool -D "${dylib}" | tail -n +2 | head -n 1 || true)"
  changed=0

  if ! otool -l "${dylib}" | grep -q "${FRAMEWORK_RPATH}"; then
    install_name_tool -add_rpath "${FRAMEWORK_RPATH}" "${dylib}"
    changed=1
  fi

  if [[ "${current_id}" != "${desired_id}" ]]; then
    install_name_tool -id "${desired_id}" "${dylib}"
    changed=1
  fi

  if [[ ${changed} -eq 1 ]]; then
    codesign --force --sign - "${dylib}" >/dev/null 2>&1 || true
  fi
done
