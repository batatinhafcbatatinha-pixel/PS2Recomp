"""Probe the GT4 ISO volume descriptor and root directory (read-only)."""
import struct
import sys

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048


def main():
    with open(ISO, "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        print("type", pvd[0], "id", pvd[1:6], "version", pvd[6])
        print("logical block size", struct.unpack_from("<H", pvd, 128)[0])
        print("volume space size", struct.unpack_from("<I", pvd, 80)[0], "sectors =",
              struct.unpack_from("<I", pvd, 80)[0] * SECTOR, "bytes")
        print("root record len", pvd[156])
        rec = pvd[156:156 + pvd[156]]
        extent = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        print("root extent sector", extent, "size", size)
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
    sys.exit(main())
