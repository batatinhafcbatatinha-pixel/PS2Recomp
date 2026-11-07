# Linux (native build)

The same sources build natively on Linux x86-64 with GCC or Clang: no Wine, no compatibility layer. The window
and input are raylib/GLFW (X11), the GS rasterizer is the runtime's OpenGL 4.6 backend, video decoding is the
system FFmpeg and audio is miniaudio (ALSA/PulseAudio).

Status (2026-10-30): builds and links with GCC 13 (generated code at `-O0 -ffp-contract=off`, like the Windows `/Od` bootstrap); not yet run on Linux. `-DBOKU2_ALLOW_CPU_GS=ON` allows the CPU reference rasterizer (`PS2X_GS=cpu`) on GPUs without fragment interlock.

## Requirements

Debian/Ubuntu packages:

```sh
sudo apt install build-essential cmake ninja-build pkg-config git python3 \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev libegl-dev libasound2-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

A GPU driver with OpenGL 4.6 and `GL_ARB_fragment_shader_interlock` (current NVIDIA and Intel drivers, and Mesa
drivers that expose it).

## Getting the sources

```sh
git clone --recurse-submodules https://github.com/batatinhafcbatatinha-pixel/boku-no-natsuyasumi-2-pc.git
```

`third_party/PS2Recomp` is a submodule: the [PS2Recomp fork](https://github.com/batatinhafcbatatinha-pixel/PS2Recomp),
branch `macos`, with the runtime changes this port needs (Linux included: SSE4.1 on GCC/Clang, a portable
`static_assert`, a 64 MiB game-thread stack on glibc, overridable with `PS2X_THREAD_STACK_MB`). Without the
submodule checked out, `project/CMakeLists.txt` falls back to a shared `../third_party/PS2Recomp` next to the game
folder.

## Build and run

```sh
./build-linux.sh      # configure once + build into build/linux (JOBS=2 by default)
./run-linux.sh        # boots game_files/SCPS_150.26 from your own disc
```

`FETCHCONTENT_SOURCE_DIR_RAYLIB` can point at an existing raylib 5.5 checkout to avoid the download.
