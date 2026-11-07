"""Dump the GT4 volume ToC area after the page-offset table (read-only)."""
from pathlib import Path

VOL = Path(r"D:\CodexBuilds\gt4\game_files\GT4.VOL")
TOC = 0x2159 * 0x800


def dump(f, off, n=0x100):
    f.seek(off)
    d = f.read(n)
    print(f"== 0x{off:x}")
    for i in range(0, len(d), 16):
        c = d[i:i + 16]
        text = "".join(chr(x) if 32 <= x < 127 else "." for x in c)
        print(f"{off + i:08x}  {c.hex(' ')}  {text}")


def main():
    with VOL.open("rb") as f:
        dump(f, TOC + 0x300, 0x200)
        dump(f, TOC + 0x680, 0x80)
        dump(f, TOC + 0x9a0, 0x80)
        dump(f, TOC + 0x10, 0x30)


if __name__ == "__main__":
    main()
