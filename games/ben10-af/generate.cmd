@echo off
setlocal
cd /d "%~dp0"
set "PYTHON=%~dp0..\recomp-python\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=python"
"%PYTHON%" scripts\prepare_config.py
if errorlevel 1 exit /b 1
"%PYTHON%" scripts\guarded_generate.py
if errorlevel 1 exit /b 1
"%PYTHON%" scripts\make_chunks.py
exit /b %errorlevel%
