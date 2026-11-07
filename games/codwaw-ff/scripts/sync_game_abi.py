"""Keep a frozen copy of the runtime headers for the generated game code.

The ~2.8k game-code objects only depend on the game ABI: R5900Context, the
memory/branch macros and the out-of-line PS2Runtime calls. Compiling them against
this snapshot means edits to VU1/GS/VIF/scheduler headers rebuild only the runtime.
The snapshot is refreshed (forcing a game-code rebuild) when an ABI header changes.

Usage: python scripts/sync_game_abi.py [--force]
"""
import hashlib
import pathlib
import shutil
import sys

root = pathlib.Path(__file__).resolve().parent.parent
runtime = root.parents[1] / "ps2xRuntime"
sources = {
    "runtime-include": runtime / "include",
    "kernel": runtime / "src/lib/Kernel",
    "iop-include": root.parents[1] / "ps2xIOP/include",
}
# Headers whose contents generated code compiles against directly.
abi_headers = [
    "ps2_runtime.h",
    "ps2_runtime_macros.h",
    "ps2_syscalls.h",
    "ps2_stubs.h",
    "ps2_call_list.h",
    "ps2_log.h",
    "runtime/ps2_address.h",
]
destination = root / "build/game-abi"
stamp = destination / "abi.sha256"

digest = hashlib.sha256()
for name in abi_headers:
    digest.update(name.encode())
    digest.update((sources["runtime-include"] / name).read_bytes())
current = digest.hexdigest()

force = "--force" in sys.argv
if not force and stamp.exists() and stamp.read_text().strip() == current:
    print("[abi] game ABI headers unchanged; game code keeps its snapshot")
    sys.exit(0)

if destination.exists():
    shutil.rmtree(destination)
for name, source in sources.items():
    shutil.copytree(source, destination / name, ignore=shutil.ignore_patterns("*.cpp", "*.c", "*.inl.cpp"))
stamp.write_text(current + "\n")
print(f"[abi] snapshot refreshed ({'forced' if force else 'ABI headers changed'}); game code will rebuild")
