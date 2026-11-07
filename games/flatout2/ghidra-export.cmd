@echo off
rem Headless Ghidra import/analysis of SLUS_212.51 (EE extension) + PS2Recomp function export.
setlocal
set "PS2RECOMP=%~dp0third_party\PS2Recomp"
if exist "%~dp0..\..\ps2xRuntime\CMakeLists.txt" set "PS2RECOMP=%~dp0..\.."
if not defined GHIDRA set "GHIDRA=C:\Users\dudua\Downloads\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC"
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (
  echo Ghidra not found at "%GHIDRA%". Set GHIDRA to your Ghidra folder.
  exit /b 1
)
if not exist "%~dp0ghidra" mkdir "%~dp0ghidra"
set "SCRIPTS=%PS2RECOMP%\ps2xRecomp\tools\ghidra"
if not exist "%SCRIPTS%" set "SCRIPTS=%~dp0..\third_party\PS2Recomp\ps2xRecomp\tools\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" FlatOut2 -import "%~dp0game_files\SLUS_212.51" -overwrite ^
  -processor "r5900:LE:32:default" -analysisTimeoutPerFile 3600 ^
  -scriptPath "%SCRIPTS%" -postScript ExportPS2Functions.java "%~dp0config\flatout2.toml" "%~dp0config\ghidra-functions.csv"
if errorlevel 1 exit /b 1
call "%GHIDRA%\support\analyzeHeadless.bat" "%~dp0ghidra" FlatOut2 -process SLUS_212.51 -noanalysis -readOnly ^
  -scriptPath "%~dp0scripts" -postScript ExportFo2FunctionsOnly.java "%~dp0config\ghidra-functions-only.csv"
exit /b %errorlevel%
