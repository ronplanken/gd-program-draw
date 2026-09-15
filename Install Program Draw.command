#!/bin/bash
set -euo pipefail
package_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
bundle_path="$package_dir/obs-program-draw.plugin"
plugin_dir="$HOME/Library/Application Support/obs-studio/plugins"
if /usr/bin/pgrep -x OBS >/dev/null 2>&1; then
  echo "Quit OBS before installing Program Draw, then run this installer again."
  exit 1
fi
if [[ ! -f "$bundle_path/Contents/MacOS/obs-program-draw" ]]; then
  echo "The plugin bundle is missing. Keep this installer next to obs-program-draw.plugin."
  exit 1
fi
if [[ "$(uname -m)" != "arm64" ]]; then
  echo "This build requires an Apple Silicon Mac."
  exit 1
fi
obs_version="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' /Applications/OBS.app/Contents/Info.plist)"
if [[ "$obs_version" != "32.2.2" ]]; then
  echo "This POC was built for OBS 32.2.2. Found $obs_version. Rebuild for that version before installing."
  exit 1
fi
/usr/bin/codesign --verify --deep --strict "$bundle_path"
/bin/mkdir -p "$plugin_dir"
if [[ -e "$plugin_dir/obs-program-draw.plugin" ]]; then
  /bin/mv "$plugin_dir/obs-program-draw.plugin" "$plugin_dir/obs-program-draw.plugin.backup-$(date +%Y%m%d-%H%M%S)"
fi
/usr/bin/ditto "$bundle_path" "$plugin_dir/obs-program-draw.plugin"
echo "Installed Program Draw POC. Reopen OBS and click Draw in the toolbar beneath the live monitor. Studio Mode is optional."
