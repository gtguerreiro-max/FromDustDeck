# Benchmark: loads the D3D12 port's save of the first level (copied to userdata/
# B13EBABEBABEBABE/58410A91/00000001/DUST) through Play > Resume the story > the map, with the
# frame rate cap raised (guest vblank 240 Hz, no even pacing) and reports, over a fixed window
# in play (100-160 s), the guest frame rate and the CPU time of all the game's threads per
# frame. On a Steam Deck the CPU and GPU share one power budget, so CPU ms per frame is the
# number that matters there; it is also much less noisy than the frame rate on this PC.
#   tools\bench.ps1 [-Tag name] [-Runs 2] [-Extra "<flags>"]
param([string]$Tag = 'bench', [int]$Runs = 2, [string]$Extra = '')
$dir = Resolve-Path "$PSScriptRoot\..\build\FromDust"
# Real key holds: SendKeys taps are too short for the menus to see them.
$route = "30:hold:ENTER:150,34:hold:ENTER:150,38:hold:ENTER:150,42:hold:ENTER:150,46:hold:ENTER:150,52:hold:SPACE:150,58:hold:SPACE:150,64:hold:SPACE:150,80:hold:SPACE:150,90:hold:SPACE:150"
$flags = "--video_mode_refresh_rate=240 --fromdust_even_pacing=false --fromdust_fps_limit=240 $Extra"
$results = @()
for ($r = 1; $r -le $Runs; $r++) {
    Remove-Item "$dir\drive.log" -ErrorAction SilentlyContinue
    $job = Start-Job -ScriptBlock {
        param($script, $keys, $extra)
        powershell -NoProfile -ExecutionPolicy Bypass -File $script -Keys $keys -Seconds 170 -Extra $extra
    } -ArgumentList "$PSScriptRoot\drive.ps1", $route, $flags
    $start = Get-Date
    function CpuAt([double]$at) {
        while (((Get-Date) - $start).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 200 }
        $p = Get-Process fromdust -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($p) { return @{ Cpu = $p.TotalProcessorTime.TotalMilliseconds; Time = Get-Date } }
        return $null
    }
    $a = CpuAt 100; $b = CpuAt 160
    Wait-Job $job | Out-Null; Receive-Job $job | Out-Null; Remove-Job $job
    if (-not $a -or -not $b) { "$Tag run ${r}: the game was not running in the window"; continue }
    # Guest frames inside the window, from the 5 s frame-rate samples.
    $frames = 0.0
    foreach ($l in Select-String -Path "$dir\drive.log" -Pattern 'Guest frames: ([\d.]+)/s') {
        $t = [datetime]::ParseExact($l.Line.Substring(1, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if ($t -gt $a.Time.AddSeconds(5) -and $t -le $b.Time) { $frames += 5 * [double]$l.Matches[0].Groups[1].Value }
    }
    $seconds = ($b.Time - $a.Time).TotalSeconds - 5
    $cpu = ($b.Cpu - $a.Cpu) * $seconds / ($seconds + 5)
    if ($frames -gt 0) {
        $res = [pscustomobject]@{ Fps = $frames / $seconds; CpuMs = $cpu / $frames }
        $results += $res
        "{0} run {1}: {2:F1} FPS, {3:F2} ms CPU per frame (all threads)" -f $Tag, $r, $res.Fps, $res.CpuMs
    } else { "$Tag run ${r}: no frame samples" }
    Copy-Item "$dir\drive.log" "$dir\bench-$Tag-$r.log" -Force
}
if ($results.Count) {
    "{0}: {1:F1} FPS, {2:F2} ms CPU per frame over {3} runs" -f $Tag,
        ($results.Fps | Measure-Object -Average).Average, ($results.CpuMs | Measure-Object -Average).Average, $results.Count
}
