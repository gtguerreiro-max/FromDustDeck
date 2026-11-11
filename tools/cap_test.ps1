# Loads the save hands-free (fromdust_autopress), saves the game's image at given seconds and
# makes a contact sheet: for checking rendering with given flags, without watching the screen.
#   tools\cap_test.ps1 -Tag name [-At "105,108"] [-Extra "<flags>"]
param([Parameter(Mandatory)][string]$Tag, [string]$At = '105,108', [string]$Extra = '', [string]$Dir = '')
if ($Dir) { $dir = Resolve-Path $Dir } else { $dir = Resolve-Path "$PSScriptRoot\..\build\FromDust" }
$route = '30:START:200,34:START:200,38:START:200,42:START:200,46:START:200,52:A:200,58:A:200,64:A:200,80:A:200,90:A:200'
Remove-Item "$dir\captures\guest-*.bmp" -ErrorAction SilentlyContinue
$last = ($At -split ',' | ForEach-Object { [int]$_ } | Measure-Object -Maximum).Maximum
$args = @("--log_file=cap-$Tag.log", "--fromdust_autopress=$route", "--fromdust_capture_at=$At") + ($Extra -split ' ' | Where-Object { $_ })
$p = Start-Process "$dir\fromdust.exe" -ArgumentList $args -WorkingDirectory $dir -PassThru
Start-Sleep ($last + 3)
Stop-Process $p -Force -ErrorAction SilentlyContinue
while (Get-Process fromdust -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }
python -I -c @"
from PIL import Image
import glob, sys
files = sorted(glob.glob(r'$dir\captures\guest-*.bmp'), key=lambda f: int(f.split('-')[-1][:-5]))
ims = [Image.open(f).convert('RGB') for f in files]
w = 320; th = [i.resize((w, int(i.height * w / i.width))) for i in ims]
s = Image.new('RGB', (w * len(th), th[0].height))
for k, t in enumerate(th): s.paste(t, (k * w, 0))
s.save(r'$dir\captures\sheet-$Tag.png'); print('sheet-$Tag.png', len(ims))
"@

