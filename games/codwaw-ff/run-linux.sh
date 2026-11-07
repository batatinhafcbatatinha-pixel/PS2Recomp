#!/usr/bin/env bash
# Run the native Linux build. Needs game_files/ (SLUS_217.46 and the disc folders) from your own disc.
#   ./run-linux.sh                     windowed, uses game_files/ as the disc
#   ./run-linux.sh /path/to/game.iso   use the ISO image instead (recommended: raw sector reads, FMV)
#   PS2X_WINDOW_MODE=fullscreen ./run-linux.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
exe="${BUILD_DIR:-$here/build/linux}/ps2recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$exe" ] || { echo "Build first: ./build-linux.sh" >&2; exit 1; }
export PS2X_WINDOW_MODE="${PS2X_WINDOW_MODE:-windowed}"
cd "$here"
exec "$exe" "$here/game_files/SLUS_217.46" "$@"
