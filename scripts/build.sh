#!/usr/bin/env bash
# Builds From Dust natively for Linux (Steam Deck: SteamOS, Vulkan, no Proton), with clang.
# Written for Arch Linux (SteamOS is Arch based): WSL on the PC (`wsl --install archlinux`) or a
# distrobox on the Deck.
#
#   scripts/build.sh [--stage deps,sdk,game,install] [--dest DIR] [--game DIR] [--jobs N]
#
# Stages (default: sdk,game,install; deps installs the Arch packages with pacman, as root):
#   sdk      Copies the ReXGlue tree this project builds on (build/rexglue-sdk of the Windows
#            checkout: PGR4Native's tree plus patches/) into $dest/rexglue-sdk, then builds and
#            installs it in $dest/sdk-install with the Vulkan backend and FidelityFX (FSR).
#   game     Runs tools/gen_manifest.py (codegen with this SDK's rexglue) and builds the game
#            and its six module libraries in $dest/game.
#   install  $dest/FromDust: the executable, the modules, the SDK libraries (found through an
#            $ORIGIN rpath), fromdust.toml with the Linux settings, play-native.sh, and the game
#            data in game/ (copied from --game). Copy that folder to the Deck.
#
# The project and its sources stay on the Windows drive (/mnt/f/FromDustDeck); the build tree
# defaults to the Linux file system ($HOME/fromdust-linux), which builds far faster than /mnt.
#
# CPU: -march=x86-64-v3 -mtune=znver2 -mprefer-vector-width=128 (the Deck's Zen 2; see
# scripts/build.ps1). libstdc++ is linked statically, so the Deck needs only glibc, libm,
# libgcc_s and the Vulkan loader; the install stage prints the newest glibc symbol version the
# binaries need, to compare with `ldd --version` on the Deck.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
stages=sdk,game,install
dest=$HOME/fromdust-linux
game=/mnt/f/FromDustRecomp/game
jobs=$(nproc)
while [[ $# -gt 0 ]]; do
  case $1 in
    --stage) stages=$2; shift 2 ;;
    --dest) dest=$2; shift 2 ;;
    --game) game=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    -h|--help) sed -n '2,/^set -euo/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
has() { [[ ",$stages," == *",$1,"* ]]; }
mkdir -p "$dest/logs"

# deck/glibc_compat.h binds libm functions that newer glibc versions re-versioned to their old
# versions, so the binaries load on the Deck's older glibc.
flags="-march=x86-64-v3 -mtune=znver2 -mprefer-vector-width=128 -ffp-model=precise -fno-math-errno -include $repo/deck/glibc_compat.h"
deck_glibc=2.41
# Not -static-libgcc: glibc's pthread_exit unwinds through the system libgcc_s.so.1, and a
# second, static unwinder in the libraries aborts there (_Unwind_SetGR, on every thread exit).
link="-static-libstdc++ -fuse-ld=lld"

if has deps; then
  # ReXGlue's Linux docs list clang (18+), cmake, ninja, gtk3 and vulkan-devel on Arch.
  pacman -Syu --needed --noconfirm base-devel clang lld llvm cmake ninja python git rsync gtk3 \
    vulkan-devel vulkan-headers vulkan-icd-loader shaderc glslang spirv-tools \
    libx11 libxext libxrandr libxi libxcursor libxss libxtst libxkbcommon wayland \
    wayland-protocols libdecor alsa-lib libpulse pipewire mesa
fi

if has sdk; then
  # The Windows checkout's tree, without its Windows build output.
  rsync -a --delete --exclude out/ --exclude '.git/index.lock' "$repo/build/rexglue-sdk/" "$dest/rexglue-sdk/"
  # FidelityFX (FSR): reuse the Windows build's checkout instead of cloning it again (the
  # clone of that large repository stalls in WSL).
  # Only its headers are used (FSR 1 and CAS, with prebuilt shaders): the FSR 2/3 runtime
  # needs FidelityFX_SC.exe, a Windows tool, for its shader permutations.
  ffx=(-DREXGLUE_FIDELITYFX_RUNTIME=OFF)
  if [[ -d "$repo/build/fidelityfx-src" ]]; then
    rsync -a "$repo/build/fidelityfx-src/" "$dest/fidelityfx-src/"
    ffx+=(-DFETCHCONTENT_SOURCE_DIR_FIDELITYFX="$dest/fidelityfx-src")
  fi
  cmake -S "$dest/rexglue-sdk" -B "$dest/sdk" -G Ninja -DCMAKE_BUILD_TYPE=Release "${ffx[@]}" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$link" -DCMAKE_SHARED_LINKER_FLAGS="$link" \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_USE_VULKAN=ON \
    -DCMAKE_INSTALL_PREFIX="$dest/sdk-install" >"$dest/logs/sdk-configure.log" 2>&1 ||
    { tail -30 "$dest/logs/sdk-configure.log"; exit 1; }
  cmake --build "$dest/sdk" --target install -j "$jobs" >"$dest/logs/sdk-build.log" 2>&1 ||
    { grep -m 20 -E "error|FAILED" "$dest/logs/sdk-build.log"; exit 1; }
fi

if has game; then
  # The project's sources, with the game data where the manifest expects it.
  src=$dest/project
  rsync -a --delete --exclude build/ --exclude generated/ --exclude game-data --exclude .git/ \
    "$repo/" "$src/"
  ln -sfn "$game" "$src/game-data"
  # gen_manifest.py reads the D3D12 port's function bounds and images from /mnt/f.
  (cd "$src" && python3 tools/gen_manifest.py --rexglue "$dest/sdk-install/bin/rexglue") ||
    { tail -20 "$src/codegen.log"; exit 1; }
  cmake -S "$src" -B "$dest/game" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$link" -DCMAKE_SHARED_LINKER_FLAGS="$link" \
    -DCMAKE_BUILD_RPATH='$ORIGIN' -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    -DCMAKE_PREFIX_PATH="$dest/sdk-install" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
    >"$dest/logs/game-configure.log" 2>&1 || { tail -30 "$dest/logs/game-configure.log"; exit 1; }
  cmake --build "$dest/game" -j "$jobs" >"$dest/logs/game-build.log" 2>&1 ||
    { grep -m 20 -E "error|FAILED" "$dest/logs/game-build.log"; exit 1; }
fi

if has install; then
  out=$dest/FromDust
  mkdir -p "$out"
  find "$dest/game" -maxdepth 2 \( -name 'fromdust' -o -name 'libfromdust_*.so' -o -name 'fromdust_*.so' \) \
    -type f -exec cp -f {} "$out/" \;
  cp -f "$dest"/sdk-install/lib/*.so* "$dest"/sdk-install/bin/*.so* "$out/" 2>/dev/null || true
  cp -f "$repo/deck/fromdust-linux.toml" "$out/fromdust.toml"
  cp -f "$repo/deck/play-native.sh" "$out/"
  chmod +x "$out/fromdust" "$out/play-native.sh"
  [[ -d $out/game ]] || cp -r "$game" "$out/game"
  echo "Installed in $out:"
  ls -la "$out" | grep -v '^d'
  echo "Newest glibc symbol versions needed (compare with 'ldd --version' on the Deck):"
  for f in "$out"/fromdust "$out"/*.so*; do
    objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' || true
  done | sort -u -V | tail -3
  newest=$(for f in "$out"/fromdust "$out"/*.so*; do objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' || true; done |
           sed 's/GLIBC_//' | sort -u -V | tail -1)
  if [[ $(printf '%s\n%s\n' "$newest" "$deck_glibc" | sort -V | tail -1) != "$deck_glibc" ]]; then
    echo "glibc $newest is newer than the Deck's $deck_glibc: add the symbols to deck/glibc_compat.h:" >&2
    for f in "$out"/fromdust "$out"/*.so*; do
      objdump -T "$f" 2>/dev/null | awk -v f="${f##*/}" -v v="GLIBC_$newest" '$0 ~ "\\(" v "\\)" {print f, $NF}'
    done >&2
    exit 1
  fi
fi
