"""Disassemble small windows of the GT4 boot ELF (no external tool)."""
import struct

ELF = r"D:\CodexBuilds\gt4\game_files\SCUS_973.28"
TEXT_VADDR = 0x01000000
TEXT_OFF = 0x1000


def words(vaddr, count):
    with open(ELF, "rb") as f:
        f.seek(TEXT_OFF + (vaddr - TEXT_VADDR))
        data = f.read(count * 4)
    return struct.unpack(f"<{count}I", data)


def show(vaddr, count=12):
    print(f"== 0x{vaddr:08x}")
    for i, w in enumerate(words(vaddr, count)):
        print(f"  0x{vaddr + i*4:08x}: {w:08x}")


if __name__ == "__main__":
    for v in (0x0101F020, 0x0101F100, 0x0101F220, 0x0101F880, 0x01000008, 0x0101F6A0):
        show(v, 10)
