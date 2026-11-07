"""Build a private analysis ELF holding every uncovered .text range from analysis/code-gaps.json,
plus a range for each unregistered referenced address in analysis/unregistered-targets.json
(from that address to the next known function start).

Each gap becomes its own LOAD segment and .text section, so code outside the gaps is never
retranslated. Also writes a function map (Ghidra CSV format) that splits gaps at data pointers.
Usage: python scripts/make_gaps_elf.py OUTPUT.elf OUTPUT.csv
"""
import bisect
import csv
import json
import struct
import sys

output_elf, output_csv = sys.argv[1:3]
elf = open('analysis/disc/SLUS_217.46', 'rb').read()
info = json.load(open('analysis/code-gaps.json'))
tlo, _ = info['text']
phoff = struct.unpack_from('<I', elf, 28)[0]
_, file_off, vaddr = struct.unpack_from('<3I', elf, phoff)
gaps = [tuple(g) for g in info['gaps']]
pointers = info['pointers']
targets = sorted(int(k, 16) for k in json.load(open('analysis/unregistered-targets.json')))
known = sorted({int(r['Start'], 16) for r in csv.DictReader(open('config/ghidra-functions.csv'))}
               | {s for s, _ in gaps} | set(targets) | {info['text'][1]})
extra = [(t, known[bisect.bisect_right(known, t)]) for t in targets]
pointers = pointers + targets
gaps = sorted(gaps + extra)

segments = [(s, elf[file_off + s - vaddr:file_off + e - vaddr]) for s, e in gaps]
count = len(segments)
data_start = (52 + 32 * count + 15) & ~15
blob = bytearray()
offsets = []
for _, code in segments:
    offsets.append(data_start + len(blob))
    blob += code
names = b"\0.text\0.shstrtab\0"
nameoff = data_start + len(blob)
shoff = (nameoff + len(names) + 3) & ~3
entry = gaps[0][0]
header = b"\x7fELF\x01\x01\x01" + bytes(9)
header += struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, entry, 52, shoff, 0x20924001, 52, 32, count, 40, count + 2, count + 1)
program = b"".join(struct.pack("<8I", 1, off, s, s, len(c), len(c), 5, 4) for off, (s, c) in zip(offsets, segments))
sections = bytes(40)
sections += b"".join(struct.pack("<10I", 1, 1, 6, s, off, len(c), 0, 0, 4, 0) for off, (s, c) in zip(offsets, segments))
sections += struct.pack("<10I", 7, 3, 0, 0, nameoff, len(names), 0, 0, 1, 0)
image = (header + program).ljust(data_start, b"\0") + blob + names
image = image.ljust(shoff, b"\0") + sections
open(output_elf, 'wb').write(image)

starts = sorted({s for s, _ in gaps} | {p for p in pointers if any(s <= p < e for s, e in gaps)})
ends = {s: e for s, e in gaps}
gap_starts = [s for s, _ in gaps]
with open(output_csv, 'w', newline='') as stream:
    writer = csv.writer(stream)
    writer.writerow(["Name", "Start", "End", "Size"])
    for index, start in enumerate(starts):
        gap_end = ends[gap_starts[bisect.bisect_right(gap_starts, start) - 1]]
        end = min(gap_end, starts[index + 1]) if index + 1 < len(starts) else gap_end
        writer.writerow([f"sub_{start:08X}_GAP", f"0x{start:08X}", f"0x{end:08X}", end - start])
print(f"Wrote {count} segments ({len(blob)} bytes) and {len(starts)} function starts")
