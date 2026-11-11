# CPU time per thread of the running fromdust.exe over a window, busiest first, with what the
# log says each thread does (the log tags lines with the host thread id, [t<id>]).
#   tools\threads.ps1 [-Seconds 20] [-Log build\FromDust\drive.log]
param([int]$Seconds = 20, [string]$Log = "$PSScriptRoot\..\build\FromDust\drive.log")
$p = Get-Process fromdust -ErrorAction Stop | Select-Object -First 1
$before = @{}; foreach ($t in $p.Threads) { try { $before[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Seconds $Seconds
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
    try { $ms = $t.TotalProcessorTime.TotalMilliseconds } catch { continue }
    $delta = $ms - ($before[$t.Id] | ForEach-Object { $_ })
    [pscustomobject]@{ Id = $t.Id; Percent = [math]::Round(100 * $delta / ($Seconds * 1000), 1) }
}
$text = if (Test-Path $Log) { Get-Content $Log -Raw } else { '' }
$rows | Sort-Object Percent -Descending | Select-Object -First 12 | ForEach-Object {
    $hint = ''
    $m = [regex]::Matches($text, "\[t$($_.Id)\] ([^\r\n]{0,90})")
    if ($m.Count) { $hint = $m[$m.Count - 1].Groups[1].Value }
    "{0,6}  {1,5}%  {2}" -f $_.Id, $_.Percent, $hint
}
