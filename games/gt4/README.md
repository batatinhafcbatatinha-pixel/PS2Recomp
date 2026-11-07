# Gran Turismo 4 (PlayStation 2, USA v2.00 (SCUS-97328))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- **Not playable yet: no picture.** The game executable (rebuilt from your `CORE.GT4`) boots, its threads and all 20 IOP modules (including Polyphony's own drivers, statically recompiled) start, but no frame reaches the screen: the game keeps polling the memory card and waiting on IOP requests.
- EE code: about 24,600 functions translated ahead of time, 9,100 recovered "gap" functions, 160 SDK functions routed to the runtime's native handlers.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`): the steps
in `setup.json` in order (Ghidra export, `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, the compile), then
`Play-GT4.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the supported
disc. Generated code, Ghidra exports and builds stay on your PC.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

Gran Turismo 4, its code, trademarks and assets belong to Polyphony Digital, Sony Interactive Entertainment and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
