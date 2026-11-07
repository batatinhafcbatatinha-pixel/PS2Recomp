"""Resolve analysis/profile.txt (module offsets from PS2X_PROFILE=1) with the linker map.

Usage: python scripts/profile_report.py [TOP] [--since EARLIER_PROFILE]
Groups samples by function and prints the hottest ones per sampled thread.
With --since, only samples taken after that earlier copy of profile.txt count.
"""
import bisect
import collections
import re
import sys

args = [a for a in sys.argv[1:]]
since = None
if '--since' in args:
    since = args[args.index('--since') + 1]
    del args[args.index('--since'):args.index('--since') + 2]
top = int(args[0]) if args else 40
base = None
symbols = []
for line in open('build/game/ps2xRuntime/ps2EntryRunner.map', errors='replace'):
    if base is None:
        m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            base = int(m.group(1), 16)
        continue
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f?\s*(\S*)', line)
    if m:
        symbols.append((int(m.group(2), 16) - base, m.group(1), m.group(3)))
symbols.sort()
starts = [s[0] for s in symbols]

def raw_counts(path):
    counts = collections.Counter()
    thread = None
    for line in open(path):
        parts = line.split()
        if parts[0] == 'thread':
            thread = parts[1]
        elif parts[0] != 'module':
            counts[(thread, parts[0])] += int(parts[1])
    return counts


earlier = raw_counts(since) if since else collections.Counter()
threads = []
modules = {}
for line in open('analysis/profile.txt'):
    parts = line.split(maxsplit=2)
    if parts[0] == 'module':
        modules[int(parts[1], 16)] = parts[2].strip().replace(chr(92), '/').split('/')[-1]
        continue
    parts = line.split()
    if parts[0] == 'thread':
        threads.append((parts[1], 0, collections.Counter()))
        continue
    offset, hits = int(parts[0], 16), int(parts[1]) - earlier[(threads[-1][0], parts[0])]
    if hits <= 0:
        continue
    threads[-1] = (threads[-1][0], threads[-1][1] + hits, threads[-1][2])
    if offset >> 63:
        threads[-1][2][f'<dll {modules.get(offset, hex(offset))}>'] += hits
        continue
    if offset == 0:
        threads[-1][2]['<outside image: system DLL / driver>'] += hits
        continue
    i = bisect.bisect_right(starts, offset) - 1
    name = f'{symbols[i][1]}  [{symbols[i][2]}]' if i >= 0 else hex(offset)
    threads[-1][2][name] += hits
for name, total, counter in threads:
    print(f'== thread {name}: {total} samples')
    for func, hits in counter.most_common(top):
        print(f'{100.0 * hits / max(total, 1):6.2f}%  {func}')
