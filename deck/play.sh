#!/usr/bin/env bash
# Starts From Dust (ReXGlue port, fromdust.exe beside this script) under Proton on a Steam Deck.
# Steam's "Add a Non-Steam Game" can list this script directly; give it no compatibility tool,
# since the script starts Proton itself. Steam can also run fromdust.exe directly with Proton
# Experimental or GE-Proton as its compatibility tool.
#
#   play.sh [--gamescope] [game flags...]     e.g. play.sh --draw_resolution_scale_x=2
#
# PROTON=<dir with ./proton> picks the Proton. By default the script uses the newest GE-Proton
# in compatibilitytools.d, else Proton Experimental. FROMDUST_PREFIX sets the Wine prefix
# (default ./prefix). FROMDUST_DEBUG=1 turns on Wine and game debug logging.
# Logs: ./logs (the newest 10). Shader caches: ./cache (kept between runs).
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)

steam=$HOME/.local/share/Steam
[[ -d $steam ]] || steam=$HOME/.steam/steam
if [[ -n ${PROTON:-} ]]; then
  proton=$PROTON
else
  proton=$(ls -d "$HOME"/.steam/steam/compatibilitytools.d/GE-Proton*/ \
    "$steam"/compatibilitytools.d/GE-Proton*/ 2>/dev/null |
    awk -F/ '{print $(NF-1) "\t" $0}' | sort -V | tail -n1 | cut -f2)
  [[ -n $proton ]] || proton="$steam/steamapps/common/Proton - Experimental"
fi
proton=${proton%/}
[[ -x $proton/proton ]] || { echo "No Proton found at '$proton' (set PROTON=<dir>)" >&2; exit 1; }

prefix=${FROMDUST_PREFIX:-$here/prefix}
mkdir -p "$prefix" "$here/logs" "$here/cache/vkd3d" "$here/cache/dxvk" "$here/cache/mesa"
for e in /proc/[0-9]*/environ; do
  if grep -qxzF "STEAM_COMPAT_DATA_PATH=$prefix" "$e" 2>/dev/null; then
    echo "From Dust is already running in $prefix" >&2; exit 1
  fi
done
export STEAM_COMPAT_DATA_PATH=$prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam
# Shader and pipeline caches beside the game, so a rebuild of the prefix keeps them.
export VKD3D_SHADER_CACHE_PATH=$here/cache/vkd3d DXVK_STATE_CACHE_PATH=$here/cache/dxvk
export MESA_SHADER_CACHE_DIR=$here/cache/mesa MESA_SHADER_CACHE_MAX_SIZE=2G

game_args=()
wrap=()
if [[ ${1:-} == --gamescope ]]; then
  shift
  # Inside Game Mode gamescope is already the compositor.
  [[ -n ${GAMESCOPE_WAYLAND_DISPLAY:-} ]] || wrap=(gamescope -W 1280 -H 800 -w 1280 -h 800 -f --)
fi
if [[ ${FROMDUST_DEBUG:-0} == 1 ]]; then
  export WINEDEBUG=+seh,+loaddll
  game_args+=(--log_level=debug)
else
  export WINEDEBUG=-all
fi
game_args+=("$@")

log=$here/logs/fromdust-$(date +%Y%m%d-%H%M%S).log
ls -1t "$here"/logs/fromdust-*.log 2>/dev/null | tail -n +10 | xargs -r rm -f
cd "$here"
set +e
"${wrap[@]}" "$proton/proton" run "$here/fromdust.exe" "${game_args[@]}" >"$log" 2>&1
code=$?
set -e
exit $code
