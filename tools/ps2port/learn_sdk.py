"""Add SDK function signatures to sdk_signatures.json from games where the functions are known.

usage: learn_sdk.py <ELF> name@0xADDR [name@0xADDR ...]
Runs the PS2Recomp analyzer on the ELF for the anchors (functions it already identifies), then stores each given
function's signature (a hash of its masked words, see sdk.py). Existing entries with the same name and words are kept once.
"""
import json
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import elfinfo  # noqa: E402
import sdk  # noqa: E402
from ps2port import tool  # noqa: E402


def main():
    elf_path = Path(sys.argv[1])
    named = {}
    for item in sys.argv[2:]:
        n, a = item.split('@')
        named[n] = int(a, 16)
    elf = elfinfo.Elf.load(elf_path)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'a.toml'
        subprocess.run([str(tool('ps2_analyzer')), str(elf_path), str(out)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
        stubs = tomllib.loads(out.read_text())['general'].get('stubs', [])
    known = {}
    for m in stubs:
        n, a = m.split('@')
        known.setdefault(n, int(a, 16))
    db = sdk.load_db()
    have = {(e['name'], e['sig']['sha1']) for e in db['functions']}
    added = 0
    for entry in sdk.learn(elf, named, known):
        key = (entry['name'], entry['sig']['sha1'])
        if key in have:
            continue
        db['functions'].append(entry)
        have.add(key)
        added += 1
    db['functions'].sort(key=lambda e: e['name'])
    sdk.SIGNATURES.write_text(json.dumps(db, indent=1) + '\n', encoding='utf-8')
    print(f'{elf_path.name}: {added} new signatures ({len(db["functions"])} total)')


if __name__ == '__main__':
    main()
