"""Catalog EE kernel syscalls in a PS2 ELF: SYSCALL preceded by `addiu/ori $v1, $zero, N`."""
import json, struct, sys

def main(elf_path, out_path):
    d = open(elf_path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shnum, shstrndx = struct.unpack_from('<HH', d, 0x30)
    secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * 40) for i in range(shnum)]
    strtab = secs[shstrndx][4]
    name = lambda s: d[strtab + s[0]:].split(b'\0')[0].decode()
    text = next(s for s in secs if name(s) == '.text')
    addr, off, size = text[3], text[4], text[5]
    calls = {}
    for i in range(0, size, 4):
        if struct.unpack_from('<I', d, off + i)[0] != 0x0000000C:
            continue
        num = None
        for back in range(1, 4):
            w = struct.unpack_from('<I', d, off + i - back * 4)[0]
            if (w & 0xFFFF0000) in (0x24030000, 0x34030000):  # addiu/ori v1, zero, imm
                num = w & 0xFFFF
                if w >> 26 == 9 and num & 0x8000:
                    num -= 0x10000
                break
        calls.setdefault(str(num), []).append(f'0x{addr + i:08X}')
    json.dump({'elf': elf_path, 'syscalls': calls}, open(out_path, 'w'), indent=1)
    for k in sorted(calls, key=lambda k: (k == 'None', int(k) if k != 'None' else 0)):
        print(f'{k:>6} x{len(calls[k])}  {calls[k][0]}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
