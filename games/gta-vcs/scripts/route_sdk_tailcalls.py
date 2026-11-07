"""Route generated direct calls into natively bound functions through the function table (idempotent).

Tail jumps (`j func`) to a known function are emitted as direct C++ calls, which bypass the table overrides that
project/main.cpp installs (SDK handlers from project/sdk-handlers.inc and the libfs handlers in main.cpp).
Rewriting those call sites to lookupFunction makes the native handler run there too. Run after every generation.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
bound = {int(a, 16) for a in re.findall(r'\{0x([0-9a-fA-F]+)u, \w+', (root / 'project/sdk-handlers.inc').read_text())}
bound |= {int(a, 16) for a in re.findall(r'\{0x([0-9a-fA-F]+)u, VcsFs', (root / 'project/main.cpp').read_text())}
folders = [root / 'project/runner', root / 'project/gap-functions']
names = {}
for folder in folders:
    for path in folder.glob('*.cpp'):
        m = re.match(r'(.+)_0x([0-9a-f]+)$', path.stem)
        if m and int(m.group(2), 16) in bound:
            names[path.stem] = int(m.group(2), 16)
if not names:
    raise SystemExit('No bound functions found in the generated sources')
pattern = re.compile(r'\b(' + '|'.join(map(re.escape, sorted(names, key=len, reverse=True))) + r')\(rdram, ctx, runtime\)')
changed = []
for folder in folders:
    for path in sorted(folder.glob('*.cpp')):
        text = path.read_text(encoding='utf-8', errors='surrogateescape')
        if not pattern.search(text):
            continue
        own = path.stem
        new = pattern.sub(lambda m: m.group(0) if m.group(1) == own else
                          f'runtime->lookupFunction(0x{names[m.group(1)]:x}u)(rdram, ctx, runtime)', text)
        if new != text:
            path.write_text(new, encoding='utf-8', errors='surrogateescape')
            changed.append(path.name)
print(f'{len(names)} bound functions; {len(changed)} files routed through the table')
if '--list' in sys.argv:
    print('\n'.join(changed))
