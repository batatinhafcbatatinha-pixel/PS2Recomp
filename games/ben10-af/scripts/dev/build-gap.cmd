@echo off
rem Runtime + gap-function rebuild that links the existing AOT archive (never recompiles the 8,760 AOT functions).
setlocal
set "VSLANG=1033"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "OUT=D:\CodexBuilds\native-ports\ben10-af"
set "AOT=%~dp0..\..\build\native\ps2recomp\ps2xRuntime\ps2_game_code.lib"
set "AOT=%AOT:\=/%"
if not defined JOBS set "JOBS=2"
cmake -S "%~dp0..\..\project" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DBEN10_PREBUILT_AOT=%AOT%" > "%~dp0..\..\analysis\native-configure.log" 2>&1
if errorlevel 1 exit /b 1
cmake --build "%OUT%" --target ps2EntryRunner --parallel %JOBS% > "%~dp0..\..\analysis\native-build.log" 2>&1
exit /b %errorlevel%
