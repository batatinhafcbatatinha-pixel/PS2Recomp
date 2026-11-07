"""Characterize CORE.ELF call graph and launcher-range references (read-only)."""
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_core_elf import CORE, decode, parse  # noqa: E402


def main():
    data = decode(CORE.read_bytes())
    _h1, _h2, entry, sections = parse(data)
    code = sections[1][1]
    base = sections[1][0]
    code_end = base + len(code)
    words = struct.unpack_from(f"<{len(code) // 4}I", code, 0)
    direct = collections.Counter()
    for i, raw in enumerate(words):
        if (raw >> 26) not in (2, 3):
            continue
        pc = base + i * 4
        target = ((pc + 4) & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)
        if base <= target < code_end:
            direct["internal"] += 1
        elif 0x01000000 <= target < 0x01080000:
            direct["launcher"] += 1
        else:
            direct[f"other:0x{target >> 24:02x}"] += 1
    print("direct j/jal:", dict(direct))
    jalr = sum(1 for raw in words if raw == 0x0000F809 or (raw & 0x3F) == 9 and (raw >> 26) == 0)
    print("jalr count:", jalr)

    # every 32-bit word anywhere in the image pointing into the launcher range
    hits = collections.Counter()
    for name, (addr, blob) in (("text", sections[1]), ("data", sections[2])):
        for i in range(0, len(blob) - 3, 4):
            value = struct.unpack_from("<I", blob, i)[0]
            if 0x01000000 <= value < 0x01080000:
                hits[(name, addr + i, value)] += 1
    print(f"words pointing into 0x01000000-0x01080000: {len(hits)}")
    for (name, at, value), _ in list(hits.items())[:20]:
        print(f"  {name} 0x{at:08x} -> 0x{value:08x}")
    # pointers into 0x70000000 (VU1) as a sanity check of the pointer scan
    vu = sum(1 for name, (addr, blob) in (("text", sections[1]), ("data", sections[2]))
             for i in range(0, len(blob) - 3, 4)
             if 0x70000000 <= struct.unpack_from("<I", blob, i)[0] < 0x70100000)
    print("words pointing into 0x70000000-0x70100000:", vu)


if __name__ == "__main__":
    main()
