"""Inspect the head of GT4.VOL / GT4L1.VOL (read-only)."""
from pathlib import Path
import struct

ROOT = Path(r"D:\CodexBuilds\gt4\game_files")


def dump(path, n=512):
    p = ROOT / path
    if not p.is_file():
        print(path, "missing")
        return
    with p.open("rb") as f:
        head = f.read(n)
    print("==", path, p.stat().st_size, "bytes")
    for i in range(0, len(head), 32):
        chunk = head[i:i + 32]
        print(f"{i:06x}  {chunk.hex(' ')}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)}")


def main():
    dump("GT4.VOL", 512)
    dump("CORE.GT4", 256)
    dump("IOPRP300.IMG", 256)


if __name__ == "__main__":
    main()
