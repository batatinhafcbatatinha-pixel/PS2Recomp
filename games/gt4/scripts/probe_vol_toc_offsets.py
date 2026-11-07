"""Probe the real GT4 volume ToC page-offset table (read-only)."""
from pathlib import Path
import struct

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
    comp_len = u32(head, 0x08)
    pages = u32(head, 0x0C)
    page_len = struct.unpack_from("<H", head, 0x10)[0]
    toc_pages = struct.unpack_from("<H", head, 0x12)[0]
    print(f"compressed_toc_len=0x{comp_len:x} pages={pages} page_len=0x{page_len:x} toc_pages={toc_pages}")
    vals = [u32(head, 0x40 + 4 * i) for i in range(pages + 1)]
    a = [v ^ (((i * KEY) + KEY) & MASK) for i, v in enumerate(vals)]
    b = [(v ^ ((i * KEY) & MASK)) & MASK for i, v in enumerate(vals)]
    c = [(v - ((i * KEY) + KEY)) & MASK for i, v in enumerate(vals)]
    for name, seq in (("xor(i*K+K)", a), ("xor(i*K)", b), ("sub(i*K+K)", c)):
        mono = all(seq[i] <= seq[i + 1] for i in range(len(seq) - 1))
        print(f"{name}: first={[hex(x) for x in seq[:6]]} last={hex(seq[-1])} monotonic={mono}")
    print("diffs(a) first 12:", [a[i + 1] - a[i] for i in range(12)])


if __name__ == "__main__":
    main()
