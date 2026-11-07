#!/usr/bin/env bash
# Native macOS build of CoD: World at War Final Fronts (Apple Silicon or Intel, no Wine). Same sources as Windows and Linux.
#   ./build-macos.sh                 configure (once) + build into build/macos
#   JOBS=4 ./build-macos.sh          parallel jobs (default: 2, the generated code is memory hungry)
# Tools: Xcode Command Line Tools, then: brew install cmake ninja pkg-config ffmpeg
# Generated game code must already exist in project/runner and project/runner-chunks: see MACOS.md.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${BUILD_DIR:-$here/build/macos}"
jobs="${JOBS:-2}"
if [ ! -d "$here/project/runner-chunks" ]; then
  echo "project/runner-chunks is missing: generate the game code from your own disc first (see MACOS.md)." >&2
  exit 1
fi
if [ ! -f "$build/build.ninja" ]; then
  launcher=()
  if command -v ccache >/dev/null; then
    launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
  fi
  cmake -S "$here/project" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release ${launcher[@]+"${launcher[@]}"} "$@"
fi
cmake --build "$build" --target ps2EntryRunner --parallel "$jobs"
echo "Built: $build/ps2recomp/ps2xRuntime/ps2EntryRunner"
