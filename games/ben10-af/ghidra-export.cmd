@echo off
rem Headless Ghidra import/analysis of SLUS_218.15 (EE extension) + PS2Recomp function export.
setlocal
if not defined GHIDRA set "GHIDRA=%~dp0..\..\.local\tools\ghidra-12.1.4"
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (
  echo Ghidra not found at "%GHIDRA%". Set GHIDRA to your Ghidra folder.
  exit /b 1
)
if not exist "%~dp0ghidra" mkdir "%~dp0ghidra"
set "SCRIPTS=%~dp0..\..\ps2xRecomp\tools\ghidra"
if not exist "%SCRIPTS%" set "SCRIPTS=%~dp0..\..\ps2xRecomp\tools\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" Ben10AF -import "%~dp0game_files\SLUS_218.15" -overwrite ^
  -processor "r5900:LE:32:default" -analysisTimeoutPerFile 600 ^
  -scriptPath "%SCRIPTS%" -postScript ExportPS2Functions.java "%~dp0config\ben10-af.toml" "%~dp0config\ghidra-functions.csv"
if errorlevel 1 exit /b 1
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" Ben10AF -process SLUS_218.15 -noanalysis -readOnly ^
  -scriptPath "%~dp0scripts" -postScript ExportBen10FunctionsOnly.java "%~dp0config\ghidra-functions-only.csv"
exit /b %errorlevel%
