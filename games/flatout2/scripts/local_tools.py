"""Where the tools of this port are: this clone first, then the author's shared workspace."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Inside the PS2Recomp repository (games/<game>) the runtime is that repository; else the port's submodule.
INSIDE_RUNTIME = ROOT.parents[1] if (ROOT.parents[1] / 'ps2xRuntime').is_dir() else ROOT / 'third_party/PS2Recomp'


def recompiler():
    """ps2_recomp from PS2_RECOMP_EXE, build-recompiler.cmd or the sibling Call of Duty build."""
    if os.environ.get('PS2_RECOMP_EXE'):
        return Path(os.environ['PS2_RECOMP_EXE'])
    for folder in (ROOT / 'build/recompiler/ps2xRecomp', ROOT.parent / 'codwaw-ff/build/ps2recomp/ps2xRecomp'):
        for name in ('ps2_recomp.exe', 'ps2_recomp'):
            if (folder / name).is_file():
                return folder / name
    raise SystemExit('ps2_recomp not found: run build-recompiler.cmd or set PS2_RECOMP_EXE')


def analyzer():
    """ps2_analyzer from build-recompiler.cmd or the sibling Call of Duty build."""
    for folder in (ROOT / 'build/recompiler/ps2xAnalyzer', ROOT.parent / 'codwaw-ff/build/ps2recomp/ps2xAnalyzer'):
        for name in ('ps2_analyzer.exe', 'ps2_analyzer'):
            if (folder / name).is_file():
                return folder / name
    raise SystemExit('ps2_analyzer not found: run build-recompiler.cmd')


def runtime_file(relative):
    """A file of the PS2Recomp runtime: the submodule when checked out, else ../third_party/PS2Recomp."""
    inside = INSIDE_RUNTIME / relative
    return inside if inside.is_file() else ROOT.parent / 'third_party/PS2Recomp' / relative
