@echo off
setlocal
set "PS2X_CD_IMAGE="

set "BIN_DIR=%~dp0build\native\ps2recomp\ps2xRuntime"
if exist "%~dp0build-location.txt" set /p BIN_DIR=<"%~dp0build-location.txt"
if not exist "%BIN_DIR%\ps2EntryRunner.exe" (
  echo The game is not built yet: run Setup.cmd first.
  echo O jogo ainda nao foi montado: rode o Setup.cmd primeiro.
  pause
  exit /b 1
)
rem Native renderer: render targets at the resolution chosen in the F1 menu.
if not defined PS2X_GS set "PS2X_GS=hw"
if not defined PS2X_FPS_LIMIT set "PS2X_FPS_LIMIT=240"
if not defined PS2X_PIN if %NUMBER_OF_PROCESSORS% GEQ 12 set "PS2X_PIN=vif:2,gsfe:0,gs:4,game:6,gl:8"
if not "%~2"=="" set "PS2X_WINDOW_MODE=%~2"
cd /d "%~dp0"
if not exist "%~dp0game_files\.disc\layout.tsv" (
  python "%~dp0..\..\tools\port-installer\prepare_folder.py" --root "%~dp0." --config "%~dp0installer.json"
  if errorlevel 1 exit /b 1
)
"%BIN_DIR%\ps2EntryRunner.exe" "%~dp0game_files\SLUS_215.90"
exit /b %errorlevel%
