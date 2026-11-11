# A/B at the shipping settings (60 FPS, even pacing): total CPU of the game, in % of one core,
# over 100-160 s on the save route, for the build in build\FromDust and for the executables
# and DLLs of another folder (copied over it for the B runs, then put back).
#   tools\ab60.ps1 -Other build\keep-locals [-Runs 2]
param([Parameter(Mandatory)][string]$Other, [int]$Runs = 2)
$root = Resolve-Path "$PSScriptRoot\.."
$dir = "$root\build\FromDust"
$route = "30:hold:ENTER:150,34:hold:ENTER:150,38:hold:ENTER:150,42:hold:ENTER:150,46:hold:ENTER:150,52:hold:SPACE:150,58:hold:SPACE:150,64:hold:SPACE:150,80:hold:SPACE:150,90:hold:SPACE:150"
function Measure-Run {
    $job = Start-Job -ScriptBlock { param($s, $k) powershell -NoProfile -ExecutionPolicy Bypass -File $s -Keys $k -Seconds 165 } -ArgumentList "$PSScriptRoot\drive.ps1", $route
    Start-Sleep 100
    $p = Get-Process fromdust -ErrorAction SilentlyContinue | Select-Object -First 1
    $c0 = $p.TotalProcessorTime.TotalMilliseconds
    Start-Sleep 60
    $p.Refresh(); $c1 = $p.TotalProcessorTime.TotalMilliseconds
    Wait-Job $job | Out-Null; Remove-Job $job
    # The game must be gone before the next run or the swap of builds (its files stay locked).
    while (Get-Process fromdust -ErrorAction SilentlyContinue) {
        Get-Process fromdust -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep -Milliseconds 500
    }
    $fps = (Select-String -Path "$dir\drive.log" -Pattern 'Guest frames: ([\d.]+)' | Select-Object -Last 8 |
        ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Measure-Object -Average).Average
    return [pscustomobject]@{ Core = ($c1 - $c0) / 600; Fps = $fps }
}
$backup = "$root\build\ab-current"
New-Item -ItemType Directory -Force $backup | Out-Null
Copy-Item "$dir\*.exe", "$dir\*.dll" $backup -Force
foreach ($side in 'A', 'B') {
    if ($side -eq 'B') { Copy-Item "$root\$Other\*.exe", "$root\$Other\*.dll" $dir -Force }
    for ($r = 1; $r -le $Runs; $r++) {
        $m = Measure-Run
        "{0} run {1}: {2:F1}% of a core at {3:F1} FPS" -f $side, $r, $m.Core, $m.Fps
    }
}
Copy-Item "$backup\*" $dir -Force
"(A = build\FromDust, B = $Other)"
