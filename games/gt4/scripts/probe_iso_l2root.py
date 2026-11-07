"""Resolve the real root of the GT4 layer 2 filesystem."""
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048
PVD2 = 1317056


def dump(f, sector, size, label):
    f.seek(sector * SECTOR)
    data = f.read(size)
    print(f"== {label} sector={sector} size={size}")
    print("   first bytes", data[:24].hex())
    off = 0
    ok = True
    while off < len(data):
        length = data[off]
        if not length:
            off = (off // SECTOR + 1) * SECTOR
            continue
        if length < 34 or off + length > len(data):
            ok = False
            break
        rec = data[off:off + length]
        ext = struct.unpack_from("<I", rec, 2)[0]
        sz = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        n = rec[32]
        name = rec[33:33 + n]
        off += length
        print(f"   {'DIR ' if flags & 2 else 'FILE'} ext={ext} size={sz} name={name!r}")
    if not ok:
        print("   (not a directory)")
    return ok


def main():
    with open(ISO, "rb") as f:
        for cand in (1317301, 1317300, 1317296, 1317317):
            dump(f, cand, 200, f"candidate {cand}")
        # scan sectors near layer break for a self directory record
        for s in range(1317000, 1317100):
            f.seek(s * SECTOR)
            d = f.read(64)
            if d[0] in (34, 0x22) and d[1] == 0 and d[32] == 1 and d[33] == 0:
                ext = struct.unpack_from("<I", d, 2)[0]
                size = struct.unpack_from("<I", d, 10)[0]
                print("self-record sector", s, "ext", ext, "size", size)


if __name__ == "__main__":
    main()
