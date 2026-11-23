# Developing

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
powershell -ExecutionPolicy Bypass -File tools\port-installer\setup\setup.ps1 -GameDir games\<game> -ToolsDir .local\tools -Source "D:\ISO\your-disc.iso"
```

Or step by step inside `games/<game>/`: the `steps` of its `setup.json` in order (Ghidra export, `build-recompiler.cmd`,
`python scripts/prepare_game_code.py`, `build-native.cmd`), then `Play-<game>.cmd`. Linux/macOS: `build-linux.sh` /
`build-macos.sh` in the games that have them.
