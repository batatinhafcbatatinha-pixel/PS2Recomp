"""Generate all the game code from your own files, after ghidra-export.cmd and build-recompiler.cmd."""
import subprocess
import sys

from local_tools import ROOT, analyzer, recompiler

(ROOT / 'analysis').mkdir(exist_ok=True)
with (ROOT / 'analysis/analyzer.log').open('w') as log:
    subprocess.run([str(analyzer()), 'game_files/SLUS_218.15', 'config/analyzer-heuristic.toml'], cwd=ROOT, stdout=log,
                   stderr=subprocess.STDOUT, check=True)
with (ROOT / 'analysis/kernel-handlers.log').open('w') as log:
    subprocess.run([str(recompiler()), 'config/kernel-handlers.toml'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
for script in ('prepare_config.py', 'guarded_generate.py', 'make_chunks.py', 'gen_gap_functions.py', 'gen_sdk_handlers.py'):
    subprocess.run([sys.executable, str(ROOT / 'scripts' / script)], cwd=ROOT, check=True)
print('Game code generated; now run build-native.cmd')
