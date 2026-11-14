<div align="center">

# From Dust for the Steam Deck

**The Xbox 360 (XBLA) version of From Dust, recompiled into a native program and running at
60 FPS on the Steam Deck.**

![Steam Deck](https://img.shields.io/badge/Steam%20Deck-60%20FPS-1A9FFF?logo=steamdeck&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)
![Game files](https://img.shields.io/badge/game%20files-not%20included-lightgrey)

</div>

This is **not an emulator**. The game's PowerPC code is translated ahead of time into C++ with
the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and built into a normal Windows
program, which the Steam Deck runs under Proton. Its graphics are drawn by a native Direct3D 12
renderer instead of an emulated Xbox 360 GPU.

> [!IMPORTANT]
> **You need your own copy of the game.** Nothing from the game is in this repository: no
> executables, data, saves or artwork. The recompiled code is generated on your computer, from
> your copy of From Dust for Xbox 360 Live Arcade (title ID `58410A91`).

> [!NOTE]
> **Made with AI.** Most of the code and the investigation were done by Claude (Anthropic's AI),
> directed, tested and played on the Deck by the repository's owner.

## Features

- **60 FPS on the Steam Deck**, with the world (water, lava, vegetation, villagers) still at the
  original 30 steps a second, so nothing runs faster than on the console.
- **Native D3D12 renderer**: [ReXNative](https://github.com/gtguerreiro-max/ReXNative), the
  universal version of [PGR4Native](https://github.com/shaunsingh/pgr4-native)'s renderer that
  this port started from: no EDRAM emulation, each draw done once. On the Deck the GPU stays
  around 80 % at ~940 MHz and ~3 W.
- **Picture:** the game's 1024x600 shown at the console's 16:9 and scaled with **AMD FSR 1**,
  16x anisotropic filtering, the console's 2x MSAA (or off, 4x, 8x).
- **In-game settings menu** over the running game: **Back + Start** on the pad, or **F1**.
- **Steam integration:** a library entry with artwork, controller through Steam Input.
- Fixes for running above 30 FPS: the camera's 30 Hz shake, the water's texture flow, objects
  vanishing for a frame. See [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Status

Played on a Steam Deck (LCD) in Game Mode, from the first levels on. Expect rough edges: this
is a fan project. The native Linux (Vulkan) build in `scripts/build.sh` is older and not
maintained; the Proton build is the one to use.

## What you need

- **From Dust for Xbox 360 (XBLA)**, extracted from its package so that the folder holds
  `Default.xex`, the `Galileo*.xex` modules and the game's data.
- **A Windows 10/11 PC** to build it: Visual Studio 2022 Build Tools (clang-cl, lld, Windows
  SDK), CMake 3.25+, Ninja, Python 3 and Git.
- The [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.x
  headers (the renderer was built with commit `60f4ea8`) and NVIDIA's DLSS SDK headers, which
  the renderer's sources include. Their paths are CMake cache variables
  (`FROMDUST_FIDELITYFX_SDK`, `FROMDUST_DLSS_SDK`, `FROMDUST_DLSS_NR`).
- **A Steam Deck** with Proton 11 or GE-Proton 11.

## Build

```powershell
git clone --recurse-submodules https://github.com/gtguerreiro-max/FromDustDeck
cd FromDustDeck
# Put (or link) your extracted game in game-data\ so that game-data\Default.xex exists.
scripts\build.ps1 -CopyGame
```

The first build fetches the ReXGlue SDK at the commit in `patches/rexglue.lock`, applies the
patches in `patches/deck` and `patches/fromdust`, builds it, generates the game's code and
builds the game into `build\FromDust`. After a change to the port itself,
`scripts\build.ps1 -Stage game,install` is enough.

## Install on the Steam Deck

1. Copy `build\FromDust` to the Deck, for example to `~/Games/FromDust` (a USB stick formatted
   exFAT works, or the network).
2. Put `deck/play-native-d3d12.sh` beside `fromdust.exe`, and `deck/fromdust-native-d3d12.toml`
   there as `fromdust.toml`.
3. In Desktop Mode, add it to Steam: **Games > Add a Non-Steam Game**, pick
   `play-native-d3d12.sh` (file type "All files"), and leave the compatibility tool **unset**:
   the script starts Proton itself, with a prefix of its own in the game folder.
   `deck/steam_shortcuts.py` can add the entry from a terminal instead (with Steam closed), with
   artwork if you give it a folder of your own images (`--art-dir`).
4. Back in Game Mode, start it. The first minutes may stutter while shaders compile; they are
   kept for later runs. Logs go to `logs/` in the game folder.

In the Quick Access menu, leave the frame limit off or at 60. Avoid 40 and 45: the world runs
at 30 steps a second, and those rates don't divide it evenly.

## Settings

Open the menu with **Back + Start** (or **F1**). D-pad up/down chooses, left/right or A
changes, B closes; touch and mouse work too. Changes are saved into `fromdust.toml`, keeping
its comments.

| Setting | Choices | |
|---|---|---|
| Antialiasing | Off, MSAA 2x (console), 4x, 8x | live |
| Upscaling | Bilinear, AMD FSR 1 | live |
| FSR sharpening | Soft, Medium, Sharp, Sharpest | live |
| Texture filtering | Game's, 2x to 16x anisotropic | next start |
| Frame rate | 60 or 30 FPS | live |
| Frame latency | 1 to 3 frames queued (fewer: quicker controls) | next start |
| Occlusion queries | Off (default) or On | next start |

Every setting is also a line in `fromdust.toml`, and a Steam launch option such as
`%command% --rexnative_fsr=false` overrides one for a session.

## Credits and licences

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): the recompiler and runtime, built on
  [Xenia](https://github.com/xenia-project/xenia) (BSD 3-Clause).
- [ReXNative](https://github.com/gtguerreiro-max/ReXNative): the native D3D12 renderer and its
  settings menu (MIT), built from the `third_party/ReXNative` submodule.
- [PGR4Native](https://github.com/shaunsingh/pgr4-native): where that renderer comes from, and
  the Steam Deck patches this port adapts.
- AMD FidelityFX FSR 1 (MIT), through Xenia's presenter shaders; Dear ImGui (MIT).
- From Dust is © Ubisoft. This project is not affiliated with or endorsed by Ubisoft or
  Microsoft.

How it works, what it took and how to debug it: [docs/TECHNICAL.md](docs/TECHNICAL.md).
