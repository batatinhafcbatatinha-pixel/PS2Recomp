@echo off
setlocal
set "PYTHON=%~dp0..\recomp-python\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=python"
"%PYTHON%" "%~dp0scripts\generate.py" %*
exit /b %errorlevel%
