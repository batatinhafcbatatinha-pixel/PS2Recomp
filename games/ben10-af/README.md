# Ben 10: Alien Force (PlayStation 2, USA (SLUS-21815))

Native PC port profile for PS2Recomp: this game's recompiler configuration, native handlers and build scripts.
**No game content is here:** no disc image, executable, game data or generated code. You need your own disc.

## Status

- Reaches gameplay. Checked with an existing save: profiles, main menu, credits, Continue, level selection and the pier level with HUD, subtitles and character. Not validated from start to finish.
- Speed on the development PC: 58–60 fps in the menus and 32–36 fps on the pier, below the 60 fps goal.
- EE (R5900) code: 8,760 functions from Ghidra plus 1,174 recovered "gap" functions; 125 SDK functions (SIF, CD, pad, memory card, sound, MPEG) are routed to the runtime's native handlers.
- Windows and [Linux](LINUX.md) builds use the same sources. [macOS](MACOS.md) build files are included: experimental, and not compiled or run with this game's code yet.
- IOP and VU0 still run through interpreters in the runtime; this is not a fully native port yet.
- The new keyboard, mouse and vibration code has not been run in the game yet.

## Building

Easiest: the launcher of this repository (`Launcher.cmd` at the root, see the [main README](../../README.md)) picks this
game from your disc image, installs the tools, builds it and starts it.

By hand, from this folder, with your extracted disc in `game_files/` (or a folder named in `build-location.txt`): the steps
in `setup.json` in order (Ghidra export, `build-recompiler.cmd`, `python scripts/prepare_game_code.py`, the compile), then
`Play-Ben10.cmd`. The runtime is this PS2Recomp repository (`../..`). `installer.json` lists the files and hashes of the supported
disc. Generated code, Ghidra exports and builds stay on your PC. Other platforms: [Linux](LINUX.md), [macOS](MACOS.md).

Development notes: `NOTES.md`.

Ben 10: Alien Force, its code, trademarks and assets belong to Cartoon Network, D3Publisher, Monkey Bar Games and the respective rights holders. Not affiliated with or endorsed by them or by Sony.
