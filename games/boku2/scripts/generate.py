"""Generate Boku2 AOT sources and its evidence-backed supplemental entry."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from local_tools import recompiler

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = 'ecdac5741e092099e08834ea16b785cdfd70bc750855caa6c4dd28fc689cb6b1'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recovered-only', action='store_true')
    args = parser.parse_args()
    if hashlib.sha256((ROOT / 'game_files/SCPS_150.26').read_bytes()).hexdigest() != EXPECTED:
        raise RuntimeError('Unexpected Boku2 ELF identity')
    compiler = recompiler()
    (ROOT / 'analysis').mkdir(exist_ok=True)
    configs = ['recovered'] if args.recovered_only else ['recomp', 'recovered']
    for name in configs:
        with (ROOT / f'analysis/{name}-generation.log').open('w') as log:
            subprocess.run([str(compiler), str(ROOT / f'config/{name}.toml')],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    register = (ROOT / 'project/recovered/register_functions.cpp').read_text()
    entries = re.findall(r'= Boku2BootMain_0x20a920; // (0x[0-9a-fA-F]+)', register)
    if not entries or entries[0] != '0x20a920':
        raise RuntimeError('Supplemental entry missing from generated table')
    source = '''#define main Boku2SharedMain
#include "../../../ps2xRuntime/src/main.cpp"
#undef main
#include "recovered/Boku2BootMain_0x20a920.cpp"
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
int main(int argc, char** argv) {
'''
    for address in entries:
        slot = f'({address}u-g_ps2RecompiledFunctionTableBase)/4'
        source += f'    if ({slot} >= g_ps2RecompiledFunctionTableSlotCount || g_ps2RecompiledFunctionTable[{slot}]) return 2;\n'
        source += f'    g_ps2RecompiledFunctionTable[{slot}] = Boku2BootMain_0x20a920;\n'
    source += '    return Boku2SharedMain(argc, argv);\n}\n'
    output = ROOT / 'project/main.cpp'
    if output.read_text() != source:
        output.write_text(source)
    if not args.recovered_only:
        runner = ROOT / 'project/runner'
        (runner / '.generation-complete.json').write_text(json.dumps(dict(
            config_sha256=hashlib.sha256((ROOT/'config/recomp.toml').read_bytes()).hexdigest(),
            cpp_files=len(list(runner.glob('*.cpp')))), indent=2)+'\n')
        subprocess.run([sys.executable, str(ROOT/'scripts/make_chunks.py')], check=True)
    (ROOT / 'analysis/recovered-entry.json').write_text(json.dumps(dict(
        elfSha256=EXPECTED, start='0x20a920', endExclusive='0x20a998',
        directJumpSource='0x20aa9c', registeredEntries=entries,
        method='Local ELF disassembly and PS2Recomp translation',
        limitations=['Static reconstruction; no gameplay validation.']), indent=2)+'\n')
    print(f'Generated supplemental entry with {len(entries)} dispatch addresses')

if __name__ == '__main__':
    main()
