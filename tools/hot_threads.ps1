# Loads the save like cap_test.ps1 and, at -At seconds, samples the CPU time of every thread of
# fromdust.exe over -Window seconds: prints the busiest threads (percent of one core) with the
# name the log gives that thread id, to see which thread limits the frame rate.
#   tools\hot_threads.ps1 [-At 110] [-Window 5] [-Extra "<flags>"]
param([int]$At = 110, [int]$Window = 5, [string]$Extra = '')
$dir = Resolve-Path "$PSScriptRoot\..\build\FromDust"
$route = '30:START:200,34:START:200,38:START:200,42:START:200,46:START:200,52:A:200,58:A:200,64:A:200,80:A:200,90:A:200'
$args = @('--log_file=hot.log', "--fromdust_autopress=$route", '--video_mode_refresh_rate=240',
          '--fromdust_even_pacing=false', '--fromdust_fps_limit=240') + ($Extra -split ' ' | Where-Object { $_ })
$p = Start-Process "$dir\fromdust.exe" -ArgumentList $args -WorkingDirectory $dir -PassThru
Start-Sleep $At
$p.Refresh()
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep $Window
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
    try {
        $d = $t.TotalProcessorTime.TotalMilliseconds - $a[$t.Id]
        [pscustomobject]@{ Tid = $t.Id; Pct = [math]::Round(100 * $d / ($Window * 1000), 1) }
    } catch {}
}
Stop-Process $p -Force
Start-Sleep 1
$log = Get-Content "$dir\hot.log" -ErrorAction SilentlyContinue
$rows | Sort-Object Pct -Descending | Select-Object -First 8 | ForEach-Object {
    $tid = $_.Tid
    $name = ($log | Select-String "\[t$tid\]" | Select-Object -First 1).Line
    "{0,6:N1}%  t{1}  {2}" -f $_.Pct, $tid, ($(if ($name) { $name.Substring(0, [math]::Min(110, $name.Length)) } else { '' }))
}
