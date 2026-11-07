"""Dump candidate 16-byte records of the GT4.VOL index."""
from pathlib import Path
import struct

ROOT = Path(r"D:\CodexBuilds\gt4\game_files")
VOL = ROOT / "GT4.VOL"


def dump_entries(f, start, count, label):
    print(f"== {label} start=0x{start:x} count={count}")
    f.seek(start)
    data = f.read(16 * count)
    for i in range(count):
        rec = data[i * 16:(i + 1) * 16]
        a, b, c, d = struct.unpack("<IIII", rec)
        print(f"  {start + i*16:08x}: {a:10d} 0x{a:08x} | {b:10d} 0x{b:08x} | {c:10d} 0x{c:08x} | {d:10d}")


def main():
    with VOL.open("rb") as f:
        f.seek(0)
        head = f.read(0x100)
        print("header words:", [f"0x{v:08x}" for v in struct.unpack("<40I", head[:160])])
        for start, count, label in ((0x0D34, 20, "node 0x7f children (0xd34..)"),
                                    (0x504, 4, "node 0x63 children"),
                                    (0x540, 2, "node 0x6d child"),
                                    (0x664, 3, "node 0x71 child"),
                                    (0xD04, 4, "node 0x75 children")):
            dump_entries(f, start, count, label)


if __name__ == "__main__":
    main()
