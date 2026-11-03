# mechassault-recomp: two dev-build instances side by side (System Link on one PC: --instance 1 and 2 bind 127.0.0.1
# and .2, broadcasts reach the other), each with its own pad script; screenshots of both every few seconds into
# runs\<tag>\a_NNN.png and b_NNN.png. Stops only the two processes it started.
# scripts\run2.ps1 -Seconds 120 -Every 3 -Tag sl1 -ScriptA scripts\pad_sl_host.txt -ScriptB scripts\pad_sl_join.txt
param([int]$Seconds = 60, [int]$Every = 3, [string]$Tag = "run2", [string]$ScriptA, [string]$ScriptB,
      [string]$Exe = "out\build\release\MechAssault.exe")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W2 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 public struct RECT { public int L, T, R, B; } }
"@
$dir = Join-Path $root "runs\$Tag"; New-Item -ItemType Directory -Force $dir | Out-Null
$env:XBE_PERF = "1"
$exePath = Join-Path $root $Exe
$env:XBE_PAD_SCRIPT = (Resolve-Path $ScriptA).Path
$pa = Start-Process -FilePath $exePath -ArgumentList "--instance 1" -WorkingDirectory (Split-Path $exePath) -PassThru
Start-Sleep -Milliseconds 500
$env:XBE_PAD_SCRIPT = (Resolve-Path $ScriptB).Path
$pb = Start-Process -FilePath $exePath -ArgumentList "--instance 2" -WorkingDirectory (Split-Path $exePath) -PassThru
function Shot($p, $file) {
    $p.Refresh(); $h = $p.MainWindowHandle; if (-not $h -or $h -eq [IntPtr]::Zero) { return }
    $r = New-Object W2+RECT; [W2]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T; if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh; $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc(); [W2]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
}
$t0 = Get-Date; $i = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not ($pa.HasExited -and $pb.HasExited)) {
    $left = $Seconds - ((Get-Date) - $t0).TotalSeconds
    Start-Sleep -Milliseconds ([int](1000 * [math]::Max(0.2, [math]::Min($Every, $left))))
    $n = "{0:D3}" -f $i
    if (-not $pa.HasExited) { Shot $pa (Join-Path $dir "a_$n.png") }
    if (-not $pb.HasExited) { Shot $pb (Join-Path $dir "b_$n.png") }
    $i++
}
foreach ($p in @($pa, $pb)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
"stopped after $Seconds s"
