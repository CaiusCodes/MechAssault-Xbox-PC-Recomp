@echo off
rem mechassault-recomp: the build toolchain in this shell only (the same lookup as build.bat), for scripted builds
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
