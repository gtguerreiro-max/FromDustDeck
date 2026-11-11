#!/usr/bin/env bash
# Plays From Dust with the native D3D12 renderer (the Windows build, ~/Games/FromDustNativeD3D)
# under Proton on the Steam Deck, like PGR4Native. Add this script to Steam as a non-Steam game
# with NO compatibility tool: it starts Proton itself, with a prefix of its own in this folder.
#   play-native-d3d12.sh [game flags...]
# FD_PROTON picks another Proton (default: the GE-Proton PGR4 uses, else Valve's Proton 11.0).
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
steam=$HOME/.local/share/Steam
proton=${FD_PROTON:-}
if [[ -z $proton ]]; then
  for p in "$steam"/compatibilitytools.d/GE-Proton11-*/ "$steam/steamapps/common/Proton 11.0/"; do
    [[ -x $p/proton ]] && { proton=${p%/}; break; }
  done
fi
export STEAM_COMPAT_DATA_PATH=$here/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam
export PROTON_USE_XALIA=0
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$here/logs"
cd "$here"
log=logs/fromdust-$(date +%Y%m%d-%H%M%S).log
ls -1t logs/fromdust-*.log 2>/dev/null | tail -n +10 | xargs -r rm -f || true
exec "$proton/proton" run "$here/fromdust.exe" "--log_file=$log" "$@"
