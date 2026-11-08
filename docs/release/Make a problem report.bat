@echo off
rem Saves a problem report zip on your Desktop: the logs, the newest crash dumps, settings.toml and info.txt.
rem For when the game crashed or froze. Nothing is sent by itself.
cd /d "%~dp0"
echo Saving a problem report, this takes a few seconds ...
start "" /wait "%~dp0MechAssault.exe" --save-report
if errorlevel 1 (
    echo The report could not be saved. Send the files in resources\logs instead.
    explorer "%~dp0resources\logs"
) else (
    echo Saved on your Desktop as MechAssault-report-^<date^>-^<time^>.zip. Send that file.
    explorer shell:Desktop
)
pause
