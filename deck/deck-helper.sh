#!/usr/bin/env bash
# Helpers run on the Deck over SSH (from the PC), kept in a file so a pkill pattern never
# matches the SSH command line itself.
#   deck-helper.sh stop      stop the D3D12 port's test run (and its wineserver)
#   deck-helper.sh xenv      print the desktop session's DISPLAY / WAYLAND_DISPLAY / XAUTHORITY
set -u
case ${1:-} in
  stop)
    for pid in $(pgrep -f 'FromDustD3D12/FromDust\.exe|FromDustD3D12\\FromDust\.exe'); do
      [ "$pid" != "$$" ] && kill "$pid" 2>/dev/null
    done
    sleep 2
    WINEPREFIX=$HOME/Games/FromDustD3D12/prefix/pfx \
      "$HOME/.local/share/Steam/steamapps/common/Proton 11.0/files/bin/wineserver" -k 2>/dev/null
    pgrep -a -f 'FromDust\.exe' | grep -v deck-helper || echo "stopped"
    ;;
  xenv)
    for name in kwin_wayland plasmashell steam; do
      for p in $(pgrep -u "$(id -u)" -x "$name"); do
        tr '\0' '\n' <"/proc/$p/environ" | grep -E '^(XAUTHORITY|DISPLAY|WAYLAND_DISPLAY)='
      done
    done | sort -u
    ls -la /run/user/"$(id -u)" | grep -i -E 'xauth|wayland' || true
    ;;
esac
