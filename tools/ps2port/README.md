# ps2port: one pipeline for any PlayStation 2 game

`ps2port.py` builds a native PC port of a PS2 game from its disc files with the same steps for every game. A game
needs no code of its own to be tried: the launcher offers it for any PS2 disc that has no profile in `games/`, in
`.local/ports/<serial>/`. A profile (`games/<game>/port.toml`) only adds what one game still needs.

```bat
python tools\ps2port\ps2port.py all  --port <folder with game_files\>
python tools\ps2port\ps2port.py play --port <same folder>
```

## Steps

| Step | What |
| --- | --- |
| `tools` | builds `ps2_recomp` / `ps2_analyzer` once for the repository (`.local/recompiler`) |
| `ghidra` | headless Ghidra (R5900): function map, labels, kernel syscall wrappers of the boot ELF (from `SYSTEM.CNF`) |
| `analyze` | PS2Recomp analyzer: SDK library matches and extra function starts |
| `config`, `generate` | recompiler configuration (function ends capped at the next start) and translation to C++ |
| `gaps` | functions the map left out: analyzer starts, `entry_` labels, code after returns and tail jumps, code pointers in the data, prologues Ghidra merged into the previous function, and addresses a run reported missing |
| `sdk` | SDK functions served by the runtime's native handlers: analyzer matches plus `sdk_signatures.json` (see below); EE-only library code keeps the game's own translation |
| `finish` | direct calls into bound functions go through the table; very large dispatch switches are bucketed; sources grouped in chunks |
| `iop` | every IOP module on the disc (`*.IRX` and the modules inside IOPRP images) recompiled statically |
| `vu1` | VU1 microprograms captured by earlier runs (`observed/vu1`) compiled to native blocks |
| `compile` | CMake/Ninja build of `tools/ps2port/project`; jobs follow the free memory, a failed build resumes with fewer |
| `play` | runs the game and records what is missing for the next build |

Each step skips itself when its output is current (`--force` redoes it); `prepare` runs everything before `compile`.

## Learning from runs

`play` sets `PS2X_OBSERVE_DIR=<port>/observed`:

- every guest address reached without translated code goes to `observed/missing.txt`; the next `gaps` step translates
  the ones inside the executable natively;
- code outside the executable (overlays, code the game decompresses) also gets a RAM snapshot (`ram_<addr>.bin`);
- VU1 programs are captured to `observed/vu1` for the `vu1` step.

While the game runs, a missing function is also translated and compiled at once (`hotrecomp.py`, with the
compiler the build installed): the runtime loads it as a DLL and continues. Only the first time costs a few seconds.
`play --no-hot` turns this off.

## SDK signatures

`sdk_signatures.json` holds the instruction words of SDK functions the analyzer does not recognise, with address
fields masked, learned from games where they were identified (`learn_sdk.py <ELF> name@0xADDR ...`). A signature
binds only when it matches exactly once; short functions are matched at a fixed offset from a known function of the
same library object. Every game's identification helps the next game.

## port.toml (optional)

```toml
name = "Gran Turismo 4"
elf = "CORE.ELF"                         # else the BOOT2 executable of SYSTEM.CNF
[env]                                    # defaults for the game (an explicit environment value wins)
PS2X_IOP_USB = "1"
[sdk]
extra = ["sceCdSearchFile@0x365758"]     # functions to bind that no signature finds yet
exclude = ["sceSomething"]               # keep the game's own translation
[gaps]
starts = ["0x20A920"]                    # extra function starts
[profile]
pre = [["{python}", "scripts/build_core_elf.py", "{files}"]]   # commands before prepare ({files} = game_files)
handlers = "handlers.cpp"                # C++ with PortProfileInstall() for what is still game-specific
```

No game content is stored anywhere in this folder or produced into the repository: everything generated stays in
the port folder on your PC.
