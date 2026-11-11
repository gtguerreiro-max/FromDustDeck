# Runs the D3D12 port (F:\FromDustRecomp) with the same autopress route as cap_test.ps1 and
# saves a capture of its window at the given second, as a reference for comparing images.
#   tools\old_shot.ps1 [-At 108] [-Out build\FromDust\captures\old-108.png]
param([int]$At = 108, [string]$Out = "$PSScriptRoot\..\build\FromDust\captures\old-$At.png")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OldWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
$env:FD_AUTOPRESS = '30:0010:200,34:0010:200,38:0010:200,42:0010:200,46:0010:200,52:1000:200,58:1000:200,64:1000:200,80:1000:200,90:1000:200'
$p = Start-Process 'F:\FromDustRecomp\build\FromDust.exe' -ArgumentList 'F:\FromDustRecomp\game' -WorkingDirectory 'F:\FromDustRecomp' -PassThru
Start-Sleep $At
$p.Refresh()
$h = $p.MainWindowHandle
[OldWin]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object OldWin+RECT; [OldWin]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object OldWin+POINT; [OldWin]::ClientToScreen($h, [ref]$pt) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Stop-Process $p -Force
"$Out $($bmp.Width)x$($bmp.Height)"
