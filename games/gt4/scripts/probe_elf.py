"""Read the SCUS_973.28 ELF headers, sections, program headers and strings."""
from pathlib import Path
import struct

ROOT = Path(r"D:\CodexBuilds\gt4\game_files")
ELF = ROOT / "SCUS_973.28"


def main():
    data = ELF.read_bytes()
    e = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = e
    print(f"type={e_type} machine={e_machine} entry=0x{e_entry:08x} phoff={e_phoff} shoff={e_shoff} flags=0x{e_flags:08x} phnum={e_phnum} shnum={e_shnum} shstrndx={e_shstrndx}")
    print("\nProgram headers:")
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<IIIIIIII", data, e_phoff + i * e_phentsize)
        print(f"  [{i}] type={p_type} off=0x{p_offset:x} vaddr=0x{p_vaddr:x} paddr=0x{p_paddr:x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x} flags={p_flags} align=0x{p_align:x}")
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize)
        sections.append(sh)
    strtab_off = sections[e_shstrndx][4]
    print("\nSections:")
    for i, sh in enumerate(sections):
        name_off, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = sh
        end = data.index(b"\0", strtab_off + name_off)
        name = data[strtab_off + name_off:end].decode("ascii", "replace")
        print(f"  [{i:2}] {name:18} type={sh_type:2} flags=0x{sh_flags:08x} addr=0x{sh_addr:08x} off=0x{sh_offset:06x} size=0x{sh_size:x}")
    print("\nStrings of interest:")
    for needle in (b"GT4.VOL", b"VOL", b"lib", b"sce", b"IOPRP", b"CORE", b"overlay", b".irx"):
        pos = 0
        hits = []
        while True:
            i = data.find(needle, pos)
            if i < 0:
                break
            hits.append(i)
            pos = i + 1
            if len(hits) > 12:
                break
        print(f"  {needle!r}: {len(hits)} {hits[:12]}")


if __name__ == "__main__":
    main()
