# mechassault-recomp: run the dev build windowed for N seconds, screenshot the window every few seconds, then close
# only the process this started. scripts\run.ps1 -Seconds 60 -Every 5 -Tag name   (shots in runs\<tag>\)
param([int]$Seconds = 40, [int]$Every = 5, [string]$Tag = "run", [string]$Exe = "out\build\release\MechAssault.exe")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 public struct RECT { public int L, T, R, B; } }
"@
$dir = Join-Path $root "runs\$Tag"; New-Item -ItemType Directory -Force $dir | Out-Null
$env:XBE_PERF = "1"
if (-not $env:XBE_CAPTURE_DIR) { $env:XBE_CAPTURE_DIR = Join-Path $root "captures" }
$pending = @(); foreach ($t in $F3At) { $pending += ,@($t, 0x72) }
if ($Keys) { foreach ($k in $Keys.Split(",")) { $a = $k.Split(":"); $pending += ,@([int]$a[0], [int][System.Windows.Forms.Keys]::($a[1])) } }
$p = Start-Process -FilePath (Join-Path $root $Exe) -WorkingDirectory (Split-Path (Join-Path $root $Exe)) -PassThru
$t0 = Get-Date; $i = 0; $lastCpu = 0; $lastT = 0; $cpuLog = Join-Path $dir "cpu.txt"; "" | Set-Content $cpuLog
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    $left = $Seconds - ((Get-Date) - $t0).TotalSeconds
    Start-Sleep -Milliseconds ([int](1000 * [math]::Max(0.2, [math]::Min($Every, $left)))); $p.Refresh(); $h = $p.MainWindowHandle
    $now = ((Get-Date) - $t0).TotalSeconds
    try { $cpu = $p.TotalProcessorTime.TotalSeconds; if ($now -gt $lastT) { "{0,7:N1} s  {1,5:N2} cores  {2,5:N0} MB" -f $now, (($cpu - $lastCpu) / ($now - $lastT)), ($p.WorkingSet64 / 1MB) | Add-Content $cpuLog }; $lastCpu = $cpu; $lastT = $now } catch {}
    $gw = [W]::FindWindow($null, "MechAssault"); if ($gw -ne [IntPtr]::Zero) { $h = $gw }
    if ($h) { foreach ($e in $pending) { if ($e[0] -ge 0 -and $now -ge $e[0]) {
        [void][W]::PostMessage($h, 0x100, [IntPtr]$e[1], [IntPtr]0); Start-Sleep -Milliseconds 120
        [void][W]::PostMessage($h, 0x101, [IntPtr]$e[1], [IntPtr]0xC0000001); $e[0] = -1 } } }
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
