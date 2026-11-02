@echo off
rem mechassault-recomp: scripts\b.bat <command...> runs a command with the toolchain, from the repository root
call "%~dp0env.bat"
cd /d "%~dp0.."
%*
