#!/usr/bin/env bash
# Starts the native Linux build of From Dust (Vulkan, no Proton). Steam's "Add a Non-Steam Game"
# can add this script, or the `fromdust` executable directly; neither needs a compatibility tool.
#
#   play-native.sh [game flags...]      e.g. play-native.sh --draw_resolution_scale_x=2
#
# FROMDUST_DEBUG=1 adds debug logging. Logs: ./logs (the newest 10).
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
cd "$here"
mkdir -p logs cache/mesa
# Mesa's shader cache beside the game, so it survives cleaning ~/.cache.
export MESA_SHADER_CACHE_DIR=$here/cache/mesa MESA_SHADER_CACHE_MAX_SIZE=2G
args=("$@")
[[ ${FROMDUST_DEBUG:-0} == 1 ]] && args=(--log_level=debug "${args[@]}")
log=logs/fromdust-$(date +%Y%m%d-%H%M%S).log
# (|| true: ls fails while there are no logs yet, which pipefail would turn into an exit.)
ls -1t logs/fromdust-*.log 2>/dev/null | tail -n +10 | xargs -r rm -f || true
exec ./fromdust --log_file="$log" "${args[@]}"
