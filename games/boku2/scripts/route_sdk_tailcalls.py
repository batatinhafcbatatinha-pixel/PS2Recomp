"""Route generated direct calls into SDK-bound functions through the function table (idempotent).

Tail jumps (`j func`) to a known function are emitted as direct C++ calls, which bypass the table overrides of
project/sdk-handlers.inc. Rewriting the call sites to lookupFunction makes the native SDK handler run there too.
"""
import os
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
runner = root / 'project/runner'
bound = set(re.findall(r'\{0x([0-9a-f]+)u, \w+\}', (root / 'project/sdk-handlers.inc').read_text()))
names = {}
for f in os.listdir(runner):
    m = re.match(r'(.+)_0x([0-9a-f]+)\.cpp$', f)
    if m and m.group(2) in bound:
        names[f[:-4]] = m.group(2)
pattern = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\(rdram, ctx, runtime\)')
changed = []
for path in sorted(runner.glob('*.cpp')):
    text = path.read_text()
    own = path.stem
    new = pattern.sub(lambda m: m.group(0) if m.group(1) == own else
                      f'runtime->lookupFunction(0x{names[m.group(1)]}u)(rdram, ctx, runtime)', text)
    if new != text:
        path.write_text(new)
        changed.append(path.name)
print(f'{len(changed)} files routed through the table')
if '--list' in sys.argv:
    print('\n'.join(changed))
