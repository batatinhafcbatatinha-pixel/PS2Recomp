@echo off
setlocal
call "%~dp0build-runtime-tests.cmd"
if errorlevel 1 exit /b 1
pushd "%~dp0..\..\..\.."
"%~dp0..\..\build\game\ps2xTest\ps2x_tests.exe"
set "TEST_RESULT=%errorlevel%"
popd
exit /b %TEST_RESULT%
