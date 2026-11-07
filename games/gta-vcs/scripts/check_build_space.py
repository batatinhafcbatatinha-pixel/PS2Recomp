"""Conservative admission check; no downloads or filesystem mutation."""
from pathlib import Path
import hashlib
import json
import shutil

root = Path(__file__).resolve().parents[1]
runner = root / 'project/runner'
if not (runner / 'register_functions.cpp').is_file():
    raise SystemExit('No translated function table; run generate.cmd before building')
stamp_path = runner / '.generation-complete.json'
if not stamp_path.is_file():
    raise SystemExit('No complete-generation marker; run generate.cmd before building')
stamp = json.loads(stamp_path.read_text())
digest = hashlib.sha256((root / 'config/recomp.toml').read_bytes()).hexdigest()
if stamp.get('config_sha256') != digest:
    raise SystemExit('Configuration changed since generation; run generate.cmd before building')
source_bytes = sum(p.stat().st_size for p in runner.glob('*.cpp'))
# Native objects, library, executable, dependencies and compiler scratch can exceed source size.
required = max(4 * 1024**3, source_bytes * 12)
free = shutil.disk_usage(root).free
print(f'Build space (informational only): {free / 1024**3:.1f} GiB free; conservative estimate {required / 1024**3:.1f} GiB')
