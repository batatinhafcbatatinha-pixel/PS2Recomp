"""Locate duplicate ISO9660 directory data and boot strings across the GT4 ISO."""
import re

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048

PATTERNS = [b"GT4.VOL", b"BOOT2", b"SCUS_973.28", b"CD001", b"IOPRP300"]


def main():
    hits = {p: [] for p in PATTERNS}
    with open(ISO, "rb") as f:
        pos = 0
        chunk = 16 * 1024 * 1024
        prev = b""
        overlap = 32
        while True:
            blob = f.read(chunk)
            if not blob:
                break
            buf = prev + blob
            base = pos - len(prev)
            for p in PATTERNS:
                start = 0
                while True:
                    i = buf.find(p, start)
                    if i < 0:
                        break
                    hits[p].append(base + i)
                    start = i + 1
            prev = buf[-overlap:]
            pos += len(blob)
    for p, hs in hits.items():
        print(p, len(hs), [f"{h}(s{h//SECTOR})" for h in hs[:30]])


if __name__ == "__main__":
    main()
