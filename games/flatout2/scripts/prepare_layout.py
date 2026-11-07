"""Write game_files/.disc (sector index) with the disc's original file LBNs, from analysis/game-files-verification.json.

The engine streams MODELS/*.IMG and the audio banks by sector; keeping the original positions makes every LSN the
game computes match the disc. Without the report (another machine), Play-LCS.cmd falls back to the installer layout.
"""
import json
import sys
from pathlib import Path

from local_tools import ROOT
game_files = lambda: ROOT / "game_files"

sys.path.insert(0, str(ROOT.parent / 'port-installer'))
from layout import build_layout  # noqa: E402
from media import Entry  # noqa: E402

report = json.loads((ROOT / 'analysis/game-files-verification.json').read_text(encoding='utf-8'))
folder = game_files()
entries = []
for f in report['files']:
    if (folder / f['path']).stat().st_size != f['bytes']:
        raise SystemExit('Size differs from the verified extraction: ' + f['path'])
    entries.append(Entry(f['path'], f['bytes'], [(e['sector'] * 2048, e['bytes']) for e in f['extents']]))
layout, metadata = build_layout(entries)
(folder / '.disc').mkdir(exist_ok=True)
(folder / '.disc/layout.tsv').write_bytes(layout)
(folder / '.disc/metadata.bin').write_bytes(metadata)
print(f'{len(entries)} files at their disc LBNs; {len(layout) + len(metadata)} index bytes')
