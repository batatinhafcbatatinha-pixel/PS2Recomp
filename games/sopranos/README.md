# The Sopranos: Road to Respect (PlayStation 2, USA v1.01 (SLUS-21388))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- **Not playable yet.** Main menu and New Game work; the opening cutscene starts and stops at its second line of dialogue (the streamed speech never advances).
- The IOP modules of the disc run as statically recompiled code; SPU2 audio is native.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`): the steps
in `setup.json` in order (Ghidra export, `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, the compile), then
`Play-Sopranos.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the supported
disc. Generated code, Ghidra exports and builds stay on your PC.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

The Sopranos: Road to Respect, its code, trademarks and assets belong to Seven Studios, THQ, HBO and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
