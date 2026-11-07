"""Printable strings of the GT4 boot ELF with file offsets."""
from pathlib import Path
import re

ELF = Path(r"D:\CodexBuilds\gt4\game_files\SCUS_973.28")


def main():
    data = ELF.read_bytes()
    for m in re.finditer(rb"[\x20-\x7e]{5,}", data):
        s = m.group().decode("ascii")
        print(f"0x{m.start():06x}  {s}")


if __name__ == "__main__":
    main()
