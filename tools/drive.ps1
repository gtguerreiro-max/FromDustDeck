# Starts build\FromDust\fromdust.exe, sends key presses to its window at given seconds and saves
# window captures, for checking a build on this PC without a person at the keyboard.
#   tools\drive.ps1 -Keys "40:{ENTER},50:{ENTER},60:{ENTER},70: ,80: " -Shots "45,120" -Seconds 130
# Keys use SendKeys syntax ("{ENTER}" Start, " " A with mnk_mode), or hold:<K+K>:<ms> to hold
# keys down (Q = LT, E = RT, WASD = left stick); a hold delays the events after it. Shots:
# shot-<s>.png.
param([string]$Keys = '', [string]$Shots = '', [int]$Seconds = 120, [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$dir = Resolve-Path "$PSScriptRoot\..\build\FromDust"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FdWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    public static void Key(byte vk, bool down) {
        keybd_event(vk, (byte)MapVirtualKey(vk, 0), down ? 0u : 2u, UIntPtr.Zero);
    }
}
'@
$events = @()
foreach ($k in ($Keys -split ',' | Where-Object { $_ })) {
    $i = $k.IndexOf(':'); $events += [pscustomobject]@{ At = [int]$k.Substring(0, $i); Key = $k.Substring($i + 1); Shot = $false }
}
foreach ($s in ($Shots -split ',' | Where-Object { $_ })) { $events += [pscustomobject]@{ At = [int]$s; Key = $null; Shot = $true } }
$events = $events | Sort-Object At
$args = @('--log_file=drive.log', '--mnk_mode=true', '--guest_frame_stats=true') + ($Extra -split ' ' | Where-Object { $_ })
$p = Start-Process "$dir\fromdust.exe" -ArgumentList $args -WorkingDirectory $dir -PassThru
$start = Get-Date
foreach ($e in $events) {
    while (((Get-Date) - $start).TotalSeconds -lt $e.At) { Start-Sleep -Milliseconds 200; if ($p.HasExited) { break } }
    if ($p.HasExited) { "exited early: 0x{0:X8}" -f $p.ExitCode; break }
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($e.Shot) {
        $r = New-Object FdWin+RECT; [FdWin]::GetWindowRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [FdWin]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
        # PrintWindow cannot read a Vulkan swapchain (the image comes back black): then copy the
        # window's area from the screen, with the window brought to the front.
        $black = $true
        foreach ($pt in @(@(0.3, 0.3), @(0.5, 0.5), @(0.7, 0.6), @(0.5, 0.8), @(0.2, 0.7))) {
            $c = $bmp.GetPixel([int]($bmp.Width * $pt[0]), [int]($bmp.Height * $pt[1]))
            if ($c.R + $c.G + $c.B -gt 0) { $black = $false; break }
        }
        if ($black) {
            [FdWin]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
            $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
        }
        $bmp.Save("$dir\shot-$($e.At).png", [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
        "shot-$($e.At).png"
    } else {
        [FdWin]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 100
        if ($e.Key -like 'hold:*') {
            # hold:<keys joined by +>:<ms>, real key-down / key-up (SendKeys cannot hold).
            # Letters and digits by name, SPACE, ENTER, UP, DOWN, LEFT, RIGHT.
            $parts = $e.Key.Split(':'); $names = $parts[1].Split('+'); $ms = [int]$parts[2]
            $map = @{ SPACE = 0x20; ENTER = 0x0D; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; TAB = 0x09 }
            $vks = $names | ForEach-Object { if ($map.ContainsKey($_)) { [byte]$map[$_] } else { [byte][char]$_.ToUpper() } }
            foreach ($vk in $vks) { [FdWin]::Key($vk, $true) }
            Start-Sleep -Milliseconds $ms
            foreach ($vk in $vks) { [FdWin]::Key($vk, $false) }
            "held $($parts[1]) $ms ms at $($e.At)s"
        } else {
            [System.Windows.Forms.SendKeys]::SendWait($e.Key); "key '$($e.Key)' at $($e.At)s"
        }
    }
}
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 500 }
if ($p.HasExited) { "exit code 0x{0:X8}" -f $p.ExitCode } else { Stop-Process $p -Force; 'stopped after run' }
