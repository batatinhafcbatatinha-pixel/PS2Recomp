@echo off
setlocal
set "PS2X_CD_IMAGE="

if not exist "%~dp0game_files\.disc\layout.tsv" (
  python "%~dp0..\..\tools\port-installer\prepare_folder.py" --root "%~dp0." --config "%~dp0installer.json"
  if errorlevel 1 exit /b 1
)
if not exist "%~dp0build\game\ps2xRuntime\ps2EntryRunner.exe" (
  echo The game is not built yet: run Setup.cmd first.
  echo O jogo ainda nao foi montado: rode o Setup.cmd primeiro.
  pause
  exit /b 1
)
rem Native renderer: render targets at the resolution chosen in the F1 menu.
if not defined PS2X_GS set "PS2X_GS=hw"
if not defined PS2X_FPS_LIMIT set "PS2X_FPS_LIMIT=240"
if not defined PS2X_PIN if %NUMBER_OF_PROCESSORS% GEQ 12 set "PS2X_PIN=vif:2,gsfe:0,gs:4,game:6,gl:8"
rem Optional second argument: windowed, fullscreen, or borderless.
if not "%~2"=="" set "PS2X_WINDOW_MODE=%~2"
cd /d "%~dp0"
rem Sector and named reads use the extracted files in game_files.
"%~dp0build\game\ps2xRuntime\ps2EntryRunner.exe" "%~dp0game_files\SLUS_217.46"
exit /b %errorlevel%
