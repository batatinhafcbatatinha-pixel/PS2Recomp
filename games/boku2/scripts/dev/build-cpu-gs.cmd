@echo off
rem Diagnostic build with the software GS allowed (run with PS2X_GS=cpu); links the AOT archive of build\native.
setlocal
set "VSLANG=1033"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "OUT=D:\CodexBuilds\native-ports\boku2-cpugs"
set "AOT=%~dp0..\..\build\native\ps2recomp\ps2xRuntime\ps2_game_code.lib"
set "AOT=%AOT:\=/%"
cmake -S "%~dp0..\..\project" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOKU2_ALLOW_CPU_GS=ON "-DBOKU2_PREBUILT_AOT=%AOT%" > "%~dp0..\..\analysis\cpugs-configure.log" 2>&1
if errorlevel 1 exit /b 1
cmake --build "%OUT%" --target ps2EntryRunner --parallel 4 > "%~dp0..\..\analysis\cpugs-build.log" 2>&1
exit /b %errorlevel%
