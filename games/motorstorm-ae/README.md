# MotorStorm: Arctic Edge (PlayStation 2, USA (SCUS-97654))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- **Not playable yet: the menu stays black.** The game boots and runs at 30 fps, but no texture is ever uploaded to the graphics synthesizer, so only outlines are drawn.
- The IOP modules of the disc (including the MultiStream streamer) run as statically recompiled code.

## Building

From this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`):
Ghidra export (`ghidra-export*.cmd`), `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, `build-native.cmd`,
then `Play-MotorStorm.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the
supported disc. Generated code, Ghidra exports and builds stay on your PC.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

MotorStorm: Arctic Edge, its code, trademarks and assets belong to BigBig Studios, Sony Interactive Entertainment and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
