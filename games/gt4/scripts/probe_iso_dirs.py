"""Compare layer 1 and layer 2 directory trees of the GT4 ISO."""
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048
BASE = 1317056


def dump(f, label, sector, size, base=0):
    f.seek(sector * SECTOR)
    data = f.read(size)
    print(f"== {label} sector={sector} size={size} first={data[:16].hex()}")
    off = 0
    while off < len(data):
        length = data[off]
        if not length:
            off = (off // SECTOR + 1) * SECTOR
            continue
        rec = data[off:off + length]
        ext = struct.unpack_from("<I", rec, 2)[0]
        sz = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        n = rec[32]
        name = rec[33:33 + n]
        off += length
        print(f"  {'DIR ' if flags & 2 else 'FILE'} ext={ext} abs={base + ext} size={sz} name={name!r}")


def main():
    with open(ISO, "rb") as f:
        dump(f, "L1 root (from L1 PVD)", 261, 544)
        dump(f, "L1 root (as L2 PVD says)", 261, 154)
        dump(f, "L2 root", BASE + 261, 154, BASE)


if __name__ == "__main__":
    main()
