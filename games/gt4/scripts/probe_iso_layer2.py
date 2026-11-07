"""Probe the second ISO9660 volume (DVD9 layer 2) of the GT4 ISO."""
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048
BASE = 1317056


def main():
    with open(ISO, "rb") as f:
        f.seek(BASE * SECTOR)
        pvd = f.read(SECTOR)
        print("type", pvd[0], "id", pvd[1:6], "size", struct.unpack_from("<I", pvd, 80)[0])
        f.seek((BASE + 1) * SECTOR)
        print("vol desc 2 type", f.read(1))
        rec = pvd[156:156 + pvd[156]]
        extent = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        print("root extent", extent, "size", size)
        f.seek(extent * SECTOR)
        data = f.read(size)
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
            if name in (b"\x00", b"\x01"):
                continue
            print(f"{'DIR ' if flags & 2 else 'FILE'} ext={ext:8d} size={sz:12d} name={name!r}")


if __name__ == "__main__":
    main()
