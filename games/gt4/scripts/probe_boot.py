"""Read SYSTEM.CNF and ELF header facts straight from the GT4 ISO (read-only)."""
import hashlib
import struct

ISO = r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso"
SECTOR = 2048


def read(f, sector, size):
    f.seek(sector * SECTOR)
    return f.read(size)


def main():
    with open(ISO, "rb") as f:
        print("SYSTEM.CNF:", repr(read(f, 429, 57)))
        elf_head = read(f, 430, 64)
        print("ELF ident", elf_head[:16].hex())
        print("e_type", struct.unpack_from("<H", elf_head, 16)[0],
              "e_machine", struct.unpack_from("<H", elf_head, 18)[0],
              "e_version", struct.unpack_from("<I", elf_head, 20)[0],
              "e_entry", hex(struct.unpack_from("<I", elf_head, 24)[0]),
              "e_phoff", struct.unpack_from("<I", elf_head, 28)[0],
              "e_shoff", struct.unpack_from("<I", elf_head, 32)[0],
              "e_flags", hex(struct.unpack_from("<I", elf_head, 36)[0]),
              "e_ehsize", struct.unpack_from("<H", elf_head, 40)[0],
              "e_phentsize", struct.unpack_from("<H", elf_head, 42)[0],
              "e_phnum", struct.unpack_from("<H", elf_head, 44)[0],
              "e_shentsize", struct.unpack_from("<H", elf_head, 46)[0],
              "e_shnum", struct.unpack_from("<H", elf_head, 48)[0])
        size = 273020
        h = hashlib.sha256()
        left = size
        while left:
            blob = f.read(min(left, 1 << 20))
            if not blob:
                raise SystemExit("truncated ELF")
            h.update(blob)
            left -= len(blob)
        print("SCUS_973.28 sha256", h.hexdigest(), "bytes", size)
        print("IOPRP300.IMG head", read(f, 1551, 32).hex())
        print("CORE.GT4 head", read(f, 564, 32).hex())


if __name__ == "__main__":
    main()
