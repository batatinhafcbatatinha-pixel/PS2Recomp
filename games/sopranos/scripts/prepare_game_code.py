"""Generate all the game code from your own files, after ghidra-export.cmd and build-recompiler.cmd."""
import subprocess
import sys

from local_tools import ROOT, analyzer, game_files

(ROOT / 'analysis').mkdir(exist_ok=True)
with (ROOT / 'analysis/analyzer.log').open('w') as log:
    subprocess.run([str(analyzer()), str(game_files() / 'SLUS_213.88'), 'config/analyzer-heuristic.toml'], cwd=ROOT, stdout=log,
                   stderr=subprocess.STDOUT, check=True)
for script in ('prepare_config.py', 'guarded_generate.py', 'make_chunks.py', 'gen_gap_functions.py', 'gen_sdk_handlers.py',
               'route_sdk_tailcalls.py'):
    subprocess.run([sys.executable, str(ROOT / 'scripts' / script)], cwd=ROOT, check=True)
print('Game code generated; now run build-native.cmd')
