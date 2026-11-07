@echo off
rem Incremental rebuild of the LCS runner (runtime, main.cpp, gap units); the game library is reused when unchanged.
setlocal
set "VSLANG=1033"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set "DATA=%~dp0."
if exist "%~dp0build-location.txt" set /p DATA=<"%~dp0build-location.txt"
cmake --build "%DATA%\build\native" --target ps2EntryRunner --parallel %1
exit /b %errorlevel%
