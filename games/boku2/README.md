# Boku no Natsuyasumi 2 (PlayStation 2, Japan (SCPS-15026))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- The native executable boots to the title screen, the demo gameplay and New Game, at 56–60 fps on the development PC. It is not validated from start to finish.
- Windows, [Linux](LINUX.md) and [macOS](MACOS.md) builds use the same sources. The macOS build is experimental and has not been run on a real Mac.
- EE (R5900) code: 5,329 functions mapped with Ghidra and recompiled; the entry point the function map missed (`0x20a920`) was recovered, plus 791 gap functions.
- A local CMake adaptation (`project/runtime_memory.cmake`) separates the game heap, the runtime arena, callback stacks and the 1 MiB main stack.
- IOP and VU0 still run through interpreters in the runtime; this is not a fully native port yet.
- The new keyboard, mouse and vibration code has not been run in the game yet.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`): the steps
in `setup.json` in order (Ghidra export, `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, the compile), then
`Play-Boku2.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the supported
disc. Generated code, Ghidra exports and builds stay on your PC. Other platforms: [Linux](LINUX.md), [macOS](MACOS.md).

Development notes: `NOTES.md`.

Boku no Natsuyasumi 2, its code, trademarks and assets belong to Sony Interactive Entertainment, Millennium Kitchen and the respective rights holders; the English translation belongs to its authors. Not affiliated with or endorsed by them or by Sony.
