"""Probe GT4L1.VOL: is it a second RoFS volume or a raw data layer? (read-only)"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_tools import game_files  # noqa: E402

PAGE = 0x800
MAGIC_BE = 0xAD90B9AC
VERSION = 0x00030001


def main():
    path = game_files() / "GT4L1.VOL"
    print(f"{path} {path.stat().st_size} bytes")
    with path.open("rb") as f:
        head = f.read(0x40)
        print("head:", head[:16].hex(" "))
        found = []
        for page in range(min(path.stat().st_size // PAGE, 10000)):
            f.seek(page * PAGE)
            block = f.read(8)
            if len(block) < 8:
                break
            magic = struct.unpack_from(">I", block, 0)[0]
            version = struct.unpack_from("<I", block, 4)[0]
            if magic == MAGIC_BE and version == VERSION:
                found.append(page)
        print("RoFS ToC pages:", [hex(p) for p in found])
        for page in found[:1]:
            f.seek(page * PAGE)
            block = f.read(0x40)
            comp, pages = struct.unpack_from("<iI", block, 8)
            plen, entries = struct.unpack_from("<HH", block, 0x10)
            print(f"  page 0x{page:x}: toc_len=0x{comp:x} pages={pages} page_len=0x{plen:x} entries={entries}")


if __name__ == "__main__":
    main()
