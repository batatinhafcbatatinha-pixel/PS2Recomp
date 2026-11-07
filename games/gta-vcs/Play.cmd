@echo off
rem Starts this game (built with tools/ps2port; see port.toml).
python "%~dp0..\..\tools\ps2port\ps2port.py" play --port "%~dp0." %*
exit /b %errorlevel%
