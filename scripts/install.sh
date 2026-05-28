#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"${ROOT_DIR}/scripts/build.sh"

PLUGIN_DIR="${HOME}/Library/Application Support/obs-studio/plugins"
mkdir -p "${PLUGIN_DIR}"
rm -rf "${PLUGIN_DIR}/obs-airplay-source.plugin"
cp -R "${ROOT_DIR}/build/obs-airplay-source.plugin" "${PLUGIN_DIR}/"
"${ROOT_DIR}/scripts/fix-bundled-gstreamer.sh" "${PLUGIN_DIR}/obs-airplay-source.plugin"

codesign --force --deep --sign - "${PLUGIN_DIR}/obs-airplay-source.plugin" >/dev/null 2>&1 || true

USER_INI="${HOME}/Library/Application Support/obs-studio/user.ini"
if [[ -f "${USER_INI}" ]]; then
  /usr/bin/python3 - "${USER_INI}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
lines = text.splitlines()
section = None
found_section = False
found_key = False
dock = {
    "title": "AirPlay Receiver",
    "url": "http://127.0.0.1:8799/",
    "uuid": "obsairplayreceiverdock",
}

for i, line in enumerate(lines):
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped[1:-1]
        found_section = found_section or section == "BasicWindow"
        continue
    if section == "BasicWindow" and stripped.startswith("ExtraBrowserDocks="):
        raw = line.split("=", 1)[1]
        try:
            docks = json.loads(raw)
        except Exception:
            docks = []
        docks = [d for d in docks if d.get("uuid") != dock["uuid"] and d.get("title") != dock["title"]]
        docks.append(dock)
        lines[i] = "ExtraBrowserDocks=" + json.dumps(docks, separators=(",", ":"))
        found_key = True
        break

if not found_section:
    lines.extend(["", "[BasicWindow]", "ExtraBrowserDocks=" + json.dumps([dock], separators=(",", ":"))])
elif not found_key:
    insert_at = len(lines)
    for i, line in enumerate(lines):
        if line.strip() == "[BasicWindow]":
            insert_at = i + 1
            while insert_at < len(lines) and not (lines[insert_at].strip().startswith("[") and lines[insert_at].strip().endswith("]")):
                insert_at += 1
            break
    lines.insert(insert_at, "ExtraBrowserDocks=" + json.dumps([dock], separators=(",", ":")))

path.write_text("\n".join(lines) + "\n")
PY
fi

echo "Installed: ${PLUGIN_DIR}/obs-airplay-source.plugin"
echo "Installed OBS custom browser dock: AirPlay Receiver"
echo "Restart OBS, then add source: AirPlay Video. The dock controls the shared receiver."
