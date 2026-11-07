"""Hexdump the second volume descriptor of the GT4 ISO and inspect candidate roots."""
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048
BASE = 1317056


def show(f, sector, n=320, label=""):
    f.seek(sector * SECTOR)
    data = f.read(n)
    print(label, sector, data.hex())
    return data


def main():
    with open(ISO, "rb") as f:
        pvd = show(f, BASE, 340, "L2 PVD")
        print("space size", struct.unpack_from("<I", pvd, 80)[0])
        print("root rec len", pvd[156])
        rec = pvd[156:156 + pvd[156]]
        print("rec hex", rec.hex())
        print("extent", struct.unpack_from("<I", rec, 2)[0], "size", struct.unpack_from("<I", rec, 10)[0], "flags", rec[25], "namelen", rec[32])
        show(f, BASE + 261, 96, "at L2+261")
        show(f, 261, 96, "at 261")
        # scan layer 2 region start for plausible directory record (34-byte self record)
        f.seek(BASE * SECTOR)
        window = f.read(64 * 1024 * 4)
        for i in range(0, len(window) - 34):
            if window[i] == 0x22 and window[i + 1] == 0 and window[i + 32] == 1 and window[i + 33] == 0:
                ext = struct.unpack_from("<I", window, i + 2)[0]
                size = struct.unpack_from("<I", window, i + 10)[0]
                if 0 < ext < 2594960 and size in (34, 154, 544, 2048):
                    print("candidate self-record at", i, "abs sector", BASE + i // SECTOR, "offset", i % SECTOR, "ext", ext, "size", size)


if __name__ == "__main__":
    main()
