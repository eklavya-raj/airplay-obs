#!/usr/bin/env bash
set -euo pipefail

# Find the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_NAME="obs-airplay-source.plugin"
SRC_PATH="${SCRIPT_DIR}/${PLUGIN_NAME}"
TARGET_DIR="${HOME}/Library/Application Support/obs-studio/plugins"

echo "=========================================="
echo "      OBS AirPlay Plugin Installer        "
echo "=========================================="
echo ""

# Force close OBS Studio to release the plugin lock from dynamic memory
if pgrep -x "obs" >/dev/null 2>&1 || pgrep -x "OBS" >/dev/null 2>&1 || pgrep -x "obs-studio" >/dev/null 2>&1; then
  echo "Closing OBS Studio to overwrite the loaded plugin..."
  killall -9 "OBS" >/dev/null 2>&1 || true
  killall -9 "obs" >/dev/null 2>&1 || true
  sleep 1.5
fi

if [[ ! -d "${SRC_PATH}" ]]; then
  echo "Error: Could not find ${PLUGIN_NAME} at ${SRC_PATH}"
  exit 1
fi

echo "Installing plugin to: ${TARGET_DIR}..."
mkdir -p "${TARGET_DIR}"
rm -rf "${TARGET_DIR}/${PLUGIN_NAME}"
cp -R "${SRC_PATH}" "${TARGET_DIR}/"

echo "Bypassing macOS security Gatekeeper quarantine..."
# Strip macOS quarantine flags on the bundle, helper, plugins, and dynamic libraries recursively
xattr -cr "${TARGET_DIR}/${PLUGIN_NAME}" >/dev/null 2>&1 || true

# Ensure the bundled binaries and all dynamic libraries are fully executable
chmod +x "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/obs-airplay-source" >/dev/null 2>&1 || true
chmod +x "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/uxplay" >/dev/null 2>&1 || true
chmod +x "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/gstreamer-1.0/"*.dylib >/dev/null 2>&1 || true
chmod +x "${TARGET_DIR}/${PLUGIN_NAME}/Contents/Frameworks/"*.dylib >/dev/null 2>&1 || true

echo "Signing plugin components..."
# Sign each dylib in Frameworks individually
for f in "${TARGET_DIR}/${PLUGIN_NAME}/Contents/Frameworks/"*.dylib; do
  if [[ -f "$f" ]]; then
    codesign --force --sign - "$f" >/dev/null 2>&1 || true
  fi
done

# Sign each dylib in gstreamer-1.0 plugins folder individually
for f in "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/gstreamer-1.0/"*.dylib; do
  if [[ -f "$f" ]]; then
    codesign --force --sign - "$f" >/dev/null 2>&1 || true
  fi
done

# Sign the helper executable uxplay
codesign --force --sign - "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/uxplay" >/dev/null 2>&1 || true

# Temporarily move gstreamer-1.0 out to sign the main bundle, then move it back
if [[ -d "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/gstreamer-1.0" ]]; then
  mv "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/gstreamer-1.0" "${TARGET_DIR}/${PLUGIN_NAME}/gstreamer-1.0-temp"
  codesign --force --sign - "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/obs-airplay-source" >/dev/null 2>&1 || true
  codesign --force --sign - "${TARGET_DIR}/${PLUGIN_NAME}" >/dev/null 2>&1 || true
  mv "${TARGET_DIR}/${PLUGIN_NAME}/gstreamer-1.0-temp" "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/gstreamer-1.0"
else
  codesign --force --sign - "${TARGET_DIR}/${PLUGIN_NAME}/Contents/MacOS/obs-airplay-source" >/dev/null 2>&1 || true
  codesign --force --sign - "${TARGET_DIR}/${PLUGIN_NAME}" >/dev/null 2>&1 || true
fi

# Pure Bash user.ini Custom Browser Dock Injector (Zero dependencies, no Python)
USER_INI="${HOME}/Library/Application Support/obs-studio/user.ini"
if [[ -f "${USER_INI}" ]]; then
  echo "Automatically configuring Custom Browser Dock in OBS settings..."
  TEMP_INI="${USER_INI}.tmp"
  rm -f "${TEMP_INI}"
  
  in_basic_window=0
  added_dock=0
  
  while IFS= read -r line || [[ -n "$line" ]]; do
    # Strip carriage return
    line="${line//$'\r'/}"
    
    if [[ "$line" =~ ^\[BasicWindow\] ]]; then
      in_basic_window=1
      echo "$line" >> "${TEMP_INI}"
      continue
    elif [[ "$line" =~ ^\[ && "$line" != "[BasicWindow]" ]]; then
      if [[ $in_basic_window -eq 1 && $added_dock -eq 0 ]]; then
        echo 'ExtraBrowserDocks=[{"title":"AirPlay Receiver","url":"http://127.0.0.1:8799/","uuid":"obsairplayreceiverdock"}]' >> "${TEMP_INI}"
        added_dock=1
      fi
      in_basic_window=0
    fi
    
    if [[ $in_basic_window -eq 1 && "$line" =~ ^ExtraBrowserDocks= ]]; then
      # Automatically merge/inject custom browser dock safely
      echo 'ExtraBrowserDocks=[{"title":"AirPlay Receiver","url":"http://127.0.0.1:8799/","uuid":"obsairplayreceiverdock"}]' >> "${TEMP_INI}"
      added_dock=1
    else
      echo "$line" >> "${TEMP_INI}"
    fi
  done < "${USER_INI}"
  
  if [[ $added_dock -eq 0 ]]; then
    # If BasicWindow section didn't exist or didn't contain ExtraBrowserDocks, append at the end
    echo "" >> "${TEMP_INI}"
    echo "[BasicWindow]" >> "${TEMP_INI}"
    echo 'ExtraBrowserDocks=[{"title":"AirPlay Receiver","url":"http://127.0.0.1:8799/","uuid":"obsairplayreceiverdock"}]' >> "${TEMP_INI}"
  fi
  
  mv "${TEMP_INI}" "${USER_INI}"
fi

echo ""
echo "Success! OBS AirPlay Plugin is now installed and the controls dock is registered."
echo "You can close this window and launch/restart OBS Studio."

# Native popup message
osascript -e 'display dialog "OBS AirPlay Plugin installed successfully!\n\nThe controls dock has been configured automatically.\n\nRestart OBS Studio and add \"AirPlay Video\" or \"AirPlay Audio\" source to start." with title "Installation Complete" buttons {"OK"} default button "OK"' >/dev/null 2>&1 || true
