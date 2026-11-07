@echo off
setlocal
set "PS2X_CD_IMAGE="
set "DATA=%~dp0."
if exist "%~dp0build-location.txt" set /p DATA=<"%~dp0build-location.txt"
set "BIN_DIR=%DATA%\build\native\ps2recomp\ps2xRuntime"
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
if not exist "%DATA%\game_files\.disc\layout.tsv" (
  rem Original disc LBNs when this folder holds the extraction report, else the installer's layout.
  python "%~dp0scripts\prepare_layout.py" 2>nul || python "%~dp0..\..\tools\port-installer\prepare_folder.py" --root "%DATA%" --config "%~dp0installer.json"
  if errorlevel 1 exit /b 1
)
"%BIN_DIR%\ps2EntryRunner.exe" "%DATA%\game_files\SLUS_213.88"
exit /b %errorlevel%
