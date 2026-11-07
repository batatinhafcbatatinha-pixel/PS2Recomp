@echo off
rem Headless Ghidra import/analysis of SLUS_215.90 (EE extension) + PS2Recomp function export.
setlocal
set "PS2RECOMP=%~dp0..\.."
if exist "%~dp0..\..\ps2xRuntime\CMakeLists.txt" set "PS2RECOMP=%~dp0..\.."
if not defined GHIDRA set "GHIDRA=%~dp0..\..\.local\tools\ghidra-12.1.4"
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (
  echo Ghidra not found at "%GHIDRA%". Set GHIDRA to your Ghidra folder.
  exit /b 1
)
if not exist "%~dp0ghidra" mkdir "%~dp0ghidra"
set "SCRIPTS=%PS2RECOMP%\ps2xRecomp\tools\ghidra"
if not exist "%SCRIPTS%" set "SCRIPTS=%~dp0..\..\ps2xRecomp\tools\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" GtaVcs -import "%~dp0game_files\SLUS_215.90" -overwrite ^
  -processor "r5900:LE:32:default" -analysisTimeoutPerFile 3600 ^
  -scriptPath "%SCRIPTS%" -postScript ExportPS2Functions.java "%~dp0config\gtavcs.toml" "%~dp0config\ghidra-functions.csv"
if errorlevel 1 exit /b 1
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" GtaVcs -process SLUS_215.90 -noanalysis -readOnly ^
  -scriptPath "%~dp0scripts" -postScript ExportVcsFunctionsOnly.java "%~dp0config\ghidra-functions-only.csv"
exit /b %errorlevel%
