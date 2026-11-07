@echo off
rem Build PS2Recomp (analyzer, recompiler, runtime) for the CoD:WaW Final Fronts port.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
rem Deep submodule paths in FetchContent deps exceed MAX_PATH; enable long paths for this process only.
set "GIT_CONFIG_COUNT=1"
set "GIT_CONFIG_KEY_0=core.longpaths"
set "GIT_CONFIG_VALUE_0=true"
set "SRC=%~dp0..\..\..\.."
cmake -S "%SRC%" -B "%~dp0..\..\build\ps2recomp" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "%~dp0..\..\build\ps2recomp" --parallel 12
exit /b %errorlevel%
