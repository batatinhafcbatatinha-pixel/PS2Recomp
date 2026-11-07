@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "IOP=%~dp0..\..\..\..\ps2xIOP"
cl /nologo /EHsc /std:c++20 /MD /O2 /I"%IOP%\include" /I"%IOP%\src" "%IOP%\tests\iop_import_tests.cpp" /Fo"%~dp0..\..\build\iop-import-tests.obj" /Fe"%~dp0..\..\build\iop-import-tests.exe" /link "%~dp0..\..\build\game\ps2xIOP\ps2_iop.lib"
if errorlevel 1 exit /b 1
"%~dp0..\..\build\iop-import-tests.exe"
exit /b %errorlevel%
