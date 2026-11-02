@echo off
set "GAME_EXE=MechAssault"
cd /d "%~dp0"
set "OUT=out\build\release"
rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || exit /b 1
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || exit /b 1
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, update the SDK. && exit /b 1
set /p SDK=<out\build\release\sdk_dir.txt
del /q "%GAME_EXE%.zip" 2>nul
rem mechassault-recomp: the portable layout
rem   MechAssault\MechAssault.exe, README.txt, licenses\, resources\{game,settings,saves,logs,runtime\compiler}
rem The executable links the C runtime statically (MECHASSAULT_STATIC_CRT), so the root holds one file to start.
set "PKG=%OUT%\package\%GAME_EXE%"
rd /s /q "%OUT%\package" 2>nul
mkdir "%PKG%\licenses" "%PKG%\resources\game" "%PKG%\resources\settings" "%PKG%\resources\saves" "%PKG%\resources\logs" "%PKG%\resources\runtime"
copy /y "%OUT%\%GAME_EXE%.exe" "%PKG%\" >nul || exit /b 1
copy /y docs\release\README.txt "%PKG%\README.txt" >nul
copy /y "%SDK%\THIRD_PARTY.txt" "%PKG%\licenses\" >nul
copy /y "%SDK%\LICENSE" "%PKG%\licenses\XboxXBE-Recomp-LICENSE.txt" >nul
copy /y "%SDK%\tools\LLVM-LICENSE.txt" "%PKG%\licenses\" >nul
rem the compiler without its work folder or any built game code: the first start builds the game from the player's copy
robocopy "%OUT%\compiler" "%PKG%\resources\runtime\compiler" /e /xd work /xf *.dll *.obj *.lib /njh /njs /nfl /ndl >nul
if errorlevel 8 exit /b 1
rem the import library of MechAssault.exe, which the game DLL links against on the player's PC (no game code)
copy /y "%OUT%\compiler\MechAssault.lib" "%PKG%\resources\runtime\compiler\" >nul || (echo MechAssault.lib missing from %OUT%\compiler & exit /b 1)
rem xbe_recomp.exe is linked statically as well; older builds needed the VC runtime beside it
if not exist "%OUT%\compiler\xbe_recomp.exe" (echo compiler\xbe_recomp.exe missing & exit /b 1)
where 7z >nul 2>nul || (echo 7-Zip not found: the package folder is ready, zip it by hand: %PKG% & exit /b 0)
7z a "%GAME_EXE%.zip" "%OUT%\package\%GAME_EXE%" >nul || exit /b 1
echo %GAME_EXE%.zip written from %PKG%
