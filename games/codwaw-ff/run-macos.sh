#!/usr/bin/env bash
# Run the native macOS build. Needs game_files/ (SLUS_217.46 and the disc folders) from your own disc.
#   ./run-macos.sh                     windowed, uses game_files/ as the disc
#   ./run-macos.sh /path/to/game.iso   use the ISO image instead (recommended: raw sector reads, FMV)
#   PS2X_WINDOW_MODE=fullscreen ./run-macos.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
exe="${BUILD_DIR:-$here/build/macos}/ps2recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$exe" ] || { echo "Build first: ./build-macos.sh" >&2; exit 1; }
export PS2X_WINDOW_MODE="${PS2X_WINDOW_MODE:-windowed}"
# macOS has no OpenGL 4.6, so the GS always runs on the CPU here.
export PS2X_GS="${PS2X_GS:-cpu}"
cd "$here"
exec "$exe" "$here/game_files/SLUS_217.46" "$@"
