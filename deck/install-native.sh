#!/usr/bin/env bash
# Installs the native Linux build of From Dust on the Steam Deck (run on the Deck, as the deck
# user; the PC sends the files over SSH):
#   install-native.sh BIN_TAR [GAME_DIR]
# BIN_TAR is fromdust-linux-bin.tar.gz (scripts/build.sh's install folder without the game data,
# plus the save). The game data comes from GAME_DIR, or from an earlier install of either port
# (~/Games/FromDust*/game), linked rather than copied. Then it adds "From Dust" to Steam's
# library as a non-Steam game that runs play-native.sh with no compatibility tool, with the
# artwork from deck/art (Steam must not be running: it rewrites its shortcuts file when it exits).
set -euo pipefail
tar_file=$1
game_src=${2:-}
dest=$HOME/Games/FromDustNative
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$dest"
# Keep the save and settings of an earlier install of this folder.
keep=$(mktemp -d)
[[ -d $dest/userdata ]] && cp -a "$dest/userdata" "$keep/"
tar -xzf "$tar_file" -C "$dest" --strip-components=1
[[ -d $keep/userdata ]] && cp -a "$keep/userdata/." "$dest/userdata/"
rm -rf "$keep"
if [[ ! -e $dest/game/Default.xex ]]; then
  if [[ -z $game_src ]]; then
    for d in "$HOME"/Games/FromDust*/game; do
      [[ -e $d/Default.xex && $d != "$dest/game" ]] && { game_src=$d; break; }
    done
  fi
  [[ -n $game_src && -e $game_src/Default.xex ]] || { echo "no game data: give GAME_DIR" >&2; exit 1; }
  rm -rf "$dest/game"
  ln -s "$(readlink -f "$game_src")" "$dest/game"
fi
chmod +x "$dest/fromdust" "$dest/play-native.sh"
echo "installed in $dest (game: $(readlink -f "$dest/game"))"
ldd "$dest/fromdust" | grep "not found" && { echo "missing libraries" >&2; exit 1; }
vulkaninfo 2>/dev/null | grep -i -m1 "fragmentShaderPixelInterlock" || true
if pgrep -x steam >/dev/null; then
  echo "Steam is running: not adding the shortcut (run again with Steam closed)"
else
  # The library artwork (deck/art, sent beside this script) is kept in the install folder,
  # which the icon path points to.
  art=()
  if [[ -d $here/art ]]; then
    mkdir -p "$dest/art" && cp -f "$here"/art/*.png "$dest/art/"
    art=(--art-dir "$dest/art")
  fi
  python3 "$here/steam_shortcuts.py" add "From Dust" "$dest/play-native.sh" --start-dir "$dest" "${art[@]}"
fi
