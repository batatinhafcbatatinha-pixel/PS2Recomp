# Ben 10: Alien Force (PlayStation 2, USA (SLUS-21815))

Native PC port profile for PS2Recomp: the few overrides this game still needs on top of the generic pipeline.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches gameplay. Checked with an existing save: profiles, main menu, credits, Continue, level selection and the pier level with HUD, subtitles and character. Not validated from start to finish.
- Speed on the development PC: 58–60 fps in the menus and 32–36 fps on the pier, below the 60 fps goal.
- Built by the generic pipeline (07/11/2026): 8,760 functions from Ghidra plus 3,054 found by the `gaps` step; 110 SDK functions served by the runtime's native handlers. With no input it runs to the autosave notice, no missing code, no errors. `handlers.cpp` keeps only the 60 fps mode.
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
`port.toml` holds only what is still specific to it (with `handlers.cpp`, its own native handlers). `installer.json` lists the files and hashes of the
supported disc. Generated code, Ghidra exports and builds stay on your PC. Windows only for now.

Development notes: `NOTES.md`.

Ben 10: Alien Force, its code, trademarks and assets belong to Cartoon Network, D3Publisher, Monkey Bar Games and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
