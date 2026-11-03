# mechassault-recomp: plays every campaign level in turn (scripts/levels/pad_<LEVEL>.txt) in the background test mode
# (XBE_BACKGROUND: a small window in the screen's corner that never takes the focus), one at a time; screenshots in
# runs\levels\<LEVEL>\, each level's log in runs\levels\<LEVEL>\mechassault.log. Needs unlock_campaign = true.
param([string]$Levels = "", [int]$Every = 6)  # -Levels "LEVEL01,LEVEL02" (one string: -File passes no arrays)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$env:XBE_BACKGROUND = "1"; $env:XBE_LOG_LEVEL = "warn"
$want = @($Levels.Split(",") | Where-Object { $_ })
$all = Get-ChildItem scripts\levels\pad_LEVEL*.txt | Sort-Object Name
foreach ($f in $all) {
    $name = $f.BaseName.Substring(4)
    if ($want.Count -and -not ($want -contains $name)) { continue }
    $env:XBE_PAD_SCRIPT = $f.FullName
    $last = (Get-Content $f.FullName | Where-Object { $_ -match '^\d' } | Select-Object -Last 1).Split(' ')[0]
    $secs = [int][math]::Ceiling([double]$last) + 8
    & powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Seconds $secs -Every $Every -Tag "levels\$name" | Out-Null
    Copy-Item out\build\release\logs\mechassault.log "runs\levels\$name\mechassault.log" -Force
    "{0} done ({1} s)" -f $name, $secs
}
