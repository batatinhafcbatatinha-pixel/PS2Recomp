#!/usr/bin/env bash
# Native Linux build of CoD: World at War Final Fronts (no Wine). Same sources as the Windows build.
#   ./build-linux.sh                 configure (once) + build into build/linux
#   JOBS=4 ./build-linux.sh          parallel jobs (default: 2, the generated code is memory hungry)
#   ./build-linux.sh -DPS2X_ENABLE_CODWAW_SUPPLEMENTS=OFF   extra CMake options on first configure
# Packages (Debian/Ubuntu): build-essential cmake ninja-build pkg-config libx11-dev libxrandr-dev
#   libxinerama-dev libxcursor-dev libxi-dev libgl-dev libegl-dev libasound2-dev
#   libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${BUILD_DIR:-$here/build/linux}"
jobs="${JOBS:-2}"
if [ ! -f "$build/build.ninja" ]; then
  launcher=()
  command -v ccache >/dev/null && launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
  cmake -S "$here/project" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${launcher[@]}" "$@"
fi
cmake --build "$build" --target ps2EntryRunner --parallel "$jobs"
echo "Built: $build/ps2recomp/ps2xRuntime/ps2EntryRunner"
