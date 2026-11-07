# macOS (native build)

**Experimental: this build has not been run on a real Mac yet.** Read the status below before you start.

The same sources build natively for Apple Silicon and Intel Macs: no Wine, no emulator, no compatibility layer. The window
and input are raylib/GLFW, video decoding is FFmpeg from Homebrew and audio is miniaudio (Core Audio).

macOS stops at OpenGL 4.1, so the GPU rasterizer used on Windows and Linux (OpenGL 4.6 + fragment interlock) cannot
run. The GS is the runtime's software rasterizer (`PS2X_GS=cpu`). `project/CMakeLists.txt` selects this on Apple with
`BOKU2_SOFTWARE_GS`, and `run-macos.sh` sets the variable. The EE, VU1 and GS code use SSE intrinsics, translated to NEON by [sse2neon](https://github.com/DLTcollab/sse2neon) on Apple Silicon.

## Status (2026-10-31)

| Check | Result |
| --- | --- |
| PS2Recomp tools, runtime and tests built with Apple's SDK (GitHub `macos-15` arm64, `macos-15-intel`) | Pass |
| Software GS Gouraud tests on those two Mac runners | Pass |
| PS2Recomp test suite on those two Mac runners | 459/461 (Intel) and 457/461 (arm64); the failures are EE kernel and VBlank tests (the set changes between runs) |
| Whole game built for ARM64 with Clang, targeting Linux | Pass |
| ARM64 game code run under `qemu-aarch64` on x86-64 Linux, software GS | No crash; reached the Sony logo after about 40 minutes, and all 68 guest events in the x86-64 reference run matched. On the current runtime, a 20-minute run matched 41 of 41 events and stopped before the logo |
| x86-64 Linux with these sources and the software GS | Title and demo gameplay render |
| The game code built with Apple's SDK | **Not tested** (generated code is never uploaded, so CI cannot build it) |
| Any run on a real Mac, including sound, controllers and the window | **Not tested** |

Performance on a Mac is unknown. The x86-64 reference ran at 6 to 15 game fps in that test, on a virtual display under WSL with
software OpenGL and four render threads. That is not a Mac figure, but the software GS is much slower than the GPU rasterizer,
so expect it to be the main bottleneck. Use `-DBOKU2_OPTIMIZE_AOT=ON` on the first configure.

Known visual difference: at the edges of characters, the software GS leaves a comb pattern in the hair and clothes.

## Requirements

- macOS 15 or later (older versions may work; CI only covers 15), Apple Silicon or Intel.
- Xcode Command Line Tools: `xcode-select --install`
- [Homebrew](https://brew.sh): `brew install cmake ninja pkg-config ffmpeg`
- Your own disc: the extracted files in `game_files/`.

## Generated code

The repository has no game code. `project/runner` and `project/runner-chunks` must be generated from your own disc first,
and the pipeline is not clone-and-build yet, on any platform. It needs the Ghidra function export (not published), and the files in `config/`
and `scripts/generate.py` contain the author's Windows paths. If you already generated the code on another machine, copy `project/` over: the
generated sources are portable.

## Getting the sources

```sh
git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/boku-no-natsuyasumi-2-pc.git
```

`third_party/PS2Recomp` points at branch `macos` of the [PS2Recomp fork](https://github.com/batatinhafcbatatinha-pixel/PS2Recomp).

## Build and run

```sh
./build-macos.sh -DBOKU2_OPTIMIZE_AOT=ON   # configure once + build into build/macos (JOBS=2 by default)
./run-macos.sh                             # boots game_files/SCPS_150.26
./run-macos.sh /path/to/game.iso           # or use your disc image (raw sector reads, FMV)
```

The first build compiles all the generated code and needs several GB of memory per job, so raise `JOBS` carefully.

`PS2X_THREAD_STACK_MB` changes the game thread's stack (64 MiB by default). macOS gives `std::thread` only 512 KiB, so the
runtime creates this thread itself.

If macOS blocks the executable, it was probably copied from another machine. Build it locally instead.
