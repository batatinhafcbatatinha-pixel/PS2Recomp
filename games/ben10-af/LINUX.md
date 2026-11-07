# Linux (native build)

The same sources build natively on Linux x86-64 with GCC or Clang: no Wine, no compatibility layer. The window
and input are raylib/GLFW (X11), the GS rasterizer is the runtime's OpenGL 4.6 backend, video decoding is the
system FFmpeg and audio is miniaudio (ALSA/PulseAudio).

Status (2026-10-29): builds and links; boots to the autosave notice in a headless test (Xvfb + llvmpipe, CPU GS).
Not yet checked on a desktop GPU.

## Requirements

Debian/Ubuntu packages:

```sh
sudo apt install build-essential cmake ninja-build pkg-config git python3 \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libegl-dev libasound2-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

A GPU driver with OpenGL 4.6 and `GL_ARB_fragment_shader_interlock` (current NVIDIA and Intel drivers, and Mesa
drivers that expose it). Without it, configure with `-DBEN10_REQUIRE_GPU_RASTERIZER=OFF` and run with `PS2X_GS=cpu`
(slow reference renderer).

## Getting the sources

```sh
git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/ben10-alien-force-pc.git
```

`third_party/PS2Recomp` is a submodule: the [PS2Recomp fork](https://github.com/batatinhafcbatatinha-pixel/PS2Recomp),
branch `macos`, with the runtime changes this port needs (OpenGL GS backend, IOP HLE fixes, and for Linux:
SSE4.1 on GCC/Clang, a portable `static_assert` and a 64 MiB game-thread stack on glibc, overridable with
`PS2X_THREAD_STACK_MB`). Without the submodule checked out, `project/CMakeLists.txt` falls back to a shared
`../third_party/PS2Recomp` next to the game folder.

## Build and run

```sh
./build-linux.sh                 # configure once + build into build/linux (JOBS=2 by default)
./run-linux.sh                   # uses game_files/ (SLUS_218.15, MODULES/, DATA.PAK) from your own disc
./run-linux.sh /path/to/disc.iso # or pass the ISO image
```

Generated game code is compiled at `-O0 -ffp-contract=off` like the Windows `/Od` bootstrap
(`-DBEN10_OPTIMIZE_AOT=ON` for `-O1`). `FETCHCONTENT_SOURCE_DIR_RAYLIB` can point at an existing raylib 5.5 checkout
to avoid the download.
