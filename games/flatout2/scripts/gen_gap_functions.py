"""Translate functions the Ghidra map leaves out (analyzer starts in gaps between Ghidra functions).

Writes config/gap-functions.{csv,toml}, runs the existing ps2_recomp into project/gap-functions and emits
project/gap-functions.inc (declarations + table entries). main.cpp installs the entries into the AOT table,
replacing the label bindings the AOT emitted for those addresses (Ghidra functions still win because a gap never
starts inside one).

Besides the analyzer's starts, function-pointer only targets are recovered (same rules as Boku2): Ghidra
`entry_` labels in gaps, code after a Ghidra function end, after `jr ra`+delay slot and after `j` (tail call)
+delay slot outside Ghidra functions, and pointers in the data that land on function-looking code (vtables).
"""
import bisect
import csv
import json
import os
import re
import subprocess
from pathlib import Path

from local_tools import recompiler

root = Path(__file__).resolve().parents[1]
text_end = 0x3FD998  # end of .text (0x100000 + 0x2FD998)
data_end = 0x4A5148  # end of the loaded file image (p_filesz)
ENTRY = 0x2B6EF0  # ELF entry (Ghidra 'entry', 0x2B6EF0-0x2B7130)
ghidra = sorted((int(r['Start'], 16), int(r['End'], 16))
                for r in csv.DictReader((root / 'config/ghidra-functions-only.csv').open()))
starts = [s for s, _ in ghidra]
analyzer = sorted({int(m, 16) for m in re.findall(r'Function \w+?_([0-9A-F]{8})\b',
                                                   (root / 'analysis/analyzer.log').read_text(errors='replace'))})


def covered(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return i >= 0 and ghidra[i][0] <= addr < ghidra[i][1]


# Ghidra's code labels in gaps (e.g. indirect-call targets) are entries too.
labels = {int(r['Start'], 16) for r in csv.DictReader((root / 'config/ghidra-functions.csv').open())
          if r['Name'].startswith('entry_')}
gaps = sorted(a for a in set(analyzer) | labels if 0x100000 <= a < text_end and not covered(a))
bounds = sorted(set(starts) | set(gaps) | {text_end})
rows = []
for a in gaps:
    end = bounds[bisect.bisect_right(bounds, a)]
    rows.append((f'gap_{a:08x}', a, end))

# Function-pointer targets (e.g. vtables) sit inside the gap ranges above: code after every Ghidra function
# end and after every `jr ra` + delay slot outside Ghidra functions (past nop padding) is another entry.
# Their ranges end at the next known start; the ranges above stay as they were.
elf = (root / 'game_files/SLUS_212.51').read_bytes()
phoff, phnum = int.from_bytes(elf[0x1C:0x20], 'little'), int.from_bytes(elf[0x2C:0x2E], 'little')
segments = []
for i in range(phnum):
    p = phoff + i * 32
    if int.from_bytes(elf[p:p + 4], 'little') == 1:
        segments.append((int.from_bytes(elf[p + 8:p + 12], 'little'), int.from_bytes(elf[p + 4:p + 8], 'little'),
                         int.from_bytes(elf[p + 16:p + 20], 'little')))


def word(a):
    for va, off, size in segments:
        if va <= a < va + size:
            return int.from_bytes(elf[off + a - va:off + a - va + 4], 'little')
    return 0


known = set(starts) | set(gaps)
trailing = set()


def after(a):
    """Start after `a` once the nop padding is skipped, if nobody claimed it yet."""
    while a < text_end and word(a) == 0:
        a += 4
    if a < text_end and a not in known and not covered(a):
        trailing.add(a)


for _, e in ghidra:
    after(e)
for a in range(0x100000, text_end, 4):
    if word(a) == 0x03E00008 and not covered(a):
        after(a + 8)
# Tail calls end functions too: code after a `j target` + delay slot outside Ghidra functions.
for a in range(0x100000, text_end, 4):
    if word(a) >> 26 == 0x02 and not covered(a):
        after(a + 8)
# Virtual-table and callback pointers in the data that land on a function-looking start (prologue, bare `jr ra`,
# or right after a `jr ra`/`j` + delay slot or nop padding) and are not a known start yet.
for a in range(text_end, data_end - 3, 4):
    t = word(a)
    if not (0x100000 <= t < text_end) or t & 3 or t in known or t in trailing:
        continue
    w, prev2 = word(t), word(t - 8)
    if ((w >> 16) == 0x27BD and w & 0x8000) or w == 0x03E00008 or word(t - 4) == 0 or prev2 == 0x03E00008 \
            or prev2 >> 26 == 0x02:
        trailing.add(t)
all_bounds = sorted(set(bounds) | trailing)
for a in sorted(trailing):
    rows.append((f'gap_{a:08x}', a, all_bounds[bisect.bisect_right(all_bounds, a)]))
rows.sort(key=lambda r: r[1])

out_dir = root / 'project/gap-functions'
csv_path = root / 'config/gap-functions.csv'
with csv_path.open('w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['Name', 'Start', 'End', 'Size'])
    # The generator anchors its table at the ELF entry; that entry is skipped when installing.
    w.writerow(['entry', f'0x{ENTRY:08X}', f'0x{dict(ghidra)[ENTRY]:08X}', dict(ghidra)[ENTRY] - ENTRY])
    for name, a, e in rows:
        w.writerow([name, f'0x{a:08X}', f'0x{e:08X}', e - a])
q = lambda p: json.dumps(p.relative_to(root).as_posix())  # the tool runs from the repository root
toml_path = root / 'config/gap-functions.toml'
toml_path.write_text('\n'.join([
    '[general]', f'input = {q(root / "game_files/SLUS_212.51")}', f'output = {q(out_dir)}',
    f'ghidra_output = {q(csv_path)}', 'single_file_output = false', 'patch_syscalls = false',
    'patch_cop0 = true', 'patch_cache = true', 'stubs = []', 'skip = []']) + '\n')

tool = recompiler()
(root / 'analysis').mkdir(exist_ok=True)
with (root / 'analysis/gap-functions.log').open('w') as log:
    subprocess.run([str(tool), str(toml_path)], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)

table = (out_dir / 'register_functions.cpp').read_text()
entries = re.findall(r'= (gap_[0-9a-f]+_0x[0-9a-f]+); // (0x[0-9a-f]+)', table)
funcs = sorted({fn for fn, _ in entries})
(root / 'project/gap-functions.inc').write_text(
    '// Generated by scripts/gen_gap_functions.py; do not edit.\n' +
    ''.join(f'void {fn}(uint8_t*, R5900Context*, PS2Runtime*);\n' for fn in funcs) +
    'static const Fo2TableEntry kFo2GapFunctions[] = {\n' +
    ''.join(f'    {{{addr}u, {fn}}},\n' for fn, addr in entries) + '};\n')
print(f'{len(rows)} gap functions, {len(funcs)} generated, {len(entries)} table entries')
