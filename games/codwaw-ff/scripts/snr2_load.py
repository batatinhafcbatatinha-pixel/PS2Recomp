"""Load an SN Systems SNR2 relocatable module (the game's MISC/MPEG_DLL.ASR) the way the game's loader does.

Reads only your own disc files and writes a private memory image (never published):
  python scripts/snr2_load.py ASR_FILE MAIN_ELF BASE OUTPUT.bin

SNR2 header (32-bit words): magic, import relocation table offset, its count, symbol table offset, its count, ...
Imports: 12-byte records (offset, type | symbol << 8, addend); type 4 = 26-bit jump, 5 = HI16, 6 = LO16. Symbols
are 12-byte records (name offset, value, hash): a nonzero value is defined by the module itself (base + value);
the others are resolved by name in the main executable's export records (name pointer, address, hash).
Internal relocations: a byte stream at header word 8. Each byte's low two bits are the type (0 LO16, 1 HI16,
2 26-bit jump, 3 32-bit word) and the rest is the distance in bytes from the previous relocation; a byte with
distance 0 is followed by the absolute 32-bit offset. A zero absolute offset ends the stream.
HI16 relocations take the carry of the next LO16 relocation, as in the MIPS ELF ABI.
"""
import struct
import sys

ASR_MODULE_OFFSET = 0x58  # the SNR2 header inside the Asura container


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def cstring(data, offset):
    return bytes(data[offset:data.index(b'\0', offset)])


def elf_image(elf):
    """(bytes at a virtual address) reader for the main executable's LOAD segments."""
    phoff, count = u32(elf, 28), struct.unpack_from('<H', elf, 44)[0]
    segments = [struct.unpack_from('<8I', elf, phoff + 32 * i) for i in range(count)]
    segments = [(s[2], s[1], max(s[4], s[5])) for s in segments if s[0] == 1]  # memory size: .bss included

    def at(address):
        for vaddr, offset, size in segments:
            if vaddr <= address < vaddr + size:
                return offset + address - vaddr
        return None
    return at


def exports(elf, names):
    """Address of each wanted name from the main executable's export records (name pointer, address, hash)."""
    at = elf_image(elf)
    phoff = u32(elf, 28)
    _, file_offset, vaddr = struct.unpack_from('<3I', elf, phoff)
    found = {}
    for name in names:
        position = elf.find(name + b'\0')
        while position >= 0 and name not in found:
            pointer = struct.pack('<I', position - file_offset + vaddr)
            record = elf.find(pointer)
            while record >= 0:
                address = u32(elf, record + 4)
                if record % 4 == 0 and at(address) is not None:
                    found[name] = address
                    break
                record = elf.find(pointer, record + 1)
            position = elf.find(name + b'\0', position + 1)
    missing = [n.decode() for n in names if n not in found]
    if missing:
        raise SystemExit(f'Exports not found in the main executable: {missing}')
    return found


def patch(module, offset, kind, value, pending):
    word = u32(module, offset)
    if kind == 'j26':
        word = (word & 0xfc000000) | ((value >> 2) & 0x03ffffff)
    elif kind == 'hi16':
        pending.append((offset, value))
        return
    elif kind == 'lo16':
        word = (word & 0xffff0000) | (value & 0xffff)
        for hi_offset, hi_value in pending:  # a HI16 takes the carry of the low half that follows it
            hi_word = u32(module, hi_offset)
            struct.pack_into('<I', module, hi_offset, (hi_word & 0xffff0000) | (((hi_value + 0x8000) >> 16) & 0xffff))
        pending.clear()
    else:
        word = value & 0xffffffff
    struct.pack_into('<I', module, offset, word)


def load(asr, elf, base):
    module = bytearray(asr[ASR_MODULE_OFFSET:])
    if module[:4] != b'SNR2':
        raise SystemExit('Not an SNR2 module')
    import_table, import_count, symbol_table, symbol_count = (u32(module, 4 * i) for i in range(1, 5))
    stream = u32(module, 32)

    # Internal relocations: the module's own addresses move by `base`.
    pending, offset, position = [], 0, stream
    while True:
        byte = module[position]
        position += 1
        kind, distance = byte & 3, byte & ~3
        if distance:
            offset += distance
        else:
            offset = u32(module, position)
            position += 4
            if offset == 0:
                break
        word = u32(module, offset)
        if kind == 2:
            patch(module, offset, 'j26', ((word & 0x03ffffff) << 2) + base, pending)
        elif kind == 3:
            patch(module, offset, 'w32', word + base, pending)
        elif kind == 1:
            pending.append((offset, None))  # resolved with the next LO16 below
        else:
            low = word & 0xffff
            low_signed = low - ((low & 0x8000) << 1)
            for i, (hi_offset, _) in enumerate(pending):
                if _ is None:
                    pending[i] = (hi_offset, ((u32(module, hi_offset) & 0xffff) << 16) + low_signed + base)
            patch(module, offset, 'lo16', low + base, pending)

    # Imports: calls and address loads bound to functions of the main executable.
    symbols = [struct.unpack_from('<3I', module, symbol_table + 12 * i) for i in range(symbol_count)]
    names = [cstring(module, name) for name, _, _ in symbols]
    # A symbol with a value is defined by the module itself (and exported to the game); the rest come from the game.
    resolved = exports(elf, sorted({n for n, (_, value, _) in zip(names, symbols) if n and not value}))
    resolved.update({n: base + value for n, (_, value, _) in zip(names, symbols) if n and value})
    pending = []
    for i in range(import_count):
        offset, info, addend = struct.unpack_from('<3I', module, import_table + 12 * i)
        kind, symbol = info & 0xff, info >> 8
        value = resolved[names[symbol]] + addend
        if kind == 4:
            patch(module, offset, 'j26', value, pending)
        elif kind == 5:
            patch(module, offset, 'hi16', value, pending)
        elif kind == 6:
            patch(module, offset, 'lo16', value, pending)
        else:
            raise SystemExit(f'Unknown import relocation type {kind} at {offset:#x}')
    return bytes(module)


if __name__ == '__main__':
    asr_path, elf_path, base, output = sys.argv[1:5]
    image = load(open(asr_path, 'rb').read(), open(elf_path, 'rb').read(), int(base, 0))
    open(output, 'wb').write(image)
    print(f'Loaded {len(image)} bytes at {int(base, 0):#x}')
