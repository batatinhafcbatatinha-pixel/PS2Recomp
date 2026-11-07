"""Parse the CORE.GT4 executable container (read-only).

Layout (public GT modding documentation):
  ushort hash1_size, byte[hash1_size] hash1
  ushort hash2_size, byte[hash2_size] hash2
  int section_count, int entrypoint
  section_count * { int target_offset, int size, byte[size] data }
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_tools import build_location  # noqa: E402

SRC = build_location() / "decompressed/CORE.ELF"


def main():
    data = SRC.read_bytes()
    pos = 0
    h1 = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    print(f"hash1 size={h1} sha512={data[pos:pos + h1].hex()}")
    pos += h1
    h2 = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    print(f"hash2 size={h2} sha512={data[pos:pos + h2].hex()}")
    pos += h2
    count, entry = struct.unpack_from("<ii", data, pos)
    pos += 8
    print(f"sections={count} entry=0x{entry:08x} header_end=0x{pos:x}")
    total = 0
    for i in range(count):
        target, size = struct.unpack_from("<ii", data, pos)
        pos += 8
        total += size
        print(f"  [{i:2d}] target=0x{target:08x} size=0x{size:08x} ({size}) data@0x{pos:x}")
        pos += size
    print(f"sum of sections={total} file={len(data)} end=0x{pos:x} trailing={len(data) - pos}")


if __name__ == "__main__":
    main()
