#!/usr/bin/env bash
# Stops the simulated partner and restores the normal (host-mode) co-op ini.
set -uo pipefail
game_dir="${DAYS_GONE_DIR:-$HOME/.local/share/Steam/steamapps/common/Days Gone/BendGame/Binaries/Win64}"
ini="$game_dir/TheysGone.ini"
systemctl --user stop theys-gone-fakepeer 2>/dev/null && echo "simulated partner stopped"
if [ -f "$ini.host-backup" ]; then mv "$ini.host-backup" "$ini"; echo "restored $ini"; fi
echo "Restart Days Gone for the ini change to take effect."
