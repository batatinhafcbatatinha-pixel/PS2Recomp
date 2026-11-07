"""Validate the GT4 volume ToC page decode (read-only)."""
from pathlib import Path
import struct
import zlib

VOL = Path(r"D:\CodexBuilds\gt4\game_files\GT4.VOL")
TOC = 0x2159 * 0x800
KEY = 0x14AC327A
MASK = 0xFFFFFFFF


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def main():
    with VOL.open("rb") as f:
        f.seek(TOC)
        head = f.read(0x400)
        pages = u32(head, 0x0C)
        vals = [u32(head, 0x40 + 4 * i) for i in range(pages + 1)]
        offs = [v ^ (((i * KEY) + KEY) & MASK) for i, v in enumerate(vals)]
        for p in range(3):
            f.seek(TOC + offs[p])
            raw = f.read(offs[p + 1] - offs[p])
            dec = bytes(b ^ 0x55 for b in raw)
            for name, data in (("raw", raw), ("xor55", dec)):
                for wbits in (15, -15, 31):
                    try:
                        out = zlib.decompress(data, wbits)
                        print(f"page {p} {name} wbits={wbits}: ok {len(out)} bytes head={out[:32].hex(' ')}")
                        break
                    except Exception as exc:
                        pass
                else:
                    print(f"page {p} {name}: inflate failed; head={data[:16].hex(' ')}")
        # tail of the compressed region
        f.seek(TOC + offs[-1])
        tail = f.read(0x20)
        print("tail@last offset:", tail.hex(" "))


if __name__ == "__main__":
    main()
