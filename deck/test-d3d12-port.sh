#!/usr/bin/env bash
# Runs the D3D12 port (F:\FromDustRecomp, copied to ~/Games/FromDustD3D12) under Proton on the
# Deck's desktop session, hands-free: loads the save with scripted presses, saves a capture at
# CAPTURE_AT seconds and exits at EXIT_AT. For checking it from the PC over SSH.
#   test-d3d12-port.sh [capture.png] [capture seconds] [exit seconds]
set -u
here=$HOME/Games/FromDustD3D12
proton="${PROTON:-$HOME/.local/share/Steam/steamapps/common/Proton 11.0}"
out=${1:-$here/capture.png}
export STEAM_COMPAT_DATA_PATH=$here/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export DISPLAY=:0 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000
# The desktop session's X authorization (an SSH session has none, and Xwayland refuses it).
XAUTHORITY=$(ls -t /run/user/1000/xauth_* 2>/dev/null | head -1)
export XAUTHORITY
mkdir -p "$STEAM_COMPAT_DATA_PATH"
export FD_AUTOPRESS='30:0010:200,34:0010:200,38:0010:200,42:0010:200,46:0010:200,52:1000:200,58:1000:200,64:1000:200,80:1000:200,90:1000:200'
export FD_CAPTURE="Z:${out//\//\\}" FD_CAPTURE_SECONDS=${2:-105} FD_EXIT_SECONDS=${3:-112}
cd "$here"
"$proton/proton" run "$here/FromDust.exe" 'Z:\home\deck\Games\FromDust\game' >"$here/test.log" 2>&1
echo "exit $?"
ls -la "$out" 2>/dev/null
grep -i "fps\|capture\|error" "$here/test.log" | tail -5
