@echo off
rem Checks which MechAssault release your copy is: choose its default.xbe or disc image when asked.
rem It writes MechAssault-copy-check.txt on your Desktop and opens it. It only reads; nothing is changed or sent.
rem Drop a default.xbe, game folder or .iso on this file to check that one directly.
start "" /wait "%~dp0MechAssault.exe" --check-copy %1
