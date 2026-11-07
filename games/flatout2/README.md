# FlatOut 2 (PlayStation 2, USA (SLUS-21251))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches races. Checked: legal screen, memory card check and a new save, main menu, Career, and a race running with position, laps and music announcements. Not validated from start to finish.
- Speed on the development PC: about 28 fps in a race, below the game's own 30 fps.
- The IOP modules of the disc (including the FMOD sound driver) run as statically recompiled code; SPU2 audio is native.

## Building

From this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`):
Ghidra export (`ghidra-export*.cmd`), `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, `build-native.cmd`,
then `Play-FlatOut2.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the
supported disc. Generated code, Ghidra exports and builds stay on your PC.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

FlatOut 2, its code, trademarks and assets belong to Bugbear Entertainment, Empire Interactive and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
