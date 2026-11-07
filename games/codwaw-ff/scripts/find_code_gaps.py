"""List .text ranges not covered by any function in ghidra-functions.csv, and data words pointing into them."""
import csv
import json
import struct

elf = open('analysis/disc/SLUS_217.46', 'rb').read()
shoff, = struct.unpack_from('<I', elf, 32)
shnum, shstr = struct.unpack_from('<HH', elf, 48)
secs = [struct.unpack_from('<10I', elf, shoff + i * 40) for i in range(shnum)]
strtab = secs[shstr][4]


def nm(o):
    return elf[strtab + o:elf.index(b'\0', strtab + o)].decode()


sec = {nm(s[0]): s for s in secs}
t = sec['.text']
tlo, thi, toff = t[3], t[3] + t[5], t[4]
rows = sorted((int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(open('config/ghidra-functions.csv')))
cov = bytearray((thi - tlo) // 4)
for s, e in rows:
    for a in range(max(s, tlo), min(e, thi), 4):
        cov[(a - tlo) // 4] = 1


def w(a):
    return struct.unpack_from('<I', elf, toff + a - tlo)[0]


gaps = []
a = tlo
while a < thi:
    if cov[(a - tlo) // 4]:
        a += 4
        continue
    b = a
    while b < thi and not cov[(b - tlo) // 4]:
        b += 4
    nz = [x for x in range(a, b, 4) if w(x) != 0]
    if nz:
        gaps.append((nz[0], nz[-1] + 4))
    a = b

ptr = set()
for n, s in sec.items():
    if n in ('.text', '.vutext') or s[1] != 1 or not s[3]:
        continue
    for o in range(0, s[5] - 3, 4):
        v, = struct.unpack_from('<I', elf, s[4] + o)
        if tlo <= v < thi and v % 4 == 0 and not cov[(v - tlo) // 4]:
            ptr.add(v)
print(f"text {tlo:#x}..{thi:#x}")
print(len(gaps), 'gaps,', sum(e - s for s, e in gaps), 'bytes;', len(ptr), 'data pointers into gaps')
for s, e in sorted(gaps, key=lambda g: g[0] - g[1])[:15]:
    print(hex(s), hex(e), e - s)
json.dump({'text': [tlo, thi], 'gaps': gaps, 'pointers': sorted(ptr)}, open('analysis/code-gaps.json', 'w'))
print('0x4532e0 pointer:', 0x4532e0 in ptr, [(hex(g[0]), hex(g[1])) for g in gaps if g[0] <= 0x4532e0 < g[1]])
