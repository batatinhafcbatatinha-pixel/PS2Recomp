@echo off
rem Headless Ghidra analysis of your SLUS_217.46 (EE extension) and the PS2Recomp function export.
rem Run "python scripts\prepare_game_code.py --disc" first (Prepare-Build.cmd does) for analysis\disc\SLUS_217.46.
setlocal
if not defined GHIDRA set "GHIDRA=%~dp0..\..\.local\tools\ghidra-12.1.4"
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (
  echo Ghidra not found at "%GHIDRA%". Set GHIDRA to your Ghidra folder.
  exit /b 1
)
if not exist "%~dp0analysis\disc\SLUS_217.46" (
  echo analysis\disc\SLUS_217.46 missing: run python scripts\prepare_game_code.py --disc first.
  exit /b 1
)
if not exist "%~dp0ghidra" mkdir "%~dp0ghidra"
set "PS2R=%~dp0..\.."
if not exist "%PS2R%\CMakeLists.txt" set "PS2R=%~dp0..\.."
set "SCRIPTS=%PS2R%\ps2xRecomp\tools\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" CoDWaWFF -import "%~dp0analysis\disc\SLUS_217.46" -overwrite ^
  -processor "r5900:LE:32:default" -analysisTimeoutPerFile 7200 ^
  -scriptPath "%SCRIPTS%" -postScript ExportPS2Functions.java "%~dp0config\codwaw-ff.toml" "%~dp0config\ghidra-functions.csv"
exit /b %errorlevel%
