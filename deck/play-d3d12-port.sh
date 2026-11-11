#!/usr/bin/env bash
# Plays From Dust (the D3D12 port, F:\FromDustRecomp) on the Steam Deck under Proton.
# Add this script to Steam with "Add a Non-Steam Game" and give it NO compatibility tool: the
# script starts Proton itself, with its own prefix in this folder.
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
proton="${PROTON:-$HOME/.local/share/Steam/steamapps/common/Proton 11.0}"
export STEAM_COMPAT_DATA_PATH=$here/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
mkdir -p "$STEAM_COMPAT_DATA_PATH"
cd "$here"
exec "$proton/proton" run "$here/FromDust.exe" 'Z:\home\deck\Games\FromDust\game'
