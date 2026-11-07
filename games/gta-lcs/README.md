# Grand Theft Auto: Liberty City Stories (PlayStation 2, USA v1.01 (SLUS-21423))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches the first mission: intro movies, menus, the opening and the first mission. Not validated further.
- The IOP modules of the disc (CDSTREAM, SDRDRV and the rest) run as statically recompiled code; VU1 still runs in the interpreter (92-95% busy).
- Speed not measured yet.

## Building

From this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`):
Ghidra export (`ghidra-export*.cmd`), `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, `build-native.cmd`,
then `Play-LCS.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the
supported disc. Generated code, Ghidra exports and builds stay on your PC.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

Grand Theft Auto: Liberty City Stories, its code, trademarks and assets belong to Rockstar Games, Rockstar Leeds, Take-Two Interactive and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
