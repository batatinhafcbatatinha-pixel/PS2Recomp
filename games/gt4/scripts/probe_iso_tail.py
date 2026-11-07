"""Look at what lies beyond the ISO9660 volume of the GT4 ISO (read-only)."""
import hashlib
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048
TOTAL = 2594960


def main():
    with open(ISO, "rb") as f:
        print("file sectors", TOTAL)
        for name, sec in (("after volume", 1317056), ("mid tail", 1400000), ("last", TOTAL - 1)):
            f.seek(sec * SECTOR)
            data = f.read(256)
            print(name, sec, data[:64].hex(), repr(data[:32]))
        # scan for CD001 volume descriptor signatures
        hits = []
        f.seek(0)
        chunk = 4 * 1024 * 1024
        pos = 0
        prev = b""
        while True:
            blob = f.read(chunk)
            if not blob:
                break
            buf = prev + blob
            start = 0
            while True:
                i = buf.find(b"CD001", start)
                if i < 0:
                    break
                hits.append(pos - len(prev) + i)
                start = i + 1
            prev = buf[-8:]
            pos += len(blob)
        print("CD001 hits", len(hits), hits[:40])
        for off in hits[:40]:
            print("  off", off, "sector", off // SECTOR, "mod", off % SECTOR, "type", off - 1 if off else None)
        # zero-area scan: count non-zero 64KiB blocks in layer 2
        f.seek(1317056 * SECTOR)
        nonzero = 0
        zero = 0
        block = 64 * 1024
        while True:
            blob = f.read(block)
            if not blob:
                break
            if blob.count(0) == len(blob):
                zero += 1
            else:
                nonzero += 1
        print("layer2 64k blocks nonzero", nonzero, "zero", zero)


if __name__ == "__main__":
    main()
