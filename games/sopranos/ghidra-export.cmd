@echo off
rem Headless Ghidra (R5900 / EmotionEngine) analysis of SLUS_213.88 + PS2Recomp function export.
rem Exports the PS2Recomp TOML (stubs + SDK bindings) and the function CSVs.
setlocal
if not defined GHIDRA set "GHIDRA=%~dp0..\..\.local\tools\ghidra-12.1.4"
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (
  echo Ghidra not found at "%GHIDRA%". Set GHIDRA to your Ghidra folder.
  exit /b 1
)
set "DATA=%~dp0."
if exist "%~dp0build-location.txt" set /p DATA=<"%~dp0build-location.txt"
if not exist "%DATA%\ghidra" mkdir "%DATA%\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%DATA%\ghidra" SOP -import "%DATA%\game_files\SLUS_213.88" -overwrite ^
  -processor "r5900:LE:32:default" -analysisTimeoutPerFile 7200 ^
  -scriptPath "%~dp0scripts" -postScript ExportPS2Functions.java "%~dp0config\ghidra.toml" "%~dp0config\ghidra-labels.csv"
if errorlevel 1 exit /b 1
call "%GHIDRA%\support\analyzeHeadless.bat" "%DATA%\ghidra" SOP -process SLUS_213.88 -noanalysis -readOnly ^
  -scriptPath "%~dp0scripts" -postScript ExportSopFunctionsOnly.java "%~dp0config\ghidra-functions-only.csv"
exit /b %errorlevel%
