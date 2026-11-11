# Poor man's sampling profiler: loads the save like hot_threads.ps1, then at -At seconds
# suspends the busiest thread (or -Rank N for the Nth busiest) -Samples times, reads its
# instruction pointer and counts module+offset. Writes build\FromDust\samples.txt (count, module,
# offset) for tools\symbolize_samples.py, which names them with the lld-link /MAP files.
#   tools\sample_thread.ps1 [-At 110] [-Samples 3000] [-Rank 1] [-Extra "<flags>"]
param([int]$At = 110, [int]$Samples = 3000, [int]$Rank = 1, [string]$Extra = '')
$dir = Resolve-Path "$PSScriptRoot\..\build\FromDust"
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Sampler {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(int access, bool inherit, int id);
    [DllImport("kernel32.dll")] public static extern int SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    // CONTEXT is 1232 bytes on x64, 16-byte aligned; ContextFlags at 0x30, Rip at 0xF8.
    public static ulong Rip(IntPtr h, IntPtr buf) {
        IntPtr ctx = new IntPtr((buf.ToInt64() + 15) & ~15L);
        Marshal.WriteInt32(ctx, 0x30, 0x100001);  // CONTEXT_CONTROL
        if (!GetThreadContext(h, ctx)) return 0;
        return (ulong)Marshal.ReadInt64(ctx, 0xF8);
    }
}
'@
$route = '30:START:200,34:START:200,38:START:200,42:START:200,46:START:200,52:A:200,58:A:200,64:A:200,80:A:200,90:A:200'
$args = @('--log_file=sample.log', "--fromdust_autopress=$route", '--video_mode_refresh_rate=240',
          '--fromdust_even_pacing=false', '--fromdust_fps_limit=240') + ($Extra -split ' ' | Where-Object { $_ })
$p = Start-Process "$dir\fromdust.exe" -ArgumentList $args -WorkingDirectory $dir -PassThru
Start-Sleep $At
$p.Refresh()
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep 2
$p.Refresh()
$busy = $p.Threads | ForEach-Object { try { [pscustomobject]@{ Id = $_.Id; D = $_.TotalProcessorTime.TotalMilliseconds - $a[$_.Id] } } catch {} } |
    Sort-Object D -Descending | Select-Object -Skip ($Rank - 1) -First 1
$mods = $p.Modules | ForEach-Object { [pscustomobject]@{ Name = $_.ModuleName; Base = [uint64]$_.BaseAddress.ToInt64(); Size = [uint64]$_.ModuleMemorySize } }
$h = [Sampler]::OpenThread(0x0002 -bor 0x0008 -bor 0x0010, $false, $busy.Id)  # suspend/resume, get/set context
$buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(1232 + 16)
$counts = @{}
for ($i = 0; $i -lt $Samples; $i++) {
    [Sampler]::SuspendThread($h) | Out-Null
    $rip = [Sampler]::Rip($h, $buf)
    [Sampler]::ResumeThread($h) | Out-Null
    $m = $mods | Where-Object { $rip -ge $_.Base -and $rip -lt $_.Base + $_.Size } | Select-Object -First 1
    $key = if ($m) { '{0} {1:x}' -f $m.Name, ($rip - $m.Base) } else { 'unknown {0:x}' -f $rip }
    $counts[$key] = 1 + [int]$counts[$key]
    Start-Sleep -Milliseconds 1
}
[Sampler]::CloseHandle($h) | Out-Null
Stop-Process $p -Force
$counts.GetEnumerator() | Sort-Object Value -Descending |
    ForEach-Object { '{0} {1}' -f $_.Value, $_.Key } | Set-Content "$dir\samples.txt"
"thread t$($busy.Id), $Samples samples -> $dir\samples.txt"
