# Starts the game N times for S seconds each and reports how each run ended, to catch
# start-up crashes that only happen sometimes. crash.txt of a failing run is kept as
# crash-<n>.txt and its log as start-<n>.log.
#   tools\start_loop.ps1 -Runs 8 -Seconds 25 [-Extra "--mnk_mode=true"]
param([int]$Runs = 8, [int]$Seconds = 25, [string]$Extra = '')
$dir = Resolve-Path "$PSScriptRoot\..\build\FromDust"
for ($i = 1; $i -le $Runs; $i++) {
    Remove-Item "$dir\crash.txt", "$dir\start.log" -ErrorAction SilentlyContinue
    $args = @('--log_file=start.log') + ($Extra -split ' ' | Where-Object { $_ })
    $p = Start-Process "$dir\fromdust.exe" -ArgumentList $args -WorkingDirectory $dir -PassThru
    if ($p.WaitForExit($Seconds * 1000)) {
        "run ${i}: exited 0x{0:X8}" -f $p.ExitCode
        Copy-Item "$dir\start.log" "$dir\start-$i.log" -ErrorAction SilentlyContinue
        Copy-Item "$dir\crash.txt" "$dir\crash-$i.txt" -ErrorAction SilentlyContinue
    } else {
        Stop-Process $p -Force; Start-Sleep 1; "run ${i}: running after ${Seconds}s"
    }
}
