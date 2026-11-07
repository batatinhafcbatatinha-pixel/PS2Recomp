"""Minimal ELF32 little-endian reader for PS2 executables: segments, sections, words, code and data ranges."""
from dataclasses import dataclass, field
from pathlib import Path
import struct

SHF_EXECINSTR = 4
PF_X = 1


@dataclass
class Elf:
    path: Path
    data: bytes
    entry: int
    segments: list = field(default_factory=list)   # (vaddr, offset, filesz, memsz, flags) of PT_LOAD
    sections: list = field(default_factory=list)   # (name, addr, size, flags)

    @classmethod
    def load(cls, path):
        path = Path(path)
        d = path.read_bytes()
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
            raise ValueError(f'{path}: not a 32-bit little-endian ELF')
        entry, phoff, shoff = struct.unpack_from('<III', d, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x2A)
        elf = cls(path, d, entry)
        for i in range(phnum):
            p_type, p_off, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from('<8I', d, phoff + i * phentsize)
            if p_type == 1:
                elf.segments.append((p_vaddr, p_off, p_filesz, p_memsz, p_flags))
        if shoff and shnum and shstrndx < shnum and shoff + shnum * shentsize <= len(d):
            raw = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
            strtab_off = raw[shstrndx][4]
            for s in raw:
                end = d.find(b'\0', strtab_off + s[0])
                name = d[strtab_off + s[0]:end].decode('ascii', 'replace')
                if s[3] and s[5]:
                    elf.sections.append((name, s[3], s[5], s[2]))
        return elf

    def word(self, addr):
        for va, off, filesz, _, _ in self.segments:
            if va <= addr < va + filesz - 3:
                return int.from_bytes(self.data[off + addr - va:off + addr - va + 4], 'little')
        return 0

    def bytes_at(self, addr, size):
        for va, off, filesz, _, _ in self.segments:
            if va <= addr and addr + size <= va + filesz:
                return self.data[off + addr - va:off + addr - va + size]
        return None

    def code_ranges(self):
        """Executable EE code: exec sections except VU microcode (.vutext/.vudata), else exec segments."""
        ranges = [(a, a + s) for n, a, s, f in self.sections if f & SHF_EXECINSTR and not n.lower().startswith('.vu')]
        if not ranges:
            ranges = [(va, va + filesz) for va, _, filesz, _, flags in self.segments if flags & PF_X]
        return merge(ranges)

    def image_range(self):
        lo = min(va for va, *_ in self.segments)
        hi = max(va + memsz for va, _, _, memsz, _ in self.segments)
        return lo, hi

    def data_ranges(self):
        """Initialised data that may hold code pointers (vtables, callbacks)."""
        code = self.code_ranges()
        out = []
        for va, _, filesz, _, _ in self.segments:
            out += subtract((va, va + filesz), code)
        return merge(out)


def merge(ranges):
    out = []
    for a, b in sorted(ranges):
        if out and a <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


def subtract(r, holes):
    parts = [r]
    for ha, hb in holes:
        nxt = []
        for a, b in parts:
            if hb <= a or ha >= b:
                nxt.append((a, b))
                continue
            if a < ha:
                nxt.append((a, ha))
            if hb < b:
                nxt.append((hb, b))
        parts = nxt
    return parts
