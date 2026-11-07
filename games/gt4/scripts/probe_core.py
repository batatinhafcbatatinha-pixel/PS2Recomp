"""Characterise CORE.GT4 and the GT4.VOL header."""
from collections import Counter
from pathlib import Path
import math
import struct

ROOT = Path(r"D:\CodexBuilds\gt4\game_files")


def entropy(blob):
    c = Counter(blob)
    n = len(blob)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def main():
    core = ROOT / "CORE.GT4"
    data = core.read_bytes()
    print("CORE.GT4 size", len(data))
    for i in range(0, 32):
        start = i * (len(data) // 32)
        print(f"  block {i:2} @0x{start:08x} entropy={entropy(data[start:start + len(data)//32]):.3f}")
    print("head", data[:32].hex())
    print("tail", data[-32:].hex())
    counts = Counter(data[:1 << 20])
    print("top bytes", counts.most_common(8))
    zeros = data.count(0)
    print("zero bytes", zeros, f"{100*zeros/len(data):.2f}%")
    for magic, name in ((b"\x1f\x8b", "gzip"), (b"\x78\x9c", "zlib9c"), (b"\x78\x01", "zlib01"), (b"\x89PNG", "png"), (b"RESET", "romdir"), (b"\x7fELF", "elf")):
        print(f"  {name}: {data.find(magic)}")

    vol = ROOT / "GT4.VOL"
    with vol.open("rb") as f:
        head = f.read(4096)
    print("\nGT4.VOL first 256 bytes")
    for i in range(0, 256, 16):
        print(f"  {i:04x}  {head[i:i+16].hex(' ')}")


if __name__ == "__main__":
    main()
