@echo off
setlocal
set "PYTHON=%~dp0..\recomp-python\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=python"
"%PYTHON%" "%~dp0scripts\prepare_game_code.py" %*
exit /b %errorlevel%
