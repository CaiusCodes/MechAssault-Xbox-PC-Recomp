# mechassault-recomp: run the dev build windowed for N seconds, screenshot the window every few seconds, then close
# only the process this started. scripts\run.ps1 -Seconds 60 -Every 5 -Tag name   (shots in runs\<tag>\)
param([int]$Seconds = 40, [int]$Every = 5, [string]$Tag = "run", [string]$Exe = "out\build\release\MechAssault.exe")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 public struct RECT { public int L, T, R, B; } }
"@
$dir = Join-Path $root "runs\$Tag"; New-Item -ItemType Directory -Force $dir | Out-Null
$env:XBE_PERF = "1"
$p = Start-Process -FilePath (Join-Path $root $Exe) -WorkingDirectory (Split-Path (Join-Path $root $Exe)) -PassThru
$t0 = Get-Date; $i = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Seconds $Every; $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -and $h -ne [IntPtr]::Zero) {
        $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r)
        $w = $r.R - $r.L; $hh = $r.B - $r.T
        if ($w -gt 0 -and $hh -gt 0) {
            $bmp = New-Object System.Drawing.Bitmap $w, $hh; $g = [System.Drawing.Graphics]::FromImage($bmp)
            $dc = $g.GetHdc(); [void][W]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
            $bmp.Save((Join-Path $dir ("{0:D3}.png" -f $i)), [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose(); $i++
        }
    }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "stopped after $Seconds s" } else { "exited by itself, code $($p.ExitCode)" }
