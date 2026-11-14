# Builds the From Dust ReXGlue port for the Steam Deck on Windows, with the Visual Studio 2022
# clang-cl, lld-link and Windows SDK. The result is a Windows x64 build that the Deck runs under
# Proton (D3D12 through vkd3d-proton), the setup F:\PGR4Native has measured on a Deck.
#
#   scripts\build.ps1 [-Stage sdk,game,install] [-Dest build] [-Jobs 10] [-Tune znver2]
#
# Stages:
#   sdk      Configures and builds the ReXGlue SDK source tree at -SdkSrc into $Dest\sdk and installs
#            it to $Dest\sdk-install. The tree is made on first use: upstream ReXGlue at the commit
#            patches\rexglue.lock names, then patches\deck (generic fixes and Steam Deck pacing
#            patches taken from F:\PGR4Native, none of its PGR4-only ones), then patches\fromdust
#            (this port's SDK changes). FidelityFX comes from $Dest\fidelityfx-src when present.
#   game     Runs tools\gen_manifest.py (codegen, adding function hints it asks for) and builds
#            the game and its six module DLLs into $Dest\game.
#   install  Gathers the executable, module DLLs, SDK DLLs and the settings file into
#            $Dest\FromDust with the game data in FromDust\game (-CopyGame), the self-contained
#            layout the runtime looks for: copy this folder to the Deck.
#
# Target CPU: -march=x86-64-v3 (FMA, AVX2, MOVBE) with -mtune=znver2, the Deck's Zen 2.
# 128-bit vectors are preferred: the Deck's kernel sets a chicken bit without the Zenbleed
# microcode fix that makes VZEROUPPER slow (PGR4Native docs/development.md).
param(
    [string]$Stage = 'sdk,game,install',
    [string]$Dest = "$PSScriptRoot\..\build",
    [string]$SdkSrc = '',
    [string]$Tune = 'znver2',
    [int]$Jobs = 10,
    [switch]$CopyGame
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$Dest = [IO.Path]::GetFullPath($Dest)
if (-not $SdkSrc) { $SdkSrc = "$Dest\rexglue-sdk" }
$vc = 'F:\PGR4Native\scripts\windows\vcenv.cmd'
$flags = "/clang:-march=x86-64-v3 /clang:-mtune=$Tune /clang:-mprefer-vector-width=128 /clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t- -Wno-unknown-argument -Wno-unused-command-line-argument"
$stages = @($Stage -split ',' | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
# vcvars64 calls vswhere, which it expects on PATH.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
New-Item -ItemType Directory -Force "$Dest\logs" | Out-Null
function Run([string]$cmd, [string]$log) {
    # The redirection is cmd's, so Windows PowerShell 5.1 never sees native stderr (with
    # ErrorActionPreference Stop it would turn vcvars' harmless warnings into a failure).
    cmd /c "call `"$vc`" $cmd > `"$Dest\logs\$log`" 2>&1"
    if ($LASTEXITCODE) { throw "$log failed (see $Dest\logs\$log)" }
}

if ($stages -contains 'sdk') {
    if (-not (Test-Path "$SdkSrc\.git")) {
        $lock = @{}
        Get-Content "$repo\patches\rexglue.lock" | Where-Object { $_ -match '^(\w+)=(.*)$' } |
            ForEach-Object { $lock[$Matches[1]] = $Matches[2] }
        # As F:\PGR4Native\scripts\windows\build.ps1 does: no CRLF conversion (the patches expect
        # the files as committed), and libmspack's git symlinks, which become text placeholders
        # without Windows developer mode, are replaced by copies of their targets.
        git clone -q --no-checkout --filter=blob:none $lock.REXGLUE_URL $SdkSrc
        git -C $SdkSrc config core.autocrlf false
        git -C $SdkSrc checkout -q $lock.REXGLUE_COMMIT
        git -C $SdkSrc submodule update -q --init --recursive --depth 1 --jobs 8
        git -C $SdkSrc submodule foreach -q --recursive 'git config core.autocrlf false'
        python -c @"
import subprocess, os, shutil
repo = r'$SdkSrc\thirdparty\libmspack'
for line in subprocess.run(['git','-C',repo,'ls-files','-s'],capture_output=True,text=True).stdout.splitlines():
    mode, _, _, path = line.split(None, 3)
    if mode == '120000':
        p = os.path.join(repo, path)
        shutil.copyfile(os.path.normpath(os.path.join(os.path.dirname(p), open(p).read().strip())), p)
"@
        $patches = @(Get-ChildItem "$repo\patches\deck\*.patch" | Sort-Object Name) +
                   @(Get-ChildItem "$repo\patches\fromdust\*.patch" | Sort-Object Name)
        foreach ($p in $patches) {
            git -C $SdkSrc apply --whitespace=nowarn $p.FullName
            if ($LASTEXITCODE) { throw "patch $($p.Name) does not apply" }
        }
    }
    $ffx = ''
    if (Test-Path "$Dest\fidelityfx-src") { $ffx = "`"-DFETCHCONTENT_SOURCE_DIR_FIDELITYFX=$Dest\fidelityfx-src`"" }
    Run "set `"CFLAGS=$flags`" && set `"CXXFLAGS=$flags`" && cmake -S `"$SdkSrc`" -B `"$Dest\sdk`" -G Ninja $ffx -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link `"-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP`" -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_USE_VULKAN=ON -DCMAKE_RC_COMPILER=rc `"-DCMAKE_INSTALL_PREFIX=$Dest\sdk-install`"" 'sdk-configure.log'
    Run "cmake --build `"$Dest\sdk`" --target install -j $Jobs" 'sdk-build.log'
}

if ($stages -contains 'game') {
    $rex = "$Dest\sdk-install\bin\rexglue.exe"
    python "$repo\tools\gen_manifest.py" --rexglue $rex
    if ($LASTEXITCODE) { throw 'codegen failed (see codegen.log)' }
    Run "set `"CFLAGS=$flags /D_CRT_SECURE_NO_WARNINGS`" && set `"CXXFLAGS=$flags /D_CRT_SECURE_NO_WARNINGS`" && cmake -S `"$repo`" -B `"$Dest\game`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link `"-DCMAKE_PREFIX_PATH=$Dest\sdk-install`" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF" 'game-configure.log'
    Run "cmake --build `"$Dest\game`" -j $Jobs" 'game-build.log'
}

if ($stages -contains 'install') {
    $out = "$Dest\FromDust"
    New-Item -ItemType Directory -Force $out | Out-Null
    Get-ChildItem "$Dest\game" -Include fromdust*.exe, fromdust*.dll -Recurse -Depth 2 |
        Where-Object FullName -notmatch 'CMakeFiles' | Copy-Item -Destination $out -Force
    # ReXNative's runtime DLLs (rexnative_add copies them beside the executable): NVIDIA DLSS /
    # DLSS 5 and AMD FidelityFX (FSR upscaler, frame generation).
    Get-ChildItem "$Dest\game\*" -Include nvngx_*.dll, amd_fidelityfx_*_dx12.dll -ErrorAction SilentlyContinue |
        Copy-Item -Destination $out -Force
    # The SDK's DLLs: the runtime, the Xenos GPU plugin and AMD FidelityFX (FSR, present_effect).
    Copy-Item "$Dest\sdk-install\bin\*.dll" $out -Force
    # Kept once written: the in-game settings menu saves its choices there.
    if (-not (Test-Path "$out\fromdust.toml")) { Copy-Item "$repo\deck\fromdust.toml" $out }
    Copy-Item "$repo\deck\play.sh" $out -Force
    if ($CopyGame) { robocopy "$repo\game-data" "$out\game" /E /NFL /NDL /NJH /NJS | Out-Null }
    Get-ChildItem $out | Select-Object Name, Length
}
