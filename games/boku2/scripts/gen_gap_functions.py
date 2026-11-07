"""Translate functions the Ghidra map leaves out (analyzer starts in gaps between Ghidra functions).

Writes config/gap-functions.{csv,toml}, runs the existing ps2_recomp into project/gap-functions and emits
project/gap-functions.inc (declarations + table entries). main.cpp installs the entries only into empty slots,
so the main AOT table always wins.
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
text_end = 0x25225C
ghidra = sorted((int(r['Start'], 16), int(r['End'], 16))
                for r in csv.DictReader((root / 'config/ghidra-functions-only.csv').open()))
starts = [s for s, _ in ghidra]
analyzer = sorted({int(m, 16) for m in re.findall(r'Function \w+?_([0-9A-F]{8})\b',
                                                   (root / 'analysis/analyzer.log').read_text(errors='replace'))})


def covered(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return i >= 0 and ghidra[i][0] <= addr < ghidra[i][1]


# Ghidra's code labels in gaps (e.g. indirect-call targets) are entries too.
labels = {int(r['Start'], 16) for r in csv.DictReader((root / 'config/ghidra-all-labels.csv').open())
          if r['Name'].startswith('entry_')}
gaps = sorted(a for a in set(analyzer) | labels if 0x100000 <= a < text_end and not covered(a))
bounds = sorted(set(starts) | set(gaps) | {text_end})
rows = []
for a in gaps:
    end = bounds[bisect.bisect_right(bounds, a)]
    rows.append((f'gap_{a:08x}', a, end))

# Code right after a Ghidra function's end (past nop padding) that nobody claimed: function-pointer
# targets such as 0x227790 (filled into a table at run time, so no data reference in the ELF).
# Their ranges only end at the next known start; the ranges above stay as they were.
elf = (root / 'game_files/SCPS_150.26').read_bytes()
text_va, text_off = 0x100000, 0x1000


def word(a):
    return int.from_bytes(elf[text_off + a - text_va:text_off + a - text_va + 4], 'little')


known = set(starts) | set(gaps)
trailing = set()
for _, e in ghidra:
    a = e
    while a < text_end and word(a) == 0:
        a += 4
    if a < text_end and a not in known and not covered(a):
        trailing.add(a)
# Gap ranges run to the next known start, so one can hold several functions (0x228008 sat inside
# gap_00227fd8): code after every `jr ra` + delay slot outside Ghidra functions starts another one.
for a in range(text_va, text_end, 4):
    if word(a) == 0x03E00008 and not covered(a):
        b = a + 8
        while b < text_end and word(b) == 0:
            b += 4
        if b < text_end and b not in known and not covered(b):
            trailing.add(b)
# Tail calls end functions too: code after a `j target` + delay slot (0x1b6e70 followed one) outside Ghidra functions.
for a in range(text_va, text_end, 4):
    if word(a) >> 26 == 0x02 and not covered(a):
        b = a + 8
        while b < text_end and word(b) == 0:
            b += 4
        if b < text_end and b not in known and not covered(b):
            trailing.add(b)
# Virtual-table and callback pointers in the data that land on a function-looking start (prologue, bare `jr ra`,
# or right after a `jr ra`/`j` + delay slot or nop padding) and are not a known start yet.
data_start, data_end = text_end, 0x2A5AB8  # end of the loaded file image (filesz)
for o in range(text_off + data_start - text_va, min(len(elf), text_off + data_end - text_va) - 3, 4):
    t = int.from_bytes(elf[o:o + 4], 'little')
    if not (text_va <= t < text_end) or t & 3 or t in known or t in trailing:
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
    w.writerow(['entry', '0x00100008', '0x001000B4', 172])
    for name, a, e in rows:
        w.writerow([name, f'0x{a:08X}', f'0x{e:08X}', e - a])
q = lambda p: json.dumps(p.relative_to(root).as_posix())  # the tool runs from the repository root
toml_path = root / 'config/gap-functions.toml'
toml_path.write_text('\n'.join([
    '[general]', f'input = {q(root / "game_files/SCPS_150.26")}', f'output = {q(out_dir)}',
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
    'static const Boku2TableEntry kBoku2GapFunctions[] = {\n' +
    ''.join(f'    {{{addr}u, {fn}}},\n' for fn, addr in entries) + '};\n')
# One translation unit: the generated files are small, the shared runtime headers are not.
(root / 'project/gap-functions.cpp').write_text('// Generated by scripts/gen_gap_functions.py; do not edit.\n' +
    '#include "ps2_runtime.h"\n' +
    ''.join(f'void {fn}(uint8_t*, R5900Context*, PS2Runtime*);\n' for fn in funcs) +
    ''.join(f'#include "gap-functions/{fn}.cpp"\n' for fn in funcs))
print(f'{len(rows)} gap functions, {len(funcs)} generated, {len(entries)} table entries')
