# Grand Theft Auto: Vice City Stories (PlayStation 2, USA (SLUS-21590))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches gameplay: legal screen, the opening cutscene and free roaming with HUD and radar. Not validated from start to finish.
- Presentation runs at 60 fps on the development PC; VU1 microcode still runs in the interpreter (about 98% busy).
- Known artifacts: a band of garbage over the top lines of the sky and a thin strip on the right edge.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`): the steps
in `setup.json` in order (Ghidra export, `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, the compile), then
`Play-VCS.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the supported
disc. Generated code, Ghidra exports and builds stay on your PC.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

Grand Theft Auto: Vice City Stories, its code, trademarks and assets belong to Rockstar Games, Rockstar Leeds, Take-Two Interactive and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
