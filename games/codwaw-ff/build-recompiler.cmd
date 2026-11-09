@echo off
rem Builds ps2_recomp and ps2_analyzer from the PS2Recomp submodule into build\recompiler.
setlocal
set "VSLANG=1033"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined CC if not exist "%VCVARS%" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined CC call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
set "PS2R=%~dp0..\.."
if not exist "%PS2R%\CMakeLists.txt" set "PS2R=%~dp0..\.."
cmake -S "%PS2R%" -B "%~dp0build\recompiler" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
if errorlevel 1 exit /b 1
cmake --build "%~dp0build\recompiler" --target ps2_recomp ps2_analyzer
exit /b %errorlevel%
