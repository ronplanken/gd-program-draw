#!/bin/bash
set -euo pipefail
package_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
if /usr/bin/pgrep -x OBS >/dev/null 2>&1; then
  echo "Quit OBS first, then run this launcher again."
  exit 1
fi
if [[ -e "$HOME/Library/Application Support/obs-studio/plugins/obs-program-draw.plugin" ]]; then
  echo "Program Draw is already installed. Launch OBS normally to avoid loading a duplicate."
  exit 1
fi
echo "Starting OBS with Program Draw for this launch. It uses your normal OBS configuration."
export OBS_PLUGINS_PATH="$package_dir"
export OBS_PLUGINS_DATA_PATH="$package_dir"
exec /Applications/OBS.app/Contents/MacOS/OBS --disable-updater
