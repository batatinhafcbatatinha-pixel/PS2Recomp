"""Generate all the game code from your own disc, after ghidra-export.cmd and build-recompiler.cmd (--disc: step 1 only).

Run by Prepare-Build.cmd (and Setup.cmd). Everything it writes is private and stays on your PC:
  1. analysis/disc: SLUS_217.46 and MISC/MPEG_DLL.ASR from game_files (checked by SHA-256).
  2. Main translation: config merge (Ghidra + SCE stubs) -> ps2_recomp -> SQRT.S fix -> chunks -> optimizer.
  3. The supplements the main function map misses: five single functions, every .text range of SLUS_217.46 that
     no function covers (scripts/find_code_gaps.py, find_unregistered_targets.py), and the MPEG module the game
     loads from MISC/MPEG_DLL.ASR, relocated the way the game's SNR2 loader does (scripts/snr2_load.py). Their
     chunks go into the PS2Recomp runtime. Nothing here is taken from a capture of a running game.
Native VU1 blocks (scripts/vu1n_gen.py) need captured microcode and are left out: the exact VU1 engine runs instead.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'scripts'
DISC = ROOT / 'analysis/disc'
# Code ranges the main function map misses (found while bringing the game up; see NOTES.md).
FUNCTIONS = {'function-139fb0': (0x139fb0, 0x139fe0), 'function-159fc8': (0x159fc8, 0x15a220),
             'function-1c90b0': (0x1c90b0, 0x1c93b8), 'function-4496b8': (0x4496b8, 0x449728),
             'function-gap': (0x1c93f0, 0x1c9688)}
# The MPEG module: load address the supplements are generated for (the runtime rebinds them at any base),
# instruction range, the constructor and the first extra entry.
MPEG = {'file': 'MISC/MPEG_DLL.ASR', 'base': 0x9e7480, 'code': (0x9e7500, 0x9f6740), 'entry': 0x9e7500,
        'ctor': (0x9e9688, 0x9e96a8), 'extra_entry': 0x9e7640}
ELF_SHA256 = json.loads((ROOT / 'installer.json').read_text(encoding='utf-8'))['required_files']['SLUS_217.46']


def ps2recomp_dir():
    """The runtime the build uses: the submodule when checked out, else the author's shared checkout."""
    inside = ROOT.parents[1]
    return inside if (inside / 'CMakeLists.txt').is_file() else ROOT.parents[1]


def tool(name, folder):
    for build in ('build/recompiler', 'build/ps2recomp'):
        for exe in (name + '.exe', name):
            if (ROOT / build / folder / exe).is_file():
                return ROOT / build / folder / exe
    raise SystemExit(f'{name} not found: run build-recompiler.cmd first')


def run(args, log=None):
    print('>', ' '.join(str(a) for a in args), flush=True)
    if log:
        with (ROOT / 'analysis' / log).open('w', encoding='utf-8', errors='replace') as out:
            subprocess.run([str(a) for a in args], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def py(script, *args, log=None):
    run([sys.executable, SCRIPTS / script, *args], log)


def disc():
    DISC.joinpath('MISC').mkdir(parents=True, exist_ok=True)
    for name in ('SLUS_217.46', 'MISC/MPEG_DLL.ASR'):
        source, target = ROOT / 'game_files' / name, DISC / name
        if not source.is_file():
            raise SystemExit(f'game_files/{name} missing: install your disc with Install-Files.cmd')
        if not target.is_file() or target.stat().st_size != source.stat().st_size:
            shutil.copy2(source, target)
    if hashlib.sha256((DISC / 'SLUS_217.46').read_bytes()).hexdigest() != ELF_SHA256:
        raise SystemExit('SLUS_217.46 is not the supported USA/Canada executable')


def main_code():
    if not (ROOT / 'config/ghidra-functions.csv').is_file():
        raise SystemExit('config/ghidra-functions.csv missing: run ghidra-export.cmd first')
    runner = ROOT / 'project/runner'
    if (runner / 'register_functions.cpp').is_file() and (ROOT / 'project/runner-chunks.orig').is_dir():
        print('Main translation already done (delete project/runner to redo it)')
        return
    config = ROOT / 'analysis/recomp.toml'
    sys.path.insert(0, str(SCRIPTS))
    __import__('merge_config').main(str(ROOT / 'config/codwaw-ff.toml'), str(ROOT / 'config/analyzer-heuristic.toml'),
                                    str(config), runner.as_posix())
    # The Ghidra export records where it imported the executable; translate the verified copy.
    text = config.read_text(encoding='utf-8').splitlines()
    text = [f'input = "{(DISC / "SLUS_217.46").as_posix()}"' if line.startswith('input = ') else line for line in text]
    config.write_text('\n'.join(text) + '\n', encoding='utf-8')
    for folder in ('project/runner', 'project/runner-opt', 'project/runner-chunks', 'project/runner-chunks.orig'):
        shutil.rmtree(ROOT / folder, ignore_errors=True)
    run([tool('ps2_recomp', 'ps2xRecomp'), config], 'recomp.log')
    py('fix_sqrt_operand.py', runner)
    py('make_chunks.py', runner, ROOT / 'project/runner-chunks')
    run([sys.executable, '-I', SCRIPTS / 'optimize_runner.py', '--apply'], 'optimize-runner.log')


def relocated_module():
    """The MPEG module as the game's SNR2 loader leaves it at MPEG['base'], from your own disc files."""
    sys.path.insert(0, str(SCRIPTS))
    loader = __import__('snr2_load')
    image = loader.load((DISC / MPEG['file']).read_bytes(), (DISC / 'SLUS_217.46').read_bytes(), MPEG['base'])
    path = ROOT / 'analysis/mpeg-module.bin'
    path.write_bytes(image)
    return path


def analysis_elfs():
    elf = DISC / 'SLUS_217.46'
    for name, (start, end) in FUNCTIONS.items():
        py('make_function_elf.py', elf, ROOT / f'analysis/{name}.elf', hex(start), hex(end))
    # Uncovered .text ranges, in the two passes the supplement was made with: first without the referenced
    # targets, which are then searched against that function list and the main translation.
    py('find_code_gaps.py')
    (ROOT / 'analysis/unregistered-targets.json').write_text('{}', encoding='utf-8')
    py('make_gaps_elf.py', ROOT / 'analysis/code-gaps.elf', ROOT / 'config/gaps-functions.csv')
    py('find_unregistered_targets.py')
    py('make_gaps_elf.py', ROOT / 'analysis/code-gaps.elf', ROOT / 'config/gaps-functions.csv')
    module, base = relocated_module(), hex(MPEG['base'])
    code = [hex(a) for a in MPEG['code']]
    py('make_function_elf.py', module, ROOT / 'analysis/mpeg-relocated.elf', *code, base, hex(MPEG['entry']))
    py('find_overlay_entries.py')
    py('make_function_elf.py', module, ROOT / 'analysis/mpeg-extra.elf', *code, base, hex(MPEG['extra_entry']))
    ctor = MPEG['ctor']
    py('make_function_elf.py', module, ROOT / 'analysis/mpeg-ctor.elf', hex(ctor[0]), hex(ctor[1]), base, hex(ctor[0]))
    return module, base


# (config, registration helper or None, uses the MPEG capture, relocated, runtime chunk prefix or None)
SUPPLEMENTS = [
    ('supplemental', None, False, False, None),
    ('supplemental-1c90b0', None, False, False, None),
    ('supplemental-4496b8', None, False, False, None),
    ('supplemental-139fb0', 'register139fb0Supplement', False, False, None),
    ('supplemental-159fc8', 'register159fc8Supplement', False, False, None),
    ('supplemental-gaps', 'registerCodeGapsSupplement', False, False, 'codwaw_gaps_chunk_'),
    ('supplemental-mpeg-ctor', 'registerMpegCtorSupplement', True, True, None),
    ('supplemental-mpeg', 'registerMpegSupplement', True, True, 'codwaw_mpeg_chunk_'),
    ('supplemental-mpeg-extra', 'registerMpegExtraSupplement', True, True, 'codwaw_mpeg_extra_chunk_'),
]


def supplements():
    module, base = analysis_elfs()
    lib = ps2recomp_dir() / 'ps2xRuntime/src/lib'
    for old in lib.glob('codwaw_*_chunk_*.cpp'):
        old.unlink()
    recompiler = tool('ps2_recomp', 'ps2xRecomp')
    for name, helper, capture, relocate, prefix in SUPPLEMENTS:
        folder = ROOT / 'project' / name
        shutil.rmtree(folder, ignore_errors=True)
        run([recompiler, ROOT / f'config/{name}.toml'], f'recomp-{name}.log')
        py('fix_sqrt_operand.py', folder)
        if helper:
            py('integrate_supplemental.py', folder, helper, *((module, base) if capture else ()))
        if relocate:
            py('relocate_supplemental.py', folder)
        if prefix:
            py('split_supplemental.py', folder, lib, prefix)
    print('Supplements ready; runtime chunks in', lib)


def main():
    disc()
    if '--disc' in sys.argv:  # only the verified copies (ghidra-export.cmd imports analysis/disc/SLUS_217.46)
        return
    main_code()
    supplements()
    print('Game code generated on this PC. Do not upload project/runner*, project/supplemental*, analysis/ or the build.')


if __name__ == '__main__':
    main()
