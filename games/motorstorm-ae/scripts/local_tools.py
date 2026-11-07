"""Where the tools of this port are: this clone first, then the author's shared workspace."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Inside the PS2Recomp repository (games/<game>) the runtime is that repository; else the port's submodule.
INSIDE_RUNTIME = ROOT.parents[1] if (ROOT.parents[1] / 'ps2xRuntime').is_dir() else ROOT / 'third_party/PS2Recomp'


def build_location():
    """The heavy build folder recorded in build-location.txt."""
    location = ROOT / 'build-location.txt'
    if not location.is_file():  # a fresh clone builds inside itself
        return ROOT
    return Path(location.read_text(encoding='utf-8').strip())


def recompiler():
    """ps2_recomp from PS2_RECOMP_EXE, this port's build, or the sibling Call of Duty build."""
    if os.environ.get('PS2_RECOMP_EXE'):
        return Path(os.environ['PS2_RECOMP_EXE'])
    for folder in (ROOT / 'recompiler/ps2xRecomp', build_location() / 'recompiler/ps2xRecomp', ROOT / 'build/recompiler/ps2xRecomp',
                   ROOT.parent / 'codwaw-ff/build/ps2recomp/ps2xRecomp'):
        for name in ('ps2_recomp.exe', 'ps2_recomp'):
            if (folder / name).is_file():
                return folder / name
    raise SystemExit('ps2_recomp not found: run build-recompiler.cmd or set PS2_RECOMP_EXE')


def analyzer():
    """ps2_analyzer from this port's build, the build folder, or the sibling Call of Duty build."""
    for folder in (ROOT / 'recompiler/ps2xAnalyzer', build_location() / 'recompiler/ps2xAnalyzer', ROOT / 'build/recompiler/ps2xAnalyzer',
                   ROOT.parent / 'codwaw-ff/build/ps2recomp/ps2xAnalyzer'):
        for name in ('ps2_analyzer.exe', 'ps2_analyzer'):
            if (folder / name).is_file():
                return folder / name
    raise SystemExit('ps2_analyzer not found: run build-recompiler.cmd')


def game_files():
    """Extracted game data (never versioned)."""
    return build_location() / 'game_files'


def runtime_dir():
    """The PS2Recomp runtime source: this port's submodule when checked out, else the shared checkout."""
    inside = INSIDE_RUNTIME
    return inside if (inside / 'CMakeLists.txt').is_file() else ROOT.parent / 'third_party/PS2Recomp'


def runtime_file(relative):
    """A file of the PS2Recomp runtime: the submodule when checked out, else ../third_party/PS2Recomp."""
    return runtime_dir() / relative
