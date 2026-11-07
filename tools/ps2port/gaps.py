"""Functions the Ghidra map leaves out ("gap" functions), found by general rules for any PS2 executable.

Sources of entry points outside the Ghidra functions:
- function starts the PS2Recomp analyzer reports, and Ghidra `entry_` labels (indirect-call targets);
- code after a Ghidra function end, after `jr ra` + delay slot and after a tail `j` + delay slot (past nop padding);
- pointers in the initialised data that land on function-looking code (vtables, callbacks);
- prologues Ghidra merged into the function before them (that one ends in a call that never returns);
- extra starts: addresses the runtime reported as missing while the game ran (observed), and a profile's list.
Each range ends at the next known start. Ghidra functions always win: a gap never starts inside one, except the
merged-prologue split, which keeps the Ghidra end.
"""
import bisect
import csv
import re


def is_prologue(w):
    return (w >> 16) == 0x27BD and w & 0x8000  # addiu sp, sp, -N


def find(elf, ghidra_rows, labels, analyzer_log_text, extra_starts=()):
    code = elf.code_ranges()
    code_lo, code_hi = code[0][0], code[-1][1]

    def in_code(a):
        return any(lo <= a < hi for lo, hi in code)

    ghidra = sorted(ghidra_rows)
    starts = [s for s, _ in ghidra]

    def covered(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and ghidra[i][0] <= a < ghidra[i][1]

    analyzer = {int(m, 16) for m in re.findall(r'Function \w+?_([0-9A-Fa-f]{8})\b', analyzer_log_text)}
    gaps = sorted(a for a in analyzer | set(labels) | set(extra_starts) if in_code(a) and not a & 3 and not covered(a))
    bounds = sorted(set(starts) | set(gaps) | {hi for _, hi in code})

    def next_bound(a, extra=()):
        b = sorted(set(bounds) | set(extra)) if extra else bounds
        return b[bisect.bisect_right(b, a)]

    rows = [(a, next_bound(a)) for a in gaps]
    known = set(starts) | set(gaps)
    trailing = set()
    word = elf.word

    def after(a):
        while in_code(a) and word(a) == 0:
            a += 4
        if in_code(a) and a not in known and not covered(a):
            trailing.add(a)

    for _, e in ghidra:
        after(e)
    for lo, hi in code:
        for a in range(lo, hi, 4):
            w = word(a)
            if covered(a):
                continue
            if w == 0x03E00008 or w >> 26 == 0x02:  # jr ra / j target: the next code is another function
                after(a + 8)
    pointers = set()
    for lo, hi in elf.data_ranges():
        for a in range(lo, hi - 3, 4):
            t = word(a)
            if not in_code(t) or t & 3:
                continue
            pointers.add(t)
            if t in known or t in trailing:
                continue
            w, prev2 = word(t), word(t - 8)
            if is_prologue(w) or w == 0x03E00008 or word(t - 4) == 0 or prev2 == 0x03E00008 or prev2 >> 26 == 0x02:
                trailing.add(t)
    all_bounds = sorted(set(bounds) | trailing)
    for a in sorted(trailing):
        rows.append((a, all_bounds[bisect.bisect_right(all_bounds, a)]))

    # Prologues inside a Ghidra range that follow a non-returning call (jal + delay + nop padding) or a data pointer.
    def ghidra_end(a):
        i = bisect.bisect_right(starts, a) - 1
        return ghidra[i][1] if i >= 0 and ghidra[i][0] < a < ghidra[i][1] else 0

    splits = []
    for lo, hi in code:
        for a in range(lo + 8, hi, 4):
            w = word(a)
            if not is_prologue(w) or not ghidra_end(a):
                continue
            b = a - 4
            while word(b) == 0 and b > a - 32:
                b -= 4
            flow_end = b != a - 4 and (word(b - 4) == 0x03E00008 or word(b - 4) >> 26 in (2, 3))
            if flow_end or a in pointers:
                splits.append(a)
    for i, a in enumerate(splits):
        end = ghidra_end(a)
        if i + 1 < len(splits) and splits[i + 1] < end:
            end = splits[i + 1]
        rows.append((a, end))
    seen, out = set(), []
    for a, e in sorted(rows):
        if a not in seen and e > a:
            seen.add(a)
            out.append((a, e))
    return out


def write_csv(path, elf, ghidra_rows, rows):
    """The generator anchors its table at the ELF entry; that row is skipped when the table is installed."""
    entry = elf.entry
    entry_end = next((e for s, e in ghidra_rows if s == entry), entry + 4)
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Name', 'Start', 'End', 'Size'])
        w.writerow(['entry', f'0x{entry:08X}', f'0x{entry_end:08X}', entry_end - entry])
        for a, e in rows:
            w.writerow([f'gap_{a:08x}', f'0x{a:08X}', f'0x{e:08X}', e - a])
