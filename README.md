## PS2Recomp: PlayStation 2 Static Recompiler (fork with game ports)

Fork of [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) with a faster, more complete runtime, a set of game
profiles in [`games/`](games/) and a launcher that builds and starts them from **your own disc**. No game content
(disc image, executable, data or generated code) is in this repository.

## How to play a game

Windows 10/11 x64. About 20-40 GB free and one to several hours for the first build of a game.

1. Get this repository, either way:
   - `git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/PS2Recomp.git`, or
   - **Code > Download ZIP** and unpack it, or
   - only [`Launcher.exe`](https://github.com/batatinhafcbatatinha-pixel/port-installer/releases/latest/download/Launcher.exe):
     run it, choose a folder, and it downloads this repository there.
2. Run **`Launcher.cmd`** (it downloads `Launcher.exe` from the
   [port-installer releases](https://github.com/batatinhafcbatatinha-pixel/port-installer/releases) the first time).
3. Click **CHOOSE ISO...** and pick your disc image (`.iso`). The game is recognised from the disc.
4. Confirm **Install**. The launcher installs what is missing (Python, Visual Studio C++ Build Tools, CMake, Ninja,
   Ghidra + the PS2 extension, Java), copies and checks the game files, translates the game code and compiles it.
   Stopping is safe: finished steps are skipped next time. Log: **Install** tab and `games/<game>/setup-log.txt`.
5. **PLAY**. **PC settings** in the launcher, or **F1** (Back+Start on a controller) in the game: display mode,
   resolution, frame limit, V-Sync, graphics card, controls, language, volume.

| Game | Disc | State |
| --- | --- | --- |
| Ben 10: Alien Force | USA (SLUS-21815) | reaches gameplay |
| Boku no Natsuyasumi 2 | Japan (SCPS-15026) | title, demo and New Game |
| Call of Duty: World at War - Final Fronts | USA/Canada (SLUS-21746) | reaches the first mission |
| FlatOut 2 | USA (SLUS-21251) | reaches races |
| Grand Theft Auto: Liberty City Stories | USA v1.01 (SLUS-21423) | reaches the first mission |
| Grand Theft Auto: Vice City Stories | USA (SLUS-21590) | reaches gameplay |
| Gran Turismo 4 | USA v2.00 (SCUS-97328) | not playable yet |
| MotorStorm: Arctic Edge | USA (SCUS-97654) | not playable yet |
| The Sopranos: Road to Respect | USA v1.01 (SLUS-21388) | not playable yet |

These discs have a profile (checked by SHA-256, see each `games/<game>/installer.json`). Details per game:
[`games/`](games/README.md). Saves go to `mc0/` and `mc1/` in the game folder.

### Any other game

Choose any other PlayStation 2 disc the same way: the launcher offers to build it with the generic pipeline
([`tools/ps2port`](tools/ps2port/README.md)), the same steps for every game, no game-specific code. It may not work
yet, or stop somewhere; while it runs, code the analysis missed is translated and compiled on the spot (a short
pause the first time) and goes into the next build. These ports live in `.local/ports/<serial>/`.

## Differences from upstream

- `games/`: nine game profiles (recompiler config, native handlers, build and play scripts)
- `tools/ps2port`: one pipeline for any game (Ghidra export, gap functions by general rules, SDK functions by learned signatures, IOP modules incl. IOPRP images, VU1 from captures, RAM-aware build)
- missing guest code recorded while playing (`PS2X_OBSERVE_DIR`) and translated + compiled at run time (`ps2_hot.cpp`)
- IOP-side client services shared by all games (loadfile, libfs, directory listing, pad2)
- runtime arena below the executable by default (no clash with a stack or heap at the top of RAM)
- launcher + one-click install/build/play (`Launcher.cmd`, [port-installer](https://github.com/batatinhafcbatatinha-pixel/port-installer) submodule in `tools/port-installer`)
- runs from the extracted disc folder (no ISO at run time), sector layout kept
- hardware GS renderer (OpenGL 4.6): internal resolution, GPU detile and scanout, mipmaps, shared-texture present
- GS frontend, VIF1/VU1/GIF on their own threads; flips queued behind pending work
- native VU1 blocks (ahead-of-time), VU1 Q/P/FSAND timing
- PATH3 gating and DMA chain fixes (no cut at 4096 tags), exact Gouraud in the software GS
- IOP modules (IRX) recompiled statically (AOT, HI16/LO16 pairing), IOP kernel, module loader, RPC, SIF and timer fixes
- native SPU2 audio, MPEG video, CDVD and memory card fixes
- OHCI USB host only with `PS2X_IOP_USB=1`
- wall-clock EE timing, scheduler and thread fixes, per-call runtime costs removed
- in-game PC menu (F1): display mode, resolution, refresh rate, V-Sync, frame limit, filter, aspect, FXAA, GPU, FPS counter, language, volume
- SDL3 input: every pad, keyboard and mouse, modern layout, vibration
- profiler and timeline (`PS2X_PROFILE_AT`), GS trace
- macOS build (arm64/x86_64, software GS, experimental) and its CI job

## Building

Windows: Visual Studio 2022 (or Build Tools) with the C++ workload and Windows SDK, CMake 3.25+, Ninja, Python 3.11+.
Game profiles also need Ghidra 12.1.4 with [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)
and Java 21 (the launcher puts all of them in `.local/tools`).

Tools and runtime (from a "x64 Native Tools" prompt):

```bat
git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/PS2Recomp.git
cd PS2Recomp
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build
```

One game, without the launcher (same steps it runs; `-Source` is your disc image):

```bat
powershell -ExecutionPolicy Bypass -File tools\port-installer\setup\setup.ps1 -GameDir games\gta-lcs -ToolsDir .local\tools -Source "D:\ISO\your-disc.iso"
```

Or step by step inside `games/<game>/`: the `steps` of its `setup.json` in order (Ghidra export, `build-recompiler.cmd`,
`python scripts/prepare_game_code.py`, `build-native.cmd`), then `Play-<game>.cmd`. Linux/macOS: `build-linux.sh` /
`build-macos.sh` in the games that have them.

The launcher itself is built from [port-installer](https://github.com/batatinhafcbatatinha-pixel/port-installer)
(`dotnet build launcher/Launcher.csproj`).

## Upstream documentation

[Discord](https://discord.gg/JQ8mawxUEf) and [wiki](https://github.com/ran-j/PS2Recomp/wiki) of the original project.

This project statically recompiles PS2 ELF binaries into C++ and provides a runtime to execute the generated code.

### Modules

* `ps2xAnalyzer`: scans ELF/functions and writes TOML config (`stubs`, `skip`, instruction patches).
* `ps2xRecomp`: reads TOML + ELF, decodes R5900 instructions, and generates C++ output.
* `ps2xRuntime`: hosts memory, function registration, syscall dispatch, and hardware stubs.
* `ps2xIOP`: R3000A IRX execution, a virtual IOP kernel, and generic HLE fallbacks.

### Features

* Translates MIPS R5900 instructions to C++ code
* PS2-specific MMI and VU0 macro support.
* Single-file or multi-file output.
* Configurable stubs, skips, and instruction patches.
* Instruction-driven syscall handling.

### How It Works
PS2Recomp works by:

* Parsing a PS2 ELF file to extract functions, symbols, and relocations
* Decoding the MIPS R5900 instructions in each function
* Translating those instructions to equivalent C++ code
* Generating a runtime that can execute the recompiled code

The translated code is very literal, with each MIPS instruction mapping to a C++ operation. For example, `addiu $r4, $r4, 0x20` becomes `ctx->r4 = ADD32(ctx->r4, 0X20);`.

### Current Behavior

* `stubs` entries generate wrappers that call known runtime syscall/stub handlers by name.
* `stubs` also supports address bindings with `handler@0xADDRESS` for stripped games (for example `sceCdRead@0x00123456`).
* Address bindings also support generic return handlers for triage: `ret0`, `ret1`, `reta0`.
* Recompiler now tries relocation-symbol auto-binding at callsites (`J/JAL`) before raw address dispatch; when relocation symbol is known (for example `sceCdRead`), it can call runtime handlers without manual address mapping.
* Recompiler discovers additional internal static entry targets and emits `entry_...` wrappers for those addresses.
* For unresolved static `J/JAL` sites, generated code falls back to `runtime->lookupFunction(0x...)`.
* `skip` entries are not recompiled and generate explicit `ps2_stubs::TODO_NAMED(...)` wrappers.
* Recompiled `SYSCALL` now calls `runtime->handleSyscall(...)` with the encoded syscall immediate.
* Runtime syscall dispatch tries encoded syscall ID first, then falls back to `$v1`.

### Usage

Preferred workflow for retail or stripped games:

1. Open the ELF in Ghidra.
2. Run `ps2xRecomp/tools/ghidra/ExportPS2Functions.java`.
3. Use the exported TOML and CSV map.
4. Recompile with the exported TOML:

```bash
./ps2_recomp config.toml
```

Fallback workflow for quick local experiments or ELFs with debug symbol :

```bash
./ps2_analyzer your_game.elf config.toml
```

See the [Ghidra Workflow](ps2xAnalyzer/Readme.md#3-ghidra-integration-for-retail-and-stripped-games-preferred) for ghdira instructions.

Then build generated output and link with `ps2xRuntime`.

### Configuration

Main fields in `config.toml`:

* `general.input`: source ELF path.
* `general.ghidra_output`: recommended function map CSV exported from Ghidra.
* `general.output`: generated C++ output folder.
* `general.single_file_output`: one combined cpp or one file per function.
* `general.low_memory_mode`: reduce peak output-generation memory by avoiding retained disassembly strings and forcing serial output generation. Generated instruction comments are still emitted; disassembly text is produced while writing each output file instead of being kept in memory.
* `general.output_worker_threads`: number of output-generation workers (clamped to nproc * 2). A positive value uses exactly that many workers. `0` uses `nproc - 1` when at least two hardware threads are available, otherwise serial output generation. `1` forces serial output generation.
* `general.patch_syscalls`: apply configured patches to `SYSCALL` instructions (`false` recommended).
* `general.patch_cop0`: apply configured patches to COP0 instructions.
* `general.patch_cache`: apply configured patches to CACHE instructions.
* `general.stubs`: names to force as stubs. Also accepts `handler@0xADDRESS` to bind a stripped function address directly to a runtime syscall/stub handler. Includes generic handlers `ret0`, `ret1`, `reta0`.
* `general.skip`: names to force as skipped wrappers.
* `patches.instructions`: raw instruction replacements by address.

Address binding for stripped ELFs:

* Use `handler@0xADDRESS` inside `general.stubs` to map a stripped function start directly to a runtime handler.
* Example: `sceCdRead@0x00123456` binds function start `0x00123456` to `ps2_stubs::sceCdRead(...)`.
* Generic temporary handlers are available: `ret0@0xADDR`, `ret1@0xADDR`, `reta0@0xADDR`.
* Before manual binding, prefer recompilation from a Ghidra-exported TOML/CSV first. The extra boundaries and synthetic entry points are usually more important than manual early triage.
* The address must be the function start in that exact ELF build.
* Addresses are not portable across different games/regions/builds.
* The handler name must exist in runtime call lists (`PS2_SYSCALL_LIST` or `PS2_STUB_LIST`).

Example:

```toml
# stripped function binding by address:
stubs = ["sceCdRead@0x00123456", "SifLoadModule@0x00127890"]
# temporary return handlers:
stubs = ["ret0@0x001D9410", "ret1@0x001D5BC8", "reta0@0x0024B7C0"]
# mixed example:
stubs = ["printf", "sceCdRead@0x00123456", "SifLoadModule@0x00127890"] 
```

### Runtime

To execute the recompiled code.

`ps2xRuntime` currently provides:

* Guest memory model and function dispatch table.
* Some syscall dispatcher with common kernel IDs.
* Basic GS/VU/file/system stubs.
* Foundation to expand and port your game.
* `ps2xIOP` execution of original IRX modules with generic HLE fallbacks.

See [IOP emulation](ps2xIOP/README.md) for module execution and the service boundary.

### Game Override Hooks

Game overrides are runtime-side, build-scoped patch modules.

A game override is C++ code that runs during `loadELF` and can replace EE function bindings by address for one specific game build. IOP RPC/DMA behavior is handled by the `ps2xIOP` emulator and its runtime transport. This is separate from recompilation output and separate from global runtime stubs/syscalls.

API:

* Header: `ps2xRuntime/include/game_overrides.h`
* Register macro: `PS2_REGISTER_GAME_OVERRIDE(name, elfName, entry, crc32, applyFn)`
* Direct bind helper: `ps2_game_overrides::bindAddressHandler(runtime, addr, "handler")`

Use Game Override modules when:

* You need per-game/per-build routing or patches without polluting global behavior.
* You need to bind many addresses, or install custom replacement logic for a specific title.

#### Recommended Iteration Loop

1. Run with minimal config and no aggressive skipping.
2. Fix hard blockers first (`function not found`, syscall TODO, critical IO stubs).
3. Use temporary return stubs only to classify call importance.
4. Promote temporary fixes to real implementations.
5. Move per-game hacks into game overrides keyed by ELF metadata.
6. Re-test from cold boot after each batch.

### Describing Your Game Project

A game project built with PS2Recomp can describe itself in a `.recomp.json` file at the root of its repository. Lists of recomp and decomp projects, such as [recomp.board](https://recomp.fyi), read that file instead of guessing the game, system and status from the README.

> [!NOTE]
> This file is optional: PS2Recomp does not read it and works the same without it. `.recomp.json` and recomp.board are third-party projects; the PS2Recomp developers have no ties to them.

Starter file:

```json
{
  "$schema": "https://recomp.fyi/schema/v1.json",
  "game": "<title as it shipped>",
  "system": "PS2",
  "type": "recomp",
  "toolchain": "PS2Recomp",
  "status": "in-progress",
  "original": { "region": "USA", "serial": "SLUS-20312" }
}
```

* `original` is the release a user must own. On retail discs the ELF is named after the serial (`SLUS_203.12` is `SLUS-20312`), and the prefix gives the region: `SLUS`/`SCUS` USA, `SLES`/`SCES` Europe, `SLPS`/`SLPM`/`SCPS` Japan.
* `status` is one of `exploring`, `in-progress`, `playable`, `released`, `complete`, `paused`. Edit it when the project moves on: a stale status is worse than none.
* Never put a game file, or a link to one, in the file.

Other fields (Wikidata item, target platforms, maintainers, links, what help is wanted) and the JSON Schema are in the [specification](https://recomp.fyi/spec).

### Limitations
 
* Performance is very bad for VU and GS
* Hardware emulation is partial and many paths are stubbed.

###  Acknowledgments

* Inspired by N64Recomp
* Uses ELFIO for ELF parsing
* Uses toml11 for TOML parsing
* Uses fmt for string formatting
* Reference for runtime PCSX2