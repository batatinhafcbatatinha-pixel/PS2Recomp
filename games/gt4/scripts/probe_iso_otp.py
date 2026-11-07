"""Test DVD9 opposite-track-path (reversed layer 2) addressing for the GT4 ISO."""
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048
BASE = 1317056
TOTAL = 2594960


def read_sector(f, sector, n=320):
    f.seek(sector * SECTOR)
    return f.read(n)


def main():
    with open(ISO, "rb") as f:
        for cand in (TOTAL - 1 - 261, TOTAL - 1 - 429, TOTAL - 1 - 105879, TOTAL - 1, TOTAL - 16):
            data = read_sector(f, cand, 96)
            print(cand, data.hex(), repr(data[:32]))


if __name__ == "__main__":
    main()
