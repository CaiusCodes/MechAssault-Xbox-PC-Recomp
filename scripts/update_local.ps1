# mechassault-recomp: the local install (out\local\MechAssault), a portable copy laid out as a release is, brought up
# to the current build without touching what the player has there: the imported game, saves, settings and logs stay.
# The first start after a code change builds the game code again from the imported game, as a release does.
# scripts\update_local.ps1            build, package (release.bat) and refresh the local install
# scripts\update_local.ps1 -NoBuild   refresh it from the package already made
param([switch]$NoBuild, [string]$Target = "out\local\MechAssault")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $NoBuild) {
    $env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;" + $env:PATH
    cmd /c "scripts\b.bat cmake --build out\build\release -j 6" | Out-Null
    if ($LASTEXITCODE) { throw "build failed" }
    cmd /c ".\release.bat" | Out-Null
}
$pkg = "out\build\release\package\MechAssault"
if (-not (Test-Path "$pkg\MechAssault.exe")) { throw "no package at $pkg" }
New-Item -ItemType Directory -Force $Target | Out-Null
# the program, readme, licences and runtime (the compiler); not resources\game, saves, settings or logs
robocopy $pkg $Target /e /xd "$pkg\resources\game" "$pkg\resources\saves" "$pkg\resources\settings" "$pkg\resources\logs" /njh /njs /nfl /ndl | Out-Null
foreach ($d in "game", "saves", "settings", "logs") { New-Item -ItemType Directory -Force "$Target\resources\$d" | Out-Null }
$v = (Get-Item "$Target\MechAssault.exe").VersionInfo.FileVersion
"local install updated: $((Resolve-Path $Target).Path) (version $v, $(git rev-parse --short HEAD))"
