# Grand Theft Auto: Vice City Stories (PlayStation 2, USA (SLUS-21590))

Native PC port profile for PS2Recomp: the few overrides this game still needs on top of the generic pipeline.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches gameplay: legal screen, the opening cutscene and free roaming with HUD and radar. Not validated from start to finish.
- Presentation runs at 60 fps on the development PC; VU1 microcode still runs in the interpreter (about 98% busy).
- Known artifacts: a band of garbage over the top lines of the sky and a thin strip on the right edge.
- Built by the generic pipeline with no code of its own (07/11/2026): 9,550 functions from Ghidra plus 3,053 found by the `gaps` step; 117 SDK functions served by the runtime's native handlers. With no input it reaches the opening scene in 150 s, no missing code, no errors.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/`:

```bat
python ..\..\tools\ps2port\ps2port.py all --port .
Play.cmd
```

This game is built by the same pipeline as any other PS2 game ([tools/ps2port](../../tools/ps2port/README.md));
`port.toml` holds only what is still specific to it. `installer.json` lists the files and hashes of the
supported disc. Generated code, Ghidra exports and builds stay on your PC. Windows only for now.

Development notes (Portuguese): `NOTES.md` / `STATUS.md`.

Grand Theft Auto: Vice City Stories, its code, trademarks and assets belong to Rockstar Games, Rockstar Leeds, Take-Two Interactive and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
