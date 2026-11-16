# Builds the From Dust ReXGlue port for the Steam Deck on Windows: ReXGlue v0.11 with ReXNative's
# patches, built with LLVM 20 (clang++, lld) in the Visual Studio 2022 environment. The result is
# a Windows x64 build that the Deck runs under Proton (D3D12 through vkd3d-proton).
#
#   scripts\build.ps1 [-Stage sdk,game,install] [-Dest build] [-Jobs 10] [-Tune znver2]
#                     [-SdkInstall <an installed ReXGlue SDK>] [-SdkSrc <its source tree>]
#                     [-FidelityFx <the FidelityFX SDK v2.4.0>] [-Llvm <LLVM 20 bin>] [-CopyGame]
#
# Stages:
#   sdk      ReXNative's scripts\build-sdk.ps1 (third_party\ReXNative): ReXGlue v0.11.0 at its
#            sdk\rexglue.lock with its sdk\patches\v0.11 (the host renderer's SDK side, From Dust's
#            SDK changes, codegen, kernel and audio fixes), into $Dest\rexglue-sdk and
#            $Dest\sdk-install, and the FidelityFX SDK into $Dest\fidelityfx-sdk. Skip it and pass
#            -SdkInstall / -SdkSrc to reuse one built already (the same lock and patches).
#   game     Runs tools\gen_manifest.py (codegen, adding function hints it asks for) and builds
#            the game and its six module DLLs into $Dest\game.
#   install  Gathers the executable, module DLLs, the DLLs the build staged and the settings file
#            into $Dest\FromDust with the game data in FromDust\game (-CopyGame), the
#            self-contained layout the runtime looks for: copy this folder to the Deck.
#
# Target CPU: -march=x86-64-v3 (FMA, AVX2, MOVBE) with -mtune=znver2, the Deck's Zen 2.
# 128-bit vectors are preferred: the Deck's kernel sets a chicken bit without the Zenbleed
# microcode fix that makes VZEROUPPER slow (PGR4Native docs/development.md).
param(
    [string]$Stage = 'sdk,game,install',
    [string]$Dest = "$PSScriptRoot\..\build",
    [string]$SdkSrc = '',
    [string]$SdkInstall = '',
    [string]$FidelityFx = '',
    [string]$Llvm = $env:REXNATIVE_LLVM,
    [string]$Tune = 'znver2',
    [int]$Jobs = 10,
    [switch]$CopyGame
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$Dest = [IO.Path]::GetFullPath($Dest)
if (-not $SdkSrc) { $SdkSrc = "$Dest\rexglue-sdk" }
if (-not $SdkInstall) { $SdkInstall = "$Dest\sdk-install" }
if (-not $FidelityFx) { $FidelityFx = "$Dest\fidelityfx-sdk" }
if (-not $Llvm) { $Llvm = 'C:\Program Files\LLVM\bin' }
# ReXNative: the third_party\ReXNative submodule, or a checkout beside this one.
$rexnative = @("$repo\third_party\ReXNative", "$repo\..\ReXNative") |
    Where-Object { Test-Path "$_\scripts\vcenv.cmd" } | Select-Object -First 1
if (-not $rexnative) { throw 'no ReXNative: clone with --recurse-submodules (third_party\ReXNative)' }
$vc = "$rexnative\scripts\vcenv.cmd"
$flags = "-march=x86-64-v3 -mtune=$Tune -mprefer-vector-width=128 -ffp-model=precise -fno-math-errno -fno-char8_t -D_CRT_SECURE_NO_WARNINGS"
$stages = @($Stage -split ',' | ForEach-Object { $_ -split ' ' } | Where-Object { $_ })
# vcvars64 calls vswhere, which it expects on PATH.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
New-Item -ItemType Directory -Force "$Dest\logs" | Out-Null
function Run([string]$cmd, [string]$log) {
    # The redirection is cmd's, so Windows PowerShell 5.1 never sees native stderr (with
    # ErrorActionPreference Stop it would turn vcvars' harmless warnings into a failure).
    cmd /c "call `"$vc`" && set `"PATH=$Llvm;%PATH%`" && $cmd > `"$Dest\logs\$log`" 2>&1"
    if ($LASTEXITCODE) { throw "$log failed (see $Dest\logs\$log)" }
}

if ($stages -contains 'sdk') {
    & "$rexnative\scripts\build-sdk.ps1" -Dest $Dest -SdkSrc $SdkSrc -FidelityFx $FidelityFx -Llvm $Llvm -Jobs $Jobs
    if (-not (Test-Path "$SdkInstall\bin\rexglue.exe")) { throw "no rexglue.exe in $SdkInstall" }
}

if ($stages -contains 'game') {
    $rex = "$SdkInstall\bin\rexglue.exe"
    python "$repo\tools\gen_manifest.py" --rexglue $rex
    if ($LASTEXITCODE) { throw 'codegen failed (see codegen.log)' }
    # A build folder of the v0.10 SDK (clang-cl) cannot be reused.
    if ((Test-Path "$Dest\game\CMakeCache.txt") -and
        (Select-String -Path "$Dest\game\CMakeCache.txt" -Pattern 'clang-cl' -Quiet)) {
        Remove-Item -Recurse -Force "$Dest\game"
    }
    Run "set `"CFLAGS=$flags`" && set `"CXXFLAGS=$flags`" && cmake -S `"$repo`" -B `"$Dest\game`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `"-DCMAKE_PREFIX_PATH=$SdkInstall`" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF `"-DFROMDUST_REXGLUE_SDK_SRC=$SdkSrc`" `"-DFROMDUST_FIDELITYFX_SDK=$FidelityFx`"" 'game-configure.log'
    Run "cmake --build `"$Dest\game`" -j $Jobs" 'game-build.log'
}

if ($stages -contains 'install') {
    $out = "$Dest\FromDust"
    New-Item -ItemType Directory -Force $out | Out-Null
    Get-ChildItem "$Dest\game" -Include fromdust*.exe, fromdust*.dll -Recurse -Depth 2 |
        Where-Object FullName -notmatch 'CMakeFiles' | Copy-Item -Destination $out -Force
    # Every DLL the build staged beside the executable: ReXGlue's modules and GPU drivers, SDL3,
    # NVIDIA DLSS / DLSS 5 and AMD FidelityFX (rexnative_add); the v0.10 SDK's DLLs go.
    $staged = @(Get-ChildItem "$Dest\game\*.dll" | ForEach-Object { $_.Name.ToLower() })
    Get-ChildItem "$out\*.dll" | Where-Object {
        ($_.Name -like 'rex*' -or $_.Name -eq 'amd_fidelityfx_dx12.dll') -and
        ($staged -notcontains $_.Name.ToLower()) } | Remove-Item -Force
    Copy-Item "$Dest\game\*.dll" $out -Force
    # Kept once written: the in-game settings menu saves its choices there.
    if (-not (Test-Path "$out\fromdust.toml")) { Copy-Item "$repo\deck\fromdust.toml" $out }
    Copy-Item "$repo\deck\play.sh" $out -Force
    if ($CopyGame) { robocopy "$repo\game-data" "$out\game" /E /NFL /NDL /NJH /NJS | Out-Null }
    Get-ChildItem $out | Select-Object Name, Length
}
