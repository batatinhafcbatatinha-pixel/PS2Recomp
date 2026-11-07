@echo off
rem Fast edit/test loop. Uses the existing CMake configuration and chunk layout.
rem Does not regenerate chunks or force a CMake configure on every invocation.
rem
rem   build-incremental.cmd [game|runtime] [jobs]
rem
rem The game code compiles against a frozen header snapshot (build\game-abi, kept by
rem scripts\sync_game_abi.py), so runtime-only header edits do not rebuild it.
rem Without a jobs argument, parallelism follows free memory (up to all cores).
setlocal
if not exist "%~dp0build\game\build.ninja" (
  echo Configure once with build-game.cmd full before using this command.
  exit /b 1
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "JOBS=%~2"
if not defined JOBS (
  for /f %%j in ('powershell -NoProfile -Command "$free=(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1MB; $cores=[Environment]::ProcessorCount; [Math]::Max(4,[Math]::Min($cores,[Math]::Floor($free/0.5)))"') do set "JOBS=%%j"
)
echo [build] jobs=%JOBS%
python "%~dp0scripts\sync_game_abi.py"
if errorlevel 1 exit /b 1
set "ABI=%~dp0build\game-abi"
set "ABI=%ABI:\=/%"
findstr /c:"PS2X_GAME_ABI_DIR:PATH=%ABI%" "%~dp0build\game\CMakeCache.txt" >nul
if errorlevel 1 (
  cmake "%~dp0build\game" "-DPS2X_GAME_ABI_DIR=%ABI%"
  if errorlevel 1 exit /b 1
)
rem The runtime-only mode reuses the existing generated-code library.
if /i "%~1"=="runtime" goto exe
rem Ninja tracks included generated .cpp files, so editing one rebuilds its chunk.
cmake --build "%~dp0build\game" --target ps2_game_code --parallel %JOBS%
if errorlevel 1 exit /b 1
:exe
cmake --build "%~dp0build\game" --target ps2EntryRunner --parallel %JOBS%
exit /b %errorlevel%
