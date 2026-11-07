"""Run PS2Recomp on config/recomp.toml into a scratch folder, then sync project/runner.

Only files whose content changed are replaced, so ninja recompiles just the affected chunks.
"""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import time

from local_tools import recompiler

root = Path(__file__).resolve().parents[1]
tool = recompiler()
out = root / 'project/runner'
scratch = root / 'project/runner.tmp'
if not (root / 'config/ghidra-core-functions-capped.csv').is_file():
    raise SystemExit('Run ghidra-export-core.cmd and scripts/prepare_config.py first')
shutil.rmtree(scratch, ignore_errors=True)
config = root / 'config/recomp.toml'
scratch_config = root / 'config/recomp.tmp.toml'
scratch_config.write_text(config.read_text().replace('output = "project/runner"', 'output = "project/runner.tmp"'))
(root / 'analysis').mkdir(exist_ok=True)
log = root / 'analysis/recompile.log'
with log.open('w') as stream:
    proc = subprocess.Popen([str(tool), str(scratch_config.relative_to(root))], cwd=root, stdout=stream,
                            stderr=subprocess.STDOUT)
    try:
        while proc.poll() is None:
            size = sum(p.stat().st_size for p in scratch.rglob('*') if p.is_file()) if scratch.exists() else 0
            if size > 4096 * 1024**2:
                proc.terminate()
                proc.wait()
                raise SystemExit('Generation stopped: 4 GiB output limit reached; output is incomplete')
            time.sleep(2)
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait()
scratch_config.unlink(missing_ok=True)
if proc.returncode:
    raise SystemExit(proc.returncode)
if not (scratch / 'register_functions.cpp').is_file():
    raise SystemExit('Generator exited successfully without registration source; incomplete output')
out.mkdir(parents=True, exist_ok=True)
(out / '.generation-complete.json').unlink(missing_ok=True)
new = {p.name for p in scratch.iterdir() if p.is_file()}
changed = removed = 0
for name in new:
    source, target = scratch / name, out / name
    if not target.is_file() or target.stat().st_size != source.stat().st_size or target.read_bytes() != source.read_bytes():
        shutil.copy2(source, target)
        changed += 1
for path in out.iterdir():
    if path.is_file() and path.name not in new and not path.name.startswith('.'):
        path.unlink()
        removed += 1
shutil.rmtree(scratch)
(out / '.generation-complete.json').write_text(json.dumps({
    'config_sha256': hashlib.sha256(config.read_bytes()).hexdigest(),
    'cpp_files': len(list(out.glob('*.cpp')))}, indent=2) + '\n')
print(f'{len(new)} generated files: {changed} changed, {removed} removed')
