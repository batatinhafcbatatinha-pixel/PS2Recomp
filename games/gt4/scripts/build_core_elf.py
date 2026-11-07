"""Build a normal ELF32 (MIPS R5900) from the CORE.GT4 executable container.

CORE.GT4 holds a compressed PS2 EE image: a SHA-512 header followed by
(target address, size, data) sections. PS2Recomp and Ghidra need a real ELF, so
this script re-lays those sections out as PT_LOAD segments and section headers.

Reads only D:\\CodexBuilds\\gt4\\game_files\\CORE.GT4; writes the derived ELF to
D:\\CodexBuilds\\gt4\\decompressed\\CORE.ELF (never versioned).
"""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_tools import build_location, game_files  # noqa: E402

CORE = game_files() / "CORE.GT4"
OUT = game_files() / "CORE.ELF"  # next to the disc files: the runtime treats the ELF folder as cdrom0:

EM_MIPS = 8
PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4


def decode(raw: bytes) -> bytes:
    flags, size = struct.unpack_from("<HI", raw, 0)
    stream = zlib.decompressobj(-15)
    data = stream.decompress(raw[6:]) + stream.flush()
    if len(data) != size:
        raise SystemExit(f"decompressed {len(data)} bytes, container says {size}")
    return data


def parse(data: bytes):
    pos = 0
    h1_size = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    h1 = data[pos:pos + h1_size]
    pos += h1_size
    h2_size = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    h2 = data[pos:pos + h2_size]
    pos += h2_size
    count, entry = struct.unpack_from("<ii", data, pos)
    pos += 8
    sections = []
    for _ in range(count):
        target, size = struct.unpack_from("<ii", data, pos)
        pos += 8
        sections.append((target, data[pos:pos + size]))
        pos += size
    if pos != len(data):
        raise SystemExit(f"container has {len(data) - pos} trailing bytes")
    return h1, h2, entry, sections


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def build_elf(entry: int, segments, names) -> bytes:
    """segments: list of (vaddr, bytes, flags); names: parallel section names."""
    phnum = len(segments)
    shnum = len(segments) + 2  # null + segments + shstrtab
    phoff = 52
    data_off = align(phoff + phnum * 32, 0x1000)
    placed = []
    cursor = data_off
    for vaddr, blob, flags in segments:
        cursor = align(cursor, 0x1000)
        placed.append((vaddr, blob, flags, cursor))
        cursor += len(blob)
    shstr = b"\x00.shstrtab\x00" + b"".join(
        b"." + name.encode() + b"\x00" for name in names)
    shstr_off = align(cursor, 4)
    shoff = align(shstr_off + len(shstr), 4)
    size = shoff + shnum * 40

    out = bytearray(size)
    # ELF header
    out[0:16] = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, EM_MIPS, 1, entry, phoff,
                     shoff, 0x20924001, 52, 32, phnum, 40, shnum, shnum - 1)
    # Program headers
    for i, (vaddr, blob, flags, off) in enumerate(placed):
        struct.pack_into("<IIIIIIII", out, phoff + i * 32, PT_LOAD, off, vaddr,
                         vaddr, len(blob), len(blob), flags, 0x1000)
    # Section data
    for vaddr, blob, flags, off in placed:
        out[off:off + len(blob)] = blob
    out[shstr_off:shstr_off + len(shstr)] = shstr
    # Section headers: null, then one per segment, then .shstrtab
    name_off = 1
    for i, (vaddr, blob, flags, off) in enumerate(placed, start=1):
        sh_type = 1  # SHT_PROGBITS
        sh_flags = 0x2 | 0x4 if flags & PF_X else 0x1 | 0x2  # ALLOC + EXEC/ WRITE
        struct.pack_into("<IIIIIIIIII", out, shoff + i * 40, name_off, sh_type,
                         sh_flags, vaddr, off, len(blob), 0, 0, 4, 0)
        name_off += len(names[i - 1]) + 2
    shstr_name = name_off
    struct.pack_into("<IIIIIIIIII", out, shoff + (shnum - 1) * 40, shstr_name, 3,
                     0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    return bytes(out)


def main():
    raw = CORE.read_bytes()
    data = decode(raw)
    h1, h2, entry, sections = parse(data)
    print(f"CORE.GT4 {len(raw)} bytes -> {len(data)} bytes; entry=0x{entry:08x}; "
          f"{len(sections)} container sections")
    for i, (target, blob) in enumerate(sections):
        print(f"  [{i}] 0x{target:08x} size=0x{len(blob):08x} "
              f"sha512={hashlib.sha512(blob).hexdigest()[:32]}...")
    print(f"  container hash1 {h1.hex()[:32]}...")
    print(f"  container hash2 {h2.hex()[:32]}...")
    # Keep only sections that carry file data, sorted by address.
    segments = [(target, blob, PF_R | PF_X if i != 2 else PF_R | PF_W)
                for i, (target, blob) in enumerate(sections) if i in (1, 2)]
    names = ["text", "data"]
    elf = build_elf(entry, segments, names)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(elf)
    print(f"wrote {OUT} ({len(elf)} bytes, sha256={hashlib.sha256(elf).hexdigest()})")
    # Self-check: entry lands in the first segment and looks like MIPS code.
    base = segments[0][0]
    off = entry - base
    words = struct.unpack_from("<4I", segments[0][1], off)
    print("entry words:", " ".join(f"0x{w:08x}" for w in words))


if __name__ == "__main__":
    main()
