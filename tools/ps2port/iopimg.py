"""IOP modules inside an IOPRP / ROM image (ROMDIR): [(name, bytes)], empty for anything else.

ROMDIR: 16-byte entries (name[10], extinfo size u16, file size u32) starting with RESET, ROMDIR, EXTINFO; the files
follow one another from the start of the image, each aligned to 16 bytes. The table ends with an all-zero entry.
"""
import struct


def modules(data):
    pos = data.find(b'RESET\0\0\0\0\0')
    if pos < 0 or pos % 16 or data[pos + 16:pos + 22] != b'ROMDIR':
        return []
    out, offset, i = [], pos, pos
    while i + 16 <= len(data):
        raw = data[i:i + 10]
        if raw == b'\0' * 10:
            break
        _, size = struct.unpack_from('<HI', data, i + 10)
        name = raw.split(b'\0')[0].decode('ascii', 'replace')
        if offset + size > len(data):
            break
        blob = data[offset:offset + size]
        if blob[:4] == b'\x7fELF':
            out.append((name, blob))
        offset += (size + 15) & ~15
        i += 16
    return out
