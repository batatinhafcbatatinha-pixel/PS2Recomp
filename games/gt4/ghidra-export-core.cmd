@echo off
rem Headless Ghidra (R5900 / EmotionEngine) analysis of the real game executable:
rem CORE.GT4 decoded into CORE.ELF by scripts/build_core_elf.py.
rem Exports the PS2Recomp TOML (stubs + SDK bindings) and the function CSVs.
setlocal
if not defined GHIDRA set "GHIDRA=%~dp0..\..\.local\tools\ghidra-12.1.4"
if not exist "%GHIDRA%\support\analyzeHeadless.bat" (
  echo Ghidra not found at "%GHIDRA%". Set GHIDRA to your Ghidra folder.
  exit /b 1
)
if not defined GT4_BUILD set "GT4_BUILD=%~dp0."
if exist "%~dp0build-location.txt" set /p GT4_BUILD=<"%~dp0build-location.txt"
if not exist "%GT4_BUILD%\ghidra" mkdir "%GT4_BUILD%\ghidra"
call "%GHIDRA%\support\analyzeHeadless.bat" "%GT4_BUILD%\ghidra" GT4CORE -import "%GT4_BUILD%\game_files\CORE.ELF" -overwrite ^
  -processor "r5900:LE:32:default" -analysisTimeoutPerFile 7200 ^
  -scriptPath "%~dp0scripts" -postScript ExportPS2Functions.java "%~dp0config\ghidra-core.toml" "%~dp0config\ghidra-core-labels.csv"
if errorlevel 1 exit /b 1
call "%GHIDRA%\support\analyzeHeadless.bat" "%GT4_BUILD%\ghidra" GT4CORE -process CORE.ELF -noanalysis -readOnly ^
  -scriptPath "%~dp0scripts" -postScript ExportGt4FunctionsOnly.java "%~dp0config\ghidra-core-functions-only.csv"
exit /b %errorlevel%
