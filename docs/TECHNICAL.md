# From Dust port: technical notes

How the port works and what it took, for anyone working on it. The user-facing
overview is the [README](../README.md).

## How it targets the Deck

The game is cross-built as a Windows x64 build. It runs on the Deck under Proton, where D3D12
goes through vkd3d-proton to the Deck's RADV Vulkan driver. This is the setup that
[PGR4Native](https://github.com/shaunsingh/pgr4-native) measured on a Deck. The SDK is upstream ReXGlue `development` at `bd833a2`
(`patches/rexglue.lock`) plus 37 of PGR4Native's patches (`patches/deck/`): the generic ones for
performance and correctness (guest memory, GPU ring wake-ups, vblank pacing, XMA audio, SDL pad
polling, RTV colour clamping), and the 8 its native renderer needs from the SDK (0021, 0022,
0030, 0031 `gpu_skip_draws`, 0032, 0049 `occlusion_query_external`, 0050, 0051). Its game-side
patches (PGR4's music player, title updates, 16:10) are not used. This port's own SDK changes
are `patches/fromdust/`.

## Native renderer (D3D12, under Proton): the Deck's 60 FPS

The renderer is [ReXNative](https://github.com/gtguerreiro-max/ReXNative): PGR4Native's native
D3D12 renderer as this port adapted it, made game-neutral so the same sources run any ReXGlue
game (`third_party/ReXNative`, a submodule; CMake takes a ReXNative checkout beside this one
instead when the submodule is absent, cache `FROMDUST_REXNATIVE_DIR`). It hooks the guest
D3D library's draw, clear, resolve, query and Swap calls in the recompiled code and draws them
with host render targets, without emulating the EDRAM or the GPU's command stream, and each
draw once instead of once per predicated tiling pass. The Xenos plugin stays in the binary for
the GPU's fences and interrupts (`--gpu_skip_draws`) and as the comparison renderer
(`--rexnative_beside_xenos=true` draws both and writes both images with their PSNR). Measured on the Deck
in the first level, same scene as the Xenos/Vulkan path (33-49 FPS with the GPU at 100 % and
1591 MHz): 60 FPS with the GPU 68 % busy at 839 MHz, 2.6 W of GFX power, every world step on
screen for 2 frames.

**From Dust's layer.** Everything the renderer knows about From Dust is in `src/rexnative/`:
`game_layer.h` (the D3D function addresses, the device's field offsets, where Resolve's
arguments are, its default clear colour, the scene surface, option defaults) and
`d3d_surface.inc` (the hooked functions). ReXNative's tool writes both from the generated code
and `Default.xex`, matching them against the other ports it knows; from a ReXNative checkout:

    python -m rexnative adapt --generated <FromDustDeck>/generated/default --xex <FromDustDeck>/game-data/Default.xex --name fromdust --title "From Dust" --out <FromDustDeck>/src/rexnative

It reads `src/rexnative/overrides.json` first, the choices kept by hand: PGR4's second restore
of a push-buffer segment (`RestoreSegment`) is inlined in From Dust, so it is the same function
as `RestoreSegment13392` (`820D65A8`); and the scene surface and option defaults are the ones the
port ran with. The real scene is 1024x600, pitch 1040, 2x MSAA (`rexnative scene` on a resolve
dump), not in the layer yet: the upscaler (DLSS / FSR) stays off. The generated layer equals the
hand-made one this port had, except the default Resolve clear colour, now From Dust's 16-byte
vector at `0x82007F50` (it read PGR4's float at `0x82258A0C`). The old `src/native/`,
`src/fromdust_d3d_trace.cpp`, `src/fromdust_settings.*` and `src/d3d_surface.inc` are in the
git history. Checked on a PC on 2026-11-14 (menus, map, a level, the settings menu); not yet
on the Deck with this build.

What From Dust needed, now in ReXNative (the options keep PGR4Native's `rexnative_*` names):
- **Addresses.** Both games link the same XDK D3D library at other addresses.
  `tools/match_d3d.py` paired PGR4's functions with From Dust's by instruction listing (exact,
  then similarity, with the next best far behind), and `tools/retarget_pgr4.py` rewrote the
  copied sources and the hook list (108 functions; the setters From Dust's engine inlines have
  no copy). ReXNative's `rexnative adapt` does the same now, with more ways to match. Checked against the earlier XenonRecomp-based D3D12 port's call counts (Swap 820E0AC8,
  DrawIndexedVertices 8250DDA0, DrawVertices 8250D988, MakeSpace 820D7D88, GetData 820DE478).
  The public Resolve is `sub_820EE9D0`, a wrapper over the listing match `sub_820ED880` with two
  new arguments.
- **Device layout.** The register shadow is at PGR4's offsets, the fields after it moved
  (`tools/device_offsets.py` votes them from aligned loads and stores): index buffer 12684,
  surfaces 12688/12704, shaders 12940/12944, vertex stride 12136, tiling list 12996/13000/13240,
  fences 11036/11024.
- **Terrain height field.** k_16_16 targets (signed integer resolves with an exponent bias):
  16-bit fixed-point resolve destinations, the targets' SNORM -32..32 remap undone when they are
  resolved, and the integer texture fetch fold of the Xenos path (`integer_fetch.h`).
- **Occlusion queries: off.** From Dust hands its pool of 89 queries, in order, to whichever
  objects it tests in a frame and reads each result in that frame. The host's counts come a
  frame or more later, so a query got another object's count, and a 0 hid a visible object for
  a frame (the background showed through: the "blue" flashes on the totem and the huts, 9 frames
  in 735). Every query now reports visible (`--rexnative_occlusion=false`, the default); the
  draws per frame are the same (1460 against 1464).
- `src/native_glue.cpp` and `RexNativeGameWorldStepped` (`src/frame_rate.cpp`): the game-side
  functions the renderer calls (`rexnative_add(... NO_DEFAULT_GLUE)`).
- **Picture.** The front buffer is the game's 1024x600, which the console's scaler shows at
  16:9: the present fits it at 16:9 (`--rexnative_display_aspect`, 1280x720 on the Deck's
  1280x800) and scales it up with AMD FSR 1 (`--rexnative_fsr`, EASU then RCAS,
  `--rexnative_fsr_sharpness` in stops; ReXNative's `renderer/src/native/fsr.cpp`). FSR runs
  the pixel shaders
  ReXGlue's D3D12 presenter ships (Xenia's `guest_output_ffx_fsr_*`, prebuilt DXBC), on the
  image after the gamma ramp, as that presenter does. On the Deck it costs 6 points of GPU
  busy time (81 % at 942 MHz against bilinear's 75 % at 897 MHz, 3.0 W of GFX power against
  2.9 W), still 60 FPS.
- **Settings menu**: ReXNative's HUD (`renderer/src/hud/`, this port's menu made game-neutral,
  after PGR4Native's in-game Options page), set up in `src/fromdust_app.h`, with the pad chord
  from `src/frame_rate.cpp` (`FilterPad`). F1, or Back+Start on the pad, opens it over the
  running game. D-pad up/down chooses, left/right (or A) changes, B closes; mouse and touch work
  too. Antialiasing, screen scaling, FSR sharpening, frame rate, frame latency, texture
  filtering, a DLSS page (upscaler, DLSS mode, DLSS 5 and its intensity), occlusion queries and
  log level. A setting the
  renderer can change live applies at once, the others are marked * until the next start, and
  each is saved into `fromdust.toml` with its comments kept. The menu has an ImGui context of
  its own, used only on the UI thread (ReXGlue's ImGui is one context in `rexruntime.dll` that
  its UI thread switches); the native present draws its triangles
  (`renderer/src/native/overlay.cpp`).
  The game sees a neutral pad while it is up, and a lone Back or Start reaches the game 120 ms
  late, in case it begins the chord. For the first 15 seconds a hint in the bottom-left
  corner says how to open it (`--rexnative_hud_hint=false` hides it).

`tools/compare_resolves.py` (from PGR4Native, with the 16-bit formats) compares the renderer's
resolves with the Xenos plugin's (`--rexnative_resolve_dump`). The Deck runs the Windows build
from `~/Games/FromDustNativeD3D` with `deck/play-native-d3d12.sh` (GE-Proton) and
`deck/fromdust-native-d3d12.toml`.

- **CPU:** `-march=x86-64-v3 -mtune=znver2 -mprefer-vector-width=128`, for the Deck's Zen 2.
  FMA, AVX2 and MOVBE become single instructions. 128-bit vectors avoid the slow
  `VZEROUPPER` that the Deck's kernel causes without the Zenbleed microcode fix.
- **Image:** the game's own 1024x600, upscaled to 1280x720 (16:9 on the 1280x800 panel) with
  AMD FSR 1, EASU edge reconstruction plus RCAS sharpening, and 16x anisotropic filtering.
  ReXGlue's `fsr2`/`fsr3` modes are not used: they hand FSR the colour image as depth and as
  motion vectors, with no jitter, so they have no temporal data to work with. See
  `deck/fromdust.toml`, which documents each setting. Docked on a TV, also try
  `draw_resolution_scale_x/y = 2`.
- **60 FPS** (`src/frame_rate.cpp`, ported from the D3D12 port's `game_loop.cpp`): the guest
  vblank runs at 120 Hz, and the game presents every second one. The world (water, lava,
  plants, people) keeps its 30 steps per second, and even pacing holds 60 FPS (30 if the
  frames can't keep up), so every step stays on screen for exactly 2 frames. Buttons only
  change before a frame where the world steps, because the game reads them as edges.
- **Camera at 60 FPS** (`src/frame_rate.cpp`, `RestoreCursorHeight`): the camera follows the
  Breath cursor (dustai `sub_8A1FF3B0`). Each world Update sets the cursor's height to the ground
  under it, and the cursor's behaviour (`sub_8A1F4298`, every frame) lifts it to its hover height.
  On a frame without a world step the behaviour got its own last result instead and gave the
  ground back, so the cursor and the camera went up and down at 30 Hz (61.15 against 61.99). The
  height the world set is put back on those frames; measured after: the same height on both.
  Found with a hardware write watchpoint on the camera's matrix, then the cursor, from the
  native renderer's camera trace (`--rexnative_trace_camera`).
- **Water flow** (`src/water_flow.cpp`): the advection pass's per-draw step (`c38.y`, plus
  `c36.x` on the title screen) is scaled by the frame's share of a world step. It uses a
  per-draw hook in the SDK, and the pass is found by the constants it reads. In play `c36.x`
  is a real-time phase (measured at the same speed at 30 and 60 FPS), so it is not touched.
  The native renderer calls the same hook from its own draws (ReXNative's
  `renderer/src/native/draw.cpp`).

## Optimizations measured (Windows PC, first level from a save)

- **Register-as-local codegen** (`cr`, `ctr`, `xer`, `non_volatile`, `reserved`, as in PGR4):
  CPU per frame across all threads went from 16.32 to 15.71 ms (-3.7 %), and FPS rose 4 %,
  uncapped (`tools/bench.ps1`).
- **Presents only on new frames**: the achievement toast kept the UI repainting at the
  display's rate (480 presents/s here, 60-90 on the Deck). At 60 FPS the game used ~148 % of a
  core with it and ~119 % without (-20 %, `tools/ab60.ps1`, 2 runs each).
- Kept: the game's 2x MSAA (`native_2x_msaa`, the GPU's own MSAA, cheap on RDNA 2; FSR 1 needs
  edges without aliasing). Not used: ReXGlue's `fsr2`/`fsr3` (see above).
- `tools/bench.ps1` (uncapped: FPS and CPU ms per frame) and `tools/ab60.ps1` (60 FPS: % of a
  core, current build against another folder) load the D3D12 port's save of the first level.
  It is copied to `userdata/B13EBABEBABEBABE/58410A91/00000001/DUST/`, and the menus get real
  key holds, because SendKeys taps are too short. `license_mask = 1` is needed, or the game
  runs as its trial.

## Current state (2026-11)

- All seven XEX modules are recompiled and load. Played on a Steam Deck (LCD) under GE-Proton
  with the native renderer, from Steam in Game Mode: 60 FPS (16.7 ms frames) with the GPU about
  80 % busy at ~940 MHz and ~3 W of GFX power, the world at its 30 steps a second (the level's
  countdown loses 28 s in 27.6 s of real time at 60 FPS, 29 s in 28.5 s at 30 FPS), the
  settings menu from the Deck's own controls.
- On Windows (D3D12) the same build runs the title screen and the levels tested at 60 FPS.
- Not done: the terrain and water surfaces move at the world's 30 Hz while the camera moves at
  60 (the earlier D3D12 port interpolated the heights in its renderer). The native Linux
  (Vulkan) build is behind the Proton one and not maintained.

What it took beyond ReXGlue as it comes (`patches/fromdust/0001-fromdust-sdk-changes.patch`,
regenerated from `build/rexglue-sdk` with `tools/make_sdk_patch.sh`):

- **Ground and water (height maps).** The terrain and water are displaced by height maps the
  GPU renders into k_16_16 targets and resolves to signed-integer textures, which the shaders
  then fetch as integers with `exp_adjust = -15`. Three things in ReXGlue lost them, found with
  RenderDoc against the D3D12 port's capture:
  - The resolve shaders ignored the destination number format (`RB_COPY_DEST_INFO`) and
    packed everything as unsigned normalized, so every negative height became 0. The
    `resolve_full_*` shaders are rebuilt from a newer Xenia's sources, which honour it
    (`build/shaderfix`: fxc for D3D12, glslang and spirv-opt in WSL for Vulkan), with
    ReXGlue's typed write buffers and its EDRAM sample layout (2x: a pixel's two samples one
    above the other; the newer Xenia interleaves them, which scrambled the game's 2x MSAA
    scene into blocks).
  - Integer texture fetches (`num_format = 1`) were sampled normalized, 2^15 times too small.
    `rex/graphics/integer_fetch.h` folds the integer range into `exp_adjust` in the copy of
    the fetch constants the shaders read, on both backends. It reads the register file only:
    reading the upload buffer back (write-combined memory) cost the GPU thread half its time.

- Module paths: the game loads its DLLs from `\Device\Harddisk0\Partition1`, which the SDK did
  not match to the recompiled modules.
- `function_table_base`: GalileoPathfind_9_9's indirect-call table did not fit before the next
  module's image, so it moves to `0x87000000`.
- `vmaxuw`: the water simulation's vector max was not implemented in codegen.
- A per-draw pixel constant hook (`rex/graphics/draw_hook.h`), for the water flow.
- FidelityFX (FSR) built with clang-cl: its warnings stay warnings, and its UTF-16 `.rc` is
  left out.
- The world gate starts only after the world has run for 5 s with no frame over 100 ms. While
  a world loads, other code reads what its updates fill in, and skipping them crashed most
  starts.
- Function starts: `tools/gen_manifest.py --seed-old` seeds the function starts the XenonRecomp
  port found that something points to. That covers vtables and callbacks, export thunks and
  addresses built in code, provided the instruction before each start can't fall through into
  it. Seeding every old start split functions at internal labels.

Debugging aids: `crash.txt` (host stack on abort, resolved with `tools/map_symbolize.py` and
the `.map` files in `build\game`), `tools/why_missing.py <addr>` for an "unregistered function"
crash, and `tools/drive.ps1` to run the game with timed key presses (`mnk_mode`: Return =
Start, Space = A) and window captures.

## Layout

| Path | Contents |
|---|---|
| `fromdust_manifest.toml` | Codegen manifest, generated by `tools/gen_manifest.py`; do not edit |
| `tools/hints/` | Function starts that codegen does not find by itself, one file per module (made by `tools/gen_manifest.py --seed-old`) |
| `patches/` | `rexglue.lock` (upstream commit), `deck/` (generic patches from PGR4Native), `fromdust/` (this port's SDK changes) |
| `src/` | The app (`FromdustApp`, a `rex::ReXApp`) |
| `src/rexnative/` | From Dust's layer for the native renderer: `game_layer.h`, `d3d_surface.inc` (generated by `rexnative adapt`) and `overrides.json` |
| `third_party/ReXNative` | The native D3D12 renderer and its settings menu ([ReXNative](https://github.com/gtguerreiro-max/ReXNative), submodule) |
| `deck/` | Deck settings (`fromdust.toml`) and the Proton launcher (`play.sh`) |
| `scripts/build.ps1` | Build: SDK, codegen and game, install |
| `game-data` | Junction to the extracted game (`the earlier D3D12 port\game`); not in git |

The game loads six images besides `Default.xex` with `XexLoadImage`: `GalileoKernel`,
`GalileoPathfind_9_9`, `GalileoSimulatorWater`, `GalileoSimulatorWater_9_9`, `GalileoTexture`
and `AInative/bin/dll/dustai.x360.f`. Each is recompiled as a ReXGlue module DLL.

## Build

You need Visual Studio 2022 Build Tools (clang-cl, Windows SDK), CMake, Ninja and Python 3.

```powershell
scripts\build.ps1                       # SDK, game and install
scripts\build.ps1 -Stage game,install   # after a change to the game or its hints
scripts\build.ps1 -Stage install -CopyGame
```

`tools/gen_manifest.py` runs codegen until it reports no unresolved branch targets. Each target
it reports becomes a hint, sized from the function bounds of the XenonRecomp port's config.

## Native Linux build (Vulkan, no Proton)

`scripts/build.sh` builds the same project natively for Linux with clang and the Vulkan backend.
Run it on Arch Linux: WSL (`wsl --install archlinux`, from an administrator
PowerShell), or a distrobox on the Deck. Use `--stage deps` once, as root, to install the
packages. The result is `~/fromdust-linux/FromDust`, with `play-native.sh` and
`deck/fromdust-linux.toml`.
- Checked on Windows with `gpu_backend = "vulkan"` and `render_target_path_vulkan = "fsi"`
  (the Linux settings): the same image as D3D12. Without `fsi` (fragment shader interlock) the
  render-target path needs integer MSAA ownership transfers. RADV has had the extension since
  Mesa 23.2; WSL's Vulkan driver (Dozen) has neither, so the Linux build can't render in WSL.
- FidelityFX is built headers only (`REXGLUE_FIDELITYFX_RUNTIME=OFF`): FSR 1 and CAS work, the
  FSR 2/3 runtime would need AMD's Windows-only shader compiler (and has no game data to use).
- libstdc++ is linked statically. `deck/glibc_compat.h` binds the libm functions that Arch's
  glibc 2.44 re-versioned to their old versions, and the install stage fails if the binaries
  need a glibc newer than the Deck's 2.41 (now: 2.38).
- `fromdust_capture_at = "40,120"` saves the game's own image to `captures/`.

## Install on the Deck

Native build (preferred, no Proton): with SSH to the Deck (user-level sshd, port 2222), the PC
sends `build/fromdust-linux-bin.tar.gz` (binaries and the save, made from
`~/fromdust-linux/FromDust`) and `deck/install-native.sh` with `deck/steam_shortcuts.py` (from
PGR4Native). The script installs to `~/Games/FromDustNative`, links the game data of an
earlier install, and, with Steam closed, adds "From Dust" to the library running
`play-native.sh` with no compatibility tool. Send `deck/art/` beside the script too: it gives the
shortcut its library artwork (`steam_shortcuts.py --art-dir`). `portrait.png` (600x900),
`grid.png` (920x430, from the store header) and `icon.png` are the game's Steam store assets
(app 33460, which has no hero or logo); `hero.png` (1920x620) is a crop of this port's own
capture of the first level.

Windows build under Proton (the earlier route):

1. Copy `build\FromDust\` (about 1.3 GB, with the game in `game/` and the save in `userdata/`)
   to a USB stick formatted exFAT, which SteamOS mounts by itself.
2. On the Deck, switch to Desktop Mode (Steam button > Power > Switch to Desktop) and copy the
   folder to `/home/deck/Games/FromDust`.
3. In Steam, choose Games > "Add a Non-Steam Game to My Library" > Browse. Set the file type to
   "All files", pick `fromdust.exe`, then Add.
4. In the library, open the game's Properties: name it "From Dust", and under Compatibility
   force **Proton Experimental**. Steam then runs it in Proton with vkd3d-proton. `play.sh`
   does the same without Steam's Proton setting, if you prefer it.
5. Back in Game Mode, the controller works as an Xbox pad (Steam Input's Gamepad layout). In
   the Quick Access menu (...) > Performance: no frame limit or 60, with the refresh rate at
   60 (an OLED panel can also run at 90). Avoid 40 and 45: the world runs at 30 steps per
   second.
6. The first minutes may stutter while the shaders compile (vkd3d keeps them for later runs).
   If something fails, `PROTON_LOG=1 %command%` in the launch options writes
   `~/steam-<id>.log`. The game writes its own log in the game folder.

Settings live in `fromdust.toml` beside the executable. Steam launch options override them for
a session, for example `%command% --draw_resolution_scale_x=2 --draw_resolution_scale_y=2`
when docked.
