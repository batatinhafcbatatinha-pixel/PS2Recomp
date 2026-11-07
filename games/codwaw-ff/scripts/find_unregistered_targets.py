"""Find code addresses referenced by data words or lui/addiu pairs that have no registered entry.

Registered = entries in project/runner/register_functions.cpp plus every start in
config/gaps-functions.csv. Writes analysis/unregistered-targets.json.
"""
import csv
import json
import re
import struct

elf = open('analysis/disc/SLUS_217.46', 'rb').read()
shoff, = struct.unpack_from('<I', elf, 32)
shnum, shstr = struct.unpack_from('<HH', elf, 48)
secs = [struct.unpack_from('<10I', elf, shoff + i * 40) for i in range(shnum)]
strtab = secs[shstr][4]
sec = {elf[strtab + s[0]:elf.index(b'\0', strtab + s[0])].decode(): s for s in secs}
t = sec['.text']
tlo, thi, toff = t[3], t[3] + t[5], t[4]

registered = {int(a, 16) for a in re.findall(r'// (0x[0-9a-f]+)$', open('project/runner/register_functions.cpp').read(), re.M)}
registered |= {int(r['Start'], 16) for r in csv.DictReader(open('config/gaps-functions.csv'))}


def word(a):
    return struct.unpack_from('<I', elf, toff + a - tlo)[0]


def plausible(v):
    # Entry must follow a jr ra / j delay slot, or start a stack frame, or follow a branch-likely slot.
    if not (tlo <= v < thi and v % 4 == 0) or v in registered:
        return False
    return word(v) != 0


targets = {}
for n, s in sec.items():
    if n in ('.text', '.vutext') or s[1] != 1 or not s[3]:
        continue
    for o in range(0, s[5] - 3, 4):
        v, = struct.unpack_from('<I', elf, s[4] + o)
        if plausible(v):
            targets.setdefault(v, f"data:{n}+{o:#x}")

hi = {}
for a in range(tlo, thi, 4):
    w = word(a)
    op = w >> 26
    if op == 15:  # lui
        hi[(w >> 16) & 31] = (a, (w & 0xffff) << 16)
    elif op in (9, 13) and (w >> 21) & 31 in hi:  # addiu / ori
        src_addr, base = hi[(w >> 21) & 31]
        if a - src_addr <= 64:
            imm = w & 0xffff
            v = (base + (imm - 0x10000 if op == 9 and imm & 0x8000 else imm)) & 0xffffffff
            if plausible(v):
                targets.setdefault(v, f"code:{src_addr:#x}")
    if op in (2, 3) or (op == 0 and w & 0x3f in (8, 9)):
        hi.clear()

json.dump({hex(k): v for k, v in sorted(targets.items())}, open('analysis/unregistered-targets.json', 'w'), indent=0)
data = sum(1 for v in targets.values() if v.startswith('data'))
print(f"{len(targets)} unregistered targets ({data} from data, {len(targets) - data} from lui pairs)")
print('0x28e628:', targets.get(0x28e628))
