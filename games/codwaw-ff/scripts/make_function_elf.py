"""Create a private analysis ELF containing one selected EE code range."""
import pathlib
import struct
import sys

source, output, start, end = sys.argv[1:5]
start, end = int(start, 0), int(end, 0)
if end <= start or start % 4 or end % 4:
    raise SystemExit("Expected a nonempty aligned [start, end) range")
b = pathlib.Path(source).read_bytes()
segments = []
if len(sys.argv) >= 6:
    segments.append((1, 0, int(sys.argv[5], 0), 0, len(b), 0, 0, 0))
else:
    phoff = struct.unpack_from("<I", b, 28)[0]
    count = struct.unpack_from("<H", b, 44)[0]
    segments = [struct.unpack_from("<8I", b, phoff + i * 32) for i in range(count)]
for segment in segments:
    kind, offset, address, _, size, _, _, _ = segment
    if kind == 1 and address <= start < end <= address + size:
        code = b[offset + start-address:offset + end-address]
        break
else:
    raise SystemExit("Code range is outside a file-backed LOAD segment")
names = b"\0.text\0.shstrtab\0"
nameoff = 128 + len(code)
shoff = (nameoff + len(names) + 3) & ~3
ident = b"\x7fELF\x01\x01\x01" + bytes(9)
entry = int(sys.argv[6], 0) if len(sys.argv) >= 7 else start
if entry % 4 or not start <= entry < end:
    raise SystemExit("Entry must be aligned and inside the selected code range")
header = ident + struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, entry, 52, shoff, 0x20924001, 52, 32, 1, 40, 3, 2)
program = struct.pack("<8I", 1, 128, start, start, len(code), len(code), 5, 4)
sections = bytes(40)
sections += struct.pack("<10I", 1, 1, 6, start, 128, len(code), 0, 0, 4, 0)
sections += struct.pack("<10I", 7, 3, 0, 0, nameoff, len(names), 0, 0, 1, 0)
image = (header + program).ljust(128, b"\0") + code + names
image = image.ljust(shoff, b"\0") + sections
pathlib.Path(output).write_bytes(image)
print(f"Wrote {len(code)} original code bytes, {start:#x}..{end:#x}")
