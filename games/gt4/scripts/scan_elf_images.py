"""Scan GT4 files for embedded ELF images and report candidate offsets."""
import argparse
from pathlib import Path

DEFAULT_ROOT = Path(r"D:\CodexBuilds\gt4\game_files")


def scan(path, needle=b"\x7fELF\x01\x01\x01", limit=200):
    hits = []
    with path.open("rb") as f:
        pos = 0
        chunk = 32 * 1024 * 1024
        prev = b""
        while True:
            blob = f.read(chunk)
            if not blob:
                break
            buf = prev + blob
            base = pos - len(prev)
            i = 0
            while True:
                i = buf.find(needle, i)
                if i < 0:
                    break
                hits.append(base + i)
                i += 1
                if len(hits) >= limit:
                    break
            if len(hits) >= limit:
                break
            prev = buf[-16:]
            pos += len(blob)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="+")
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = ap.parse_args()
    for name in args.names:
        path = args.root / name
        if not path.is_file():
            print(name, "missing")
            continue
        hits = scan(path)
        print(f"{name}: {len(hits)} ELF-like signatures")
        for h in hits[:40]:
            print(f"   0x{h:08x} ({h})")


if __name__ == "__main__":
    main()
