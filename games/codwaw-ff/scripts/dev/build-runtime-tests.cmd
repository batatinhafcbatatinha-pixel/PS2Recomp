@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake --build "%~dp0..\..\build\game" --target ps2x_tests ps2EntryRunner --parallel 12
exit /b %errorlevel%
