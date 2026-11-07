"""Decode CORE.GT4 into a plain ELF (read-only on the source).

CORE.GT4 layout for retail GT4 (no Salsa20 layer):
  0x00 ushort boot load flags
  0x02 uint   decompressed size
  0x06        compressed data (raw deflate)
Writes the decompressed executable to D:\\CodexBuilds\\gt4\\decompressed\\.
"""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_tools import build_location, game_files  # noqa: E402

CORE = game_files() / "CORE.GT4"
OUT_DIR = build_location() / "decompressed"


def main():
    raw = CORE.read_bytes()
    flags, size = struct.unpack_from("<HI", raw, 0)
    print(f"CORE.GT4 {len(raw)} bytes; boot flags=0x{flags:04x} decompressed size={size} (0x{size:x})")
    body = raw[6:]
    for wbits, name in ((15, "zlib"), (-15, "raw deflate")):
        stream = zlib.decompressobj(wbits)
        try:
            data = stream.decompress(body) + stream.flush()
        except zlib.error as exc:
            print(f"{name}: {exc}")
            continue
        print(f"{name}: {len(data)} bytes, expected {size}, eof={stream.eof}, "
              f"unused={len(stream.unused_data)} head={data[:16].hex(' ')}")
        if len(data) == size:
            OUT_DIR.mkdir(parents=True, exist_ok=True)
            target = OUT_DIR / "CORE.ELF"
            target.write_bytes(data)
            print(f"wrote {target}")
            return
    raise SystemExit("CORE.GT4 did not decode as documented")


if __name__ == "__main__":
    main()
