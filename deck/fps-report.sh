#!/usr/bin/env bash
# Frame rate spread of the D3D12 port's last session on the Deck (FromDust.log: one
# "gpu: N fps" line per second), plus whether it is running now.
log=$HOME/Games/FromDustD3D12/FromDust.log
pgrep -f 'FromDust\.exe' >/dev/null && echo "running now" || echo "not running"
echo "log: $(stat -c '%y' "$log" | cut -c1-19), $(grep -c ' fps,' "$log") seconds of frames"
grep -oE 'gpu: [0-9]+ fps' "$log" | awk '{print $2}' | sort -n | uniq -c | sort -k2 -n | tail -12
