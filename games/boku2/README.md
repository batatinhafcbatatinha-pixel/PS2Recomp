# Boku no Natsuyasumi 2 (PlayStation 2, Japan (SCPS-15026))

Native PC port profile for PS2Recomp: the few overrides this game still needs on top of the generic pipeline.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- The native executable boots to the title screen, the demo gameplay and New Game, at 56–60 fps on the development PC. It is not validated from start to finish.
- Built by the generic pipeline with no code of its own (07/11/2026): the boot entry the function map misses (`0x20a920`) and the gap functions are found by the `gaps` step; title screen and demo ran with no errors.
- IOP and VU0 still run through interpreters in the runtime; this is not a fully native port yet.
- The new keyboard, mouse and vibration code has not been run in the game yet.

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

Development notes: `NOTES.md`.

Boku no Natsuyasumi 2, its code, trademarks and assets belong to Sony Interactive Entertainment, Millennium Kitchen and the respective rights holders; the English translation belongs to its authors. Not affiliated with or endorsed by them or by Sony.
