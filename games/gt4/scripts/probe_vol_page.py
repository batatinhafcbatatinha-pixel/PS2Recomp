"""Probe one GT4 volume ToC page decode (read-only)."""
from pathlib import Path
import struct
import zlib

VOL = Path(r"D:\CodexBuilds\gt4\game_files\GT4.VOL")
BASE = 0x2159 * 0x800
KEY = 0x14AC327A


def main():
    with VOL.open("rb") as f:
        f.seek(BASE + 0x40)
        raw = f.read(408 * 4)
        vals = struct.unpack_from("<408I", raw, 0)
        offs = [v ^ ((KEY * (i + 1)) & 0xFFFFFFFF) for i, v in enumerate(vals)]
        print("first offsets:", [hex(x) for x in offs[:5]], "last:", hex(offs[-1]))
        for p in range(3):
            start, end = BASE + offs[p], BASE + offs[p + 1]
            f.seek(start)
            data = f.read(end - start)
            print(f"page {p}: 0x{start:x}..0x{end:x} len={len(data)} head={data[:12].hex(' ')} tail={data[-8:].hex(' ')}")
            obj = zlib.decompressobj(-15)
            out = obj.decompress(data) + obj.flush()
            print(f"   inflated={len(out)} eof={obj.eof} unused={len(obj.unused_data)}")
            dec = bytes(b ^ 0x55 for b in out)
            flag, count, nxt, prev = struct.unpack_from("<hhII", dec, 0)
            print(f"   page flag={flag} count={count} next={nxt} prev={prev} head={dec[:24].hex(' ')}")


if __name__ == "__main__":
    main()
