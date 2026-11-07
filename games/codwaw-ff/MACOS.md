# macOS (native build)

**Experimental: this build has never been compiled or run with this game's code on macOS.** Read the status below before you start.

The same sources are set up to build natively for Apple Silicon and Intel Macs: no Wine, no emulator, no compatibility layer. The
window and input are raylib/GLFW, video decoding is FFmpeg from Homebrew and audio is miniaudio (Core Audio).

macOS stops at OpenGL 4.1, so the GPU rasterizer (OpenGL 4.6 + fragment interlock) cannot run. The GS is the runtime's software
rasterizer (`PS2X_GS=cpu`). `project/CMakeLists.txt` selects this on Apple with `CODWAW_SOFTWARE_GS`, and `run-macos.sh` sets the variable.
SSE intrinsics are translated to NEON by [sse2neon](https://github.com/DLTcollab/sse2neon) on Apple Silicon.

## Status (2026-10-31)

| Check | Result |
| --- | --- |
| PS2Recomp tools, runtime and tests built with Apple's SDK (GitHub `macos-15` arm64, `macos-15-intel`) | Pass |
| PS2Recomp test suite on those two Mac runners | 459/461 (Intel) and 457/461 (arm64); the failures are EE kernel and VBlank tests (the set changes between runs) |
| Final Fronts built for macOS, or for ARM64 on any system | **Not tested** |
| Final Fronts with the software GS on any system, using these changes | **Not tested** |
| Any run on a real Mac, including sound, controllers and the window | **Not tested** |

The same runtime and approach were tested further with [Boku no Natsuyasumi 2](https://github.com/batatinhafcbatatinha-pixel/boku-no-natsuyasumi-2-pc/blob/main/MACOS.md). Expect
the software GS to be much slower than the GPU rasterizer. Performance on a Mac is unknown.

## Requirements

- macOS 15 or later (older versions may work; CI only covers 15), Apple Silicon or Intel.
- Xcode Command Line Tools: `xcode-select --install`
- [Homebrew](https://brew.sh): `brew install cmake ninja pkg-config ffmpeg`
- Your own disc: the extracted files in `game_files/`.

## Generated code

The repository has no game code. `project/runner` and `project/runner-chunks` must be generated from your own disc first, and the
pipeline is not clone-and-build yet, on any platform. The Ghidra exports are not published, and the scripts and configuration expect
the author's workspace. If you already generated the code on another machine, copy `project/` over: the generated sources are portable.

The private supplements (`codwaw_*_chunk_*.cpp`) include generated game code and are not published. Without them, `project/CMakeLists.txt` builds with `PS2X_ENABLE_CODWAW_SUPPLEMENTS=OFF`; that configuration has not been run on any Mac either.

## Getting the sources

```sh
git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/cod-world-at-war-final-fronts-pc.git
```

`third_party/PS2Recomp` points at branch `macos` of the [PS2Recomp fork](https://github.com/batatinhafcbatatinha-pixel/PS2Recomp).

## Build and run

```sh
./build-macos.sh                     # configure once + build into build/macos (JOBS=2 by default)
./run-macos.sh                       # boots game_files/SLUS_217.46
./run-macos.sh /path/to/game.iso     # or use your disc image (raw sector reads, FMV)
```

The first build compiles all the generated code and needs several GB of memory per job, so raise `JOBS` carefully.

`PS2X_THREAD_STACK_MB` changes the game thread's stack (64 MiB by default). macOS gives `std::thread` only 512 KiB, so the
runtime creates this thread itself.
