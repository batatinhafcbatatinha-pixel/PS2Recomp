# Call of Duty: World at War - Final Fronts (PlayStation 2, USA/Canada (SLUS-21746))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Boots through the menus and video playback into the first mission, with scenery, soldiers and HUD drawn. Not validated end to end.
- Speed in the first mission on the development PC: 19–24 fps on the farm and 38–41 fps with the camera on the sky, below the 60 fps goal.
- EE (R5900) code: 81,680 functions recompiled ahead of time to C++, compiled into a game library and linked with the PS2Recomp runtime.
- Graphics: hardware GS renderer and native VU1 blocks adopted from the God of War native port; the stock runtime GS is still available.
- The game code and VU1 are native; IOP and VU0 still run through interpreters in the runtime, so this is not a fully native port yet.
- The new keyboard, mouse, pad layout and vibration code has not been run in the game yet.
- Developed on Windows x64. [Linux](LINUX.md) builds use the same sources but the full game has not been built or run there. [macOS](MACOS.md) build files are included: experimental, and not compiled or run with this game's code yet.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`): the steps
in `setup.json` in order (Ghidra export, `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, the compile), then
`Play-CoD.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the supported
disc. Generated code, Ghidra exports and builds stay on your PC. Other platforms: [Linux](LINUX.md), [macOS](MACOS.md).

Development notes: `NOTES.md`.

Call of Duty: World at War - Final Fronts, its code, trademarks and assets belong to Activision, Rebellion and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
