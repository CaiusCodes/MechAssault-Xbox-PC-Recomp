# mechassault-recomp: render scale / anti-aliasing benchmark. For each case, a windowed run of the Mission 1 walk
# script with that setting; prints the last two 10 s frame timing lines and the renderer's per-second host cost.
param([int]$Width = 1280, [int]$Height = 960, [string[]]$Cases = @("1,1", "2,1", "3,1", "4,1", "2,0"))
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$settings = "out\build\release\settings.toml"
$saved = Get-Content $settings
$env:XBE_PAD_SCRIPT = "$root\scripts\pad_mission1_play.txt"
$env:XBE_LOG_LEVEL = "warn"
foreach ($c in $Cases) {
    $scale, $aa = $c.Split(",")
    @("display_mode = `"windowed`"", "width = $Width", "height = $Height", "perf_overlay = 1", "aspect_ratio = 0",
      "render_scale = $scale", "anti_aliasing = $aa") | Set-Content -Encoding ascii $settings
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run.ps1 -Seconds 92 -Every 91 -Tag "bench_s${scale}_aa$aa" | Out-Null
    "== render_scale $scale anti_aliasing $aa ($Width x $Height window)"
    $log = Get-Content out\build\release\logs\mechassault.log
    $log | Select-String "frames:" | Select-Object -Last 2 | ForEach-Object { $_.Line }
    $log | Select-String "perf: [0-9.]+ fps" | Select-Object -Last 1 | ForEach-Object { $_.Line }
}
$saved | Set-Content -Encoding ascii $settings
