"""Inspect the decompressed CORE.GT4 payload (read-only)."""
import collections
import math
from pathlib import Path

P = Path(r"D:\CodexBuilds\gt4\decompressed\CORE.ELF")


def main():
    data = P.read_bytes()
    print(f"{P} {len(data)} bytes")
    counts = collections.Counter(data)
    entropy = -sum((c / len(data)) * math.log2(c / len(data)) for c in counts.values())
    print(f"entropy={entropy:.3f} bits/byte")
    for magic, label in ((b"\x7fELF", "ELF"), (b"\x1f\x8b", "gzip"), (b"\xc5\xee\xf7\xff", "PS2Zip"),
                         (b"sce", "sce"), (b"SCEI", "SCEI"), (b"RoFS", "RoFS")):
        first = data.find(magic)
        print(f"{label}: first at {first if first >= 0 else 'absent'} count={data.count(magic)}")
    text = "".join(chr(b) if 32 <= b < 127 else "." for b in data[:0x200])
    print(text)
    print("head:", data[:64].hex(" "))
    print("tail:", data[-64:].hex(" "))


if __name__ == "__main__":
    main()
