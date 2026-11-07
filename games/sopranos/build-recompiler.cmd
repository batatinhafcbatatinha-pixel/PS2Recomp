@echo off
rem Builds ps2_recomp and ps2_analyzer from PS2Recomp (submodule, else the shared checkout) into the build folder.
setlocal
set "PS2RECOMP=%~dp0third_party\PS2Recomp"
if exist "%~dp0..\..\ps2xRuntime\CMakeLists.txt" set "PS2RECOMP=%~dp0..\.."
set "VSLANG=1033"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
set "SRC=%PS2RECOMP%"
if not exist "%SRC%\CMakeLists.txt" set "SRC=%~dp0..\third_party\PS2Recomp"
set "DATA=%~dp0."
if exist "%~dp0build-location.txt" set /p DATA=<"%~dp0build-location.txt"
cmake -S "%SRC%" -B "%DATA%\recompiler" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
if errorlevel 1 exit /b 1
cmake --build "%DATA%\recompiler" --target ps2_recomp ps2_analyzer
exit /b %errorlevel%
