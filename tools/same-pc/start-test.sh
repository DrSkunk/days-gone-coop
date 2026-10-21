#!/usr/bin/env bash
# Same-PC co-op test: points the mod at a simulated partner on this machine and launches Days Gone.
# Usage: ./start-test.sh [follow|circle|still]   (default: follow)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
game_dir="${DAYS_GONE_DIR:-$HOME/.local/share/Steam/steamapps/common/Days Gone/BendGame/Binaries/Win64}"
ini="$game_dir/TheysGone.ini"
mode="${1:-follow}"
case "$mode" in
  follow) peer_args=(--follow --speed 700) ;;
  circle) peer_args=(--speed 300) ;;
  still)  peer_args=(--speed 0) ;;
  *) echo "usage: $0 [follow|circle|still]" >&2; exit 2 ;;
esac
[ -f "$ini" ] || { echo "TheysGone.ini not found in $game_dir" >&2; exit 1; }
[ -f "$ini.host-backup" ] || cp "$ini" "$ini.host-backup"
sed -i -e 's/^PeerHost=.*/PeerHost=127.0.0.1/' -e 's/^PeerPort=.*/PeerPort=27778/' "$ini"

systemctl --user stop theys-gone-fakepeer 2>/dev/null || true
systemctl --user reset-failed theys-gone-fakepeer 2>/dev/null || true
systemd-run --user --unit=theys-gone-fakepeer --collect \
  -p StandardOutput="truncate:$here/fake_peer.log" -p StandardError="append:$here/fake_peer.log" \
  python3 -u "$here/fake_peer.py" --game 127.0.0.1:27777 --port 27778 "${peer_args[@]}" >/dev/null
echo "simulated partner running ($mode mode), log: $here/fake_peer.log"

if pgrep -f 'DaysGone.exe' >/dev/null; then
  echo "Days Gone is already running; restart it if the mod was loaded with another PeerHost."
else
  steam steam://rungameid/1259420 >/dev/null 2>&1 &
  disown
  echo "launching Days Gone..."
fi
echo "Load your save; the partner appears near you. F7 = teleport to partner. Stop with $here/stop-test.sh"
