# Grand Theft Auto: Liberty City Stories (PlayStation 2, USA v1.01 (SLUS-21423))

Native PC port profile for PS2Recomp: the few overrides this game still needs on top of the generic pipeline.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches the first mission: intro movies, menus, the opening and the first mission. Not validated further.
- The IOP modules of the disc (CDSTREAM, SDRDRV and the rest) run as statically recompiled code; VU1 still runs in the interpreter (92-95% busy).
- Speed not measured yet.
- Built by the generic pipeline (07/11/2026): 5,537 functions from Ghidra plus 2,232 found by the `gaps` step; 118 SDK functions served by the runtime's native handlers. With no input it plays the logo, the intro credits and reaches the 3D city behind the title, no missing code, no errors. `handlers.cpp` keeps only the end-of-movie notice.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/`:

```bat
python ..\..\tools\ps2port\ps2port.py all --port .
Play.cmd
```

This game is built by the same pipeline as any other PS2 game ([tools/ps2port](../../tools/ps2port/README.md));
`port.toml` holds only what is still specific to it (with `handlers.cpp`, its own native handlers). `installer.json` lists the files and hashes of the
supported disc. Generated code, Ghidra exports and builds stay on your PC. Windows only for now.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

Grand Theft Auto: Liberty City Stories, its code, trademarks and assets belong to Rockstar Games, Rockstar Leeds, Take-Two Interactive and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
