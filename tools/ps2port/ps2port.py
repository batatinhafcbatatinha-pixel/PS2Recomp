#!/usr/bin/env python3
"""ps2port: build a native PC port of any PlayStation 2 game from its disc files, with the same steps for every game.

usage: python tools/ps2port/ps2port.py <step> [--port DIR] [--jobs N] [--force]

A port folder holds game_files/ (the disc, from the launcher's installer) and optionally port.toml (a game's
overrides). Everything generated goes to <port>/build, observations from runs to <port>/observed.
Steps (each skips itself when its output is newer than its inputs; --force redoes it):
  tools     build ps2_recomp / ps2_analyzer once for the whole repository (.local/recompiler)
  ghidra    headless Ghidra analysis of the boot executable: function map, labels, kernel wrappers
  analyze   PS2Recomp analyzer: SDK library matches and extra function starts
  config    recompiler configuration from the Ghidra map
  generate  translate the executable to C++ (build/runner)
  gaps      translate functions the map left out, incl. the ones a run reported missing (observed/missing.txt)
  sdk       bind SDK functions to the runtime's native handlers (analyzer + signatures + port.toml)
  finish    route bound tail calls through the table, bound big dispatch switches, group sources in chunks
  iop       recompile the disc's IOP modules (IRX, also inside IOPRP images)
  vu1       compile the VU1 microprograms a run captured (observed/vu1) to native code
  compile   compile and link build/native/.../ps2EntryRunner.exe
  prepare   tools ghidra analyze config generate gaps sdk finish iop vu1   (everything before compile)
  all       prepare + compile
  play      run the game (records missing functions and VU1 programs for the next build)
"""
import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import elfinfo  # noqa: E402
import gaps as gapmod  # noqa: E402
import sdk as sdkmod  # noqa: E402

RECOMPILER = REPO / '.local/recompiler'


class Port:
    def __init__(self, folder, work=None):
        self.dir = Path(folder).resolve()
        self.files = self.dir / 'game_files'
        # Generated files and observations: in the port folder, or in --work (game files left where they are).
        work = Path(work).resolve() if work else self.dir
        self.build = work / 'build'
        self.observed = work / 'observed'
        self.cfg = self.build / 'config'
        self.log = self.build / 'analysis'
        self.gen = self.build / 'port-gen'
        t = self.dir / 'port.toml'
        self.toml = tomllib.loads(t.read_text(encoding='utf-8')) if t.is_file() else {}
        for d in (self.build, self.cfg, self.log, self.gen):
            d.mkdir(parents=True, exist_ok=True)

    @property
    def elf_name(self):
        if self.toml.get('elf'):
            return self.toml['elf']
        cnf = self.files / 'SYSTEM.CNF'
        if not cnf.is_file():
            raise SystemExit(f'{cnf} missing: install the game files first')
        m = re.search(r'BOOT2?\s*=\s*cdrom0?:\\?([^;\s]+)', cnf.read_text(errors='replace'), re.I)
        if not m:
            raise SystemExit('SYSTEM.CNF names no boot executable')
        return m.group(1).replace('\\', '/').split('/')[-1]

    @property
    def elf_path(self):
        return self.files / self.elf_name

    def rel(self, p):
        # Not resolved: game_files may be a junction to another drive.
        try:
            return Path(os.path.relpath(os.path.abspath(p), self.dir)).as_posix()
        except ValueError:  # another drive
            return Path(os.path.abspath(p)).as_posix()


def log(msg):
    print(msg, flush=True)


def newer(target, *sources):
    target = Path(target)
    if not target.exists():
        return False
    t = target.stat().st_mtime
    return all(not Path(s).exists() or Path(s).stat().st_mtime <= t for s in sources)


def tool(name):
    for sub in ('ps2xRecomp', 'ps2xAnalyzer'):
        for exe in (name + '.exe', name):
            p = RECOMPILER / sub / exe
            if p.is_file():
                return p
    raise SystemExit(f'{name} not built: run the "tools" step')


def vc_env():
    """The Visual C++ environment (when cl is not already on PATH)."""
    if os.name != 'nt' or shutil.which('cl'):
        return None
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        return None
    path = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                           'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                          capture_output=True, text=True).stdout.strip()
    vcvars = Path(path) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not vcvars.is_file():
        return None
    out = subprocess.run(f'"{vcvars}" >nul && set', shell=True, capture_output=True, text=True).stdout
    # Windows names are case-insensitive: cmd prints "Path=" when the parent spelt it so (Explorer, PowerShell, the
    # launcher), which must replace "PATH", not sit next to it.
    env = {k.upper(): v for k, v in os.environ.items()}
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k.upper()] = v
    env['VSLANG'] = '1033'
    return env


def tool_path_env(env=None):
    """env (or os.environ) with the repository's portable tools (.local/tools: CMake, Ninja) first on PATH."""
    env = dict(env or os.environ)
    front = [p for p in (REPO / '.local/tools/cmake-3.31.12/bin', REPO / '.local/tools/ninja-1.13.2') if p.is_dir()]
    if front:
        env['PATH'] = os.pathsep.join(map(str, front)) + os.pathsep + env.get('PATH', '')
    return env


def run(cmd, cwd=None, logfile=None, env=None):
    env = tool_path_env(env)
    cmd = [str(c) for c in cmd]
    exe = shutil.which(cmd[0], path=env.get('PATH'))
    if exe:
        cmd[0] = exe
    log('> ' + ' '.join(cmd))
    if logfile:
        with open(logfile, 'w', encoding='utf-8', errors='replace') as out:
            r = subprocess.run(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, env=env, stdin=subprocess.DEVNULL)
    else:
        r = subprocess.run(cmd, cwd=cwd, env=env)
    if r.returncode:
        raise SystemExit(f'failed ({r.returncode}): {cmd[0]}' + (f' - see {logfile}' if logfile else ''))


# ------------------------------------------------------------------------------------------------ steps

def step_tools(port, args):
    if not args.force and all((RECOMPILER / s / (n + ('.exe' if os.name == 'nt' else ''))).is_file()
                              for s, n in (('ps2xRecomp', 'ps2_recomp'), ('ps2xAnalyzer', 'ps2_analyzer'))):
        log('tools: already built')
        return
    env = vc_env()
    run(['cmake', '-S', REPO, '-B', RECOMPILER, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DPS2X_BUILD_RUNTIME=OFF',
         '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF'], env=env)
    run(['cmake', '--build', RECOMPILER, '--target', 'ps2_recomp', 'ps2_analyzer'], env=env)


def ghidra_home():
    for c in (os.environ.get('GHIDRA'), REPO / '.local/tools/ghidra-12.1.4'):
        if c and (Path(c) / 'support').is_dir():
            return Path(c)
    raise SystemExit('Ghidra not found: set GHIDRA or let the launcher install it (.local/tools/ghidra-12.1.4)')


def step_ghidra(port, args):
    out = port.cfg / 'ghidra-functions.csv'
    if not args.force and newer(out, port.elf_path):
        log('ghidra: up to date')
        return
    home = ghidra_home()
    # Ghidra refuses project paths with a folder starting with '.' (ports live in .local/ports): the temporary
    # project goes to the system temp folder and is deleted after the export.
    import tempfile
    project = Path(tempfile.gettempdir()) / 'ps2port-ghidra' / hashlib.sha1(str(port.dir).encode()).hexdigest()[:12]
    shutil.rmtree(project, ignore_errors=True)
    project.mkdir(parents=True)
    headless = home / 'support' / ('analyzeHeadless.bat' if os.name == 'nt' else 'analyzeHeadless')
    scripts = REPO / 'ps2xRecomp/tools/ghidra'  # one folder: the .bat launcher splits arguments at ';'
    run([headless, project, 'port', '-import', port.elf_path, '-overwrite', '-processor', 'r5900:LE:32:default',
         '-analysisTimeoutPerFile', '7200', '-scriptPath', scripts,
         '-postScript', 'ExportPS2Functions.java', port.cfg / 'ghidra.toml', port.cfg / 'ghidra-labels.csv',
         '-postScript', 'ExportFunctionsOnly.java', out], logfile=port.log / 'ghidra.log')
    shutil.rmtree(project, ignore_errors=True)
    if not out.is_file():
        raise SystemExit('Ghidra exported nothing; see build/analysis/ghidra.log')


def step_analyze(port, args):
    out = port.cfg / 'analyzer.toml'
    if not args.force and newer(out, port.elf_path):
        log('analyze: up to date')
        return
    run([tool('ps2_analyzer'), port.elf_path, out], cwd=port.dir, logfile=port.log / 'analyzer.log')


def ghidra_rows(port):
    rows = sorted((int(r['Start'], 16), int(r['End'], 16), r['Name'])
                  for r in csv.DictReader((port.cfg / 'ghidra-functions.csv').open()))
    # Ghidra's End is the highest address of the body; a body with a far piece spans other functions: cap it.
    capped = []
    for i, (s, e, n) in enumerate(rows):
        if i + 1 < len(rows) and e > rows[i + 1][0]:
            e = rows[i + 1][0]
        capped.append((s, e, n))
    return capped


def step_config(port, args):
    rows = ghidra_rows(port)
    if not rows:
        raise SystemExit('Empty Ghidra function map')
    with (port.cfg / 'ghidra-functions-capped.csv').open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Name', 'Start', 'End', 'Size'])
        for s, e, n in rows:
            w.writerow([n, f'0x{s:08X}', f'0x{e:08X}', e - s])
    stubs = tomllib.loads((port.cfg / 'ghidra.toml').read_text())['general'].get('stubs', [])
    q = json.dumps
    lines = ['# Generated by tools/ps2port; do not edit.', '[general]',
             f'input = {q(port.rel(port.elf_path))}', f'output = {q(port.rel(port.build / "runner"))}',
             f'ghidra_output = {q(port.rel(port.cfg / "ghidra-functions-capped.csv"))}',
             'single_file_output = false', 'low_memory_mode = true', 'output_worker_threads = 1',
             'patch_syscalls = false', 'patch_cop0 = true', 'patch_cache = true', 'stubs = [']
    lines += [f'  {q(s)},' for s in stubs] + [']', 'skip = []']
    (port.cfg / 'recomp.toml').write_text('\n'.join(lines) + '\n')
    log(f'config: {len(rows)} functions, {len(stubs)} kernel wrappers bound')


def generate_into(port, config, out, limit_gb=8):
    """Run ps2_recomp on config into a scratch folder, then replace only changed files in out."""
    scratch = out.with_name(out.name + '.tmp')
    shutil.rmtree(scratch, ignore_errors=True)
    text = config.read_text()
    tmp_config = config.with_name(config.stem + '.tmp.toml')
    tmp_config.write_text(text.replace(f'output = "{port.rel(out)}"', f'output = "{port.rel(scratch)}"'))
    logf = port.log / f'{out.name}-generate.log'
    with logf.open('w') as stream:
        proc = subprocess.Popen([str(tool('ps2_recomp')), port.rel(tmp_config)], cwd=port.dir, stdout=stream,
                                stderr=subprocess.STDOUT)
        try:
            while proc.poll() is None:
                size = sum(p.stat().st_size for p in scratch.rglob('*') if p.is_file()) if scratch.exists() else 0
                if size > limit_gb * 1024 ** 3:
                    proc.terminate()
                    raise SystemExit(f'generation stopped at the {limit_gb} GB limit')
                time.sleep(2)
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait()
    tmp_config.unlink(missing_ok=True)
    if proc.returncode or not (scratch / 'register_functions.cpp').is_file():
        raise SystemExit(f'ps2_recomp failed; see {logf}')
    out.mkdir(parents=True, exist_ok=True)
    new = {p.name for p in scratch.iterdir() if p.is_file()}
    changed = 0
    for name in new:
        s, t = scratch / name, out / name
        if not t.is_file() or t.stat().st_size != s.stat().st_size or t.read_bytes() != s.read_bytes():
            shutil.copy2(s, t)
            changed += 1
    for p in out.iterdir():
        if p.is_file() and p.name not in new and not p.name.startswith('.'):
            p.unlink()
    shutil.rmtree(scratch)
    return len(new), changed


def step_generate(port, args):
    out = port.build / 'runner'
    stamp = out / '.complete'
    if not args.force and newer(stamp, port.cfg / 'recomp.toml', port.elf_path):
        log('generate: up to date')
        return
    stamp.unlink(missing_ok=True)
    n, changed = generate_into(port, port.cfg / 'recomp.toml', out)
    stamp.write_text(str(n))
    log(f'generate: {n} files, {changed} changed')


def observed_starts(port):
    """In-image function starts a run reported missing (observed/missing.txt: one hex address per line)."""
    f = port.observed / 'missing.txt'
    if not f.is_file():
        return set()
    return {int(x.split()[0], 16) for x in f.read_text().splitlines() if x.strip() and not x.startswith('#')}


def step_gaps(port, args):
    elf = elfinfo.Elf.load(port.elf_path)
    rows = [(s, e) for s, e, _ in ghidra_rows(port)]
    labels = {int(r['Start'], 16) for r in csv.DictReader((port.cfg / 'ghidra-labels.csv').open())
              if r['Name'].startswith('entry_')} if (port.cfg / 'ghidra-labels.csv').is_file() else set()
    extra = observed_starts(port) | {int(str(a), 0) for a in port.toml.get('gaps', {}).get('starts', [])}
    analyzer_log = (port.log / 'analyzer.log').read_text(errors='replace') if (port.log / 'analyzer.log').is_file() else ''
    found = gapmod.find(elf, rows, labels, analyzer_log, extra)
    csv_path = port.cfg / 'gap-functions.csv'
    gapmod.write_csv(csv_path, elf, rows, found)
    out = port.build / 'gap'
    toml_path = port.cfg / 'gap-functions.toml'
    q = json.dumps
    toml_path.write_text('\n'.join(['[general]', f'input = {q(port.rel(port.elf_path))}', f'output = {q(port.rel(port.build / "gap"))}',
                                    f'ghidra_output = {q(port.rel(port.cfg / "gap-functions.csv"))}', 'single_file_output = false',
                                    'patch_syscalls = false', 'patch_cop0 = true', 'patch_cache = true', 'stubs = []',
                                    'skip = []']) + '\n')
    generate_into(port, toml_path, out)
    table = (out / 'register_functions.cpp').read_text()
    entries = re.findall(r'= (gap_[0-9a-f]+_0x[0-9a-f]+); // (0x[0-9a-f]+)', table)
    funcs = sorted({fn for fn, _ in entries})
    decl = ''.join(f'void {fn}(uint8_t*, R5900Context*, PS2Runtime*);\n' for fn in funcs)
    (out / 'gap-declarations.h').write_text('#pragma once\n#include <cstdint>\nstruct R5900Context;\nclass PS2Runtime;\n' + decl)
    (port.gen / 'gap-functions.inc').write_text('// Generated by tools/ps2port; do not edit.\n' + decl +
        'static const PortTableEntry kPortGapFunctions[] = {\n' +
        ''.join(f'    {{{addr}u, {fn}}},\n' for fn, addr in entries) + '    {0u, nullptr}\n};\n')
    log(f'gaps: {len(found)} functions ({len(extra)} observed/profile starts), {len(entries)} table entries')


def step_sdk(port, args):
    elf = elfinfo.Elf.load(port.elf_path)
    analyzer = tomllib.loads((port.cfg / 'analyzer.toml').read_text())['general'].get('stubs', [])
    known = {}
    for m in analyzer:
        n, a = m.split('@')
        known.setdefault(n, int(a, 16))
    by_sig = sdkmod.find_by_signature(elf, known)
    ghidra_wrappers = {int(s.split('@')[1], 16) for s in
                       tomllib.loads((port.cfg / 'ghidra.toml').read_text())['general'].get('stubs', []) if '@' in s}
    matches = [m for m in analyzer if int(m.split('@')[1], 16) not in ghidra_wrappers]
    matches += [f'{n}@0x{a:x}' for n, a in by_sig.items() if a not in ghidra_wrappers]
    prof = port.toml.get('sdk', {})
    wrappers, entries, skipped = sdkmod.bindings(REPO, matches, prof.get('exclude', []), prof.get('extra', []))
    (port.gen / 'sdk-handlers.inc').write_text('// Generated by tools/ps2port; do not edit.\n' + '\n'.join(wrappers) +
        '\nstatic const PortTableEntry kPortSdkHandlers[] = {\n' +
        ''.join(f'    {{0x{a:x}u, {fn}}},\n' for a, fn in entries) + '    {0u, nullptr}\n};\n')
    (port.log / 'sdk.json').write_text(json.dumps({'analyzer': len(analyzer), 'signatures': by_sig,
                                                   'bound': len(entries), 'skipped': skipped}, indent=1))
    log(f'sdk: {len(entries)} bound ({len(by_sig)} found by signature), {len(skipped)} left to the game code')


def profile_handlers(port):
    h = port.toml.get('profile', {}).get('handlers')
    return (port.dir / h) if h else None


def step_finish(port, args):
    # 1. Direct calls into natively bound functions go through the table (else the handler is bypassed).
    bound = {int(a, 16) for a in re.findall(r'\{0x([0-9a-fA-F]+)u, \w+', (port.gen / 'sdk-handlers.inc').read_text())}
    ph = profile_handlers(port)
    if ph and ph.is_file():
        bound |= {int(a, 16) for a in re.findall(r'\{0x([0-9a-fA-F]+)u,', ph.read_text())}
    folders = [port.build / 'runner', port.build / 'gap']
    names = {}
    for folder in folders:
        for p in folder.glob('*.cpp'):
            m = re.match(r'(.+)_0x([0-9a-f]+)$', p.stem)
            if m and int(m.group(2), 16) in bound:
                names[p.stem] = int(m.group(2), 16)
    routed = 0
    if names:
        pat = re.compile(r'\b(' + '|'.join(map(re.escape, sorted(names, key=len, reverse=True))) + r')\(rdram, ctx, runtime\)')
        for folder in folders:
            for p in sorted(folder.glob('*.cpp')):
                text = p.read_text(encoding='utf-8', errors='surrogateescape')
                if not pat.search(text):
                    continue
                new = pat.sub(lambda m: m.group(0) if m.group(1) == p.stem else
                              f'runtime->lookupFunction(0x{names[m.group(1)]:x}u)(rdram, ctx, runtime)', text)
                if new != text:
                    p.write_text(new, encoding='utf-8', errors='surrogateescape')
                    routed += 1
    # 2. Very large resumable dispatch switches: two-level switch (same PC -> label mapping) to bound compile cost.
    bucketed = bucket_dispatch(port.build / 'runner')
    # 3. Chunk files that include the generated sources (CMake handles a few hundred units, not 80,000).
    nchunks = make_chunks(port.build / 'runner', port.build / 'chunks')
    log(f'finish: {routed} files routed through the table, {bucketed} switches bucketed, {nchunks} chunks')


def bucket_dispatch(runner):
    pattern = re.compile(r'    switch \(ctx->pc\) \{\n(?P<cases>(?:        case 0x[0-9a-f]+u: goto label_[0-9a-f]+;\n)+)'
                         r'        default: break;\n    \}')
    count = 0
    for source in runner.glob('*.cpp'):
        if source.stat().st_size < 4 * 1024 ** 2:
            continue
        text = source.read_text(encoding='utf-8')
        if 'BUCKETED_NATIVE_ENTRY' in text:
            continue
        match = pattern.search(text)
        if not match:
            continue
        pairs = re.findall(r'case (0x[0-9a-f]+)u: goto (label_[0-9a-f]+);', match['cases'])
        if len(pairs) < 16000:
            continue
        groups = {}
        for address, label in pairs:
            groups.setdefault(int(address, 16) >> 10, []).append((address, label))
        lines = ['    { // PORT_BUCKETED_NATIVE_ENTRY: preserves all resumable PCs',
                 '        const uint32_t port_resume_pc = ctx->pc;', '        switch (port_resume_pc >> 10u) {']
        for bucket, entries in groups.items():
            lines += [f'        case 0x{bucket:x}u:', '            switch (port_resume_pc) {']
            lines += [f'            case {a}u: goto {l};' for a, l in entries]
            lines += ['            default: break;', '            }', '            break;']
        lines += ['        default: break;', '        }', '    }']
        replacement = '\n'.join(lines)
        if re.findall(r'case (0x[0-9a-f]+)u: goto (label_[0-9a-f]+);', replacement) != pairs:
            raise SystemExit('dispatch bucketing changed the PC mapping')
        source.write_text(text[:match.start()] + replacement + text[match.end():], encoding='utf-8')
        count += 1
    return count


def make_chunks(runner, chunks, batch=32):
    names = sorted(n.name for n in runner.glob('*.cpp') if n.name != 'main.cpp')
    if 'register_functions.cpp' not in names:
        raise SystemExit('generated sources missing')
    rest = [n for n in names if n != 'register_functions.cpp']
    groups = [(f'chunk_{i // batch:05d}.cpp', rest[i:i + batch]) for i in range(0, len(rest), batch)]
    groups.append(('chunk_register.cpp', ['register_functions.cpp']))
    chunks.mkdir(parents=True, exist_ok=True)
    rel = os.path.relpath(runner, chunks).replace('\\', '/')
    wanted = set()
    for name, group in groups:
        wanted.add(name)
        text = ''.join(f'#include "{rel}/{n}"\n' for n in group)
        p = chunks / name
        if not p.is_file() or p.read_text(encoding='utf-8') != text:
            p.write_text(text, encoding='utf-8', newline='\n')
    for p in chunks.glob('*.cpp'):
        if p.name not in wanted:
            p.unlink()
    return len(groups)


def step_iop(port, args):
    """Every IOP module on the disc (*.IRX and the modules inside IOPRP images) recompiled statically."""
    import iopimg
    out = port.build / 'iop_aot'
    work = port.build / 'irx'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    seen, files = set(), []
    for f in sorted(port.files.rglob('*')):
        if not f.is_file() or f.name.startswith('.'):
            continue
        blobs = []
        if f.suffix.lower() == '.irx':
            blobs = [(f.stem, f.read_bytes())]
        elif f.stat().st_size < 8 * 1024 ** 2:
            blobs = iopimg.modules(f.read_bytes())
        for name, data in blobs:
            h = hashlib.sha1(data).hexdigest()
            if data[:4] != b'\x7fELF' or h in seen:
                continue
            seen.add(h)
            p = work / f'{name}_{h[:8]}.irx'
            p.write_bytes(data)
            files.append(p)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    if files:
        run([sys.executable, '-I', REPO / 'ps2xIOP/tools/irx_aot.py', out] + files, logfile=port.log / 'iop.log')
    log(f'iop: {len(files)} modules recompiled')


def step_vu1(port, args):
    cap = port.observed / 'vu1'
    out = port.build / 'vu1n'
    if not cap.is_dir() or not any(cap.iterdir()):
        log('vu1: no captured programs yet (they are recorded while the game runs)')
        return
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    run([sys.executable, '-I', HERE / 'vu1n_gen.py', out, cap, '--parts', '6'], logfile=port.log / 'vu1.log')
    log(f'vu1: {len(list(out.glob("vu1n_part*.cpp")))} native parts')


def step_compile(port, args):
    native = port.build / 'native'
    env = vc_env()
    defs = [f'-DPORT_DIR={port.dir.as_posix()}', f'-DPORT_BUILD={port.build.as_posix()}']
    ph = profile_handlers(port)
    if ph and ph.is_file():
        defs.append(f'-DPORT_PROFILE_HANDLERS={ph.as_posix()}')
    if (port.build / 'vu1n').is_dir():
        defs.append(f'-DPS2X_VU1N_DIR={(port.build / "vu1n").as_posix()}')
    env_defaults = port.toml.get('env', {})
    (port.gen / 'port-env.inc').write_text('// Generated by tools/ps2port; do not edit.\nstatic const char* const kPortEnv[][2] = {\n' +
        ''.join(f'    {{{json.dumps(k)}, {json.dumps(str(v))}}},\n' for k, v in env_defaults.items()) + '    {nullptr, nullptr}\n};\n')
    run(['cmake', '-S', HERE / 'project', '-B', native, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'] + defs, env=env)
    if args.configure_only:
        return
    # The generated code needs about 1.6 GB per compiler: jobs follow the free memory, and a failed build (usually
    # memory) is resumed with half the jobs; ninja keeps what was already compiled.
    jobs = args.jobs or max(1, min((os.cpu_count() or 2) - 1, int((free_ram_gb() - 1.5) / 1.6)))
    tries_at_one = 0
    while True:
        try:
            run(['cmake', '--build', native, '--target', 'ps2EntryRunner', '--parallel', str(jobs)], env=env,
                logfile=port.log / 'compile.log')
            break
        except SystemExit:
            if jobs == 1:
                tries_at_one += 1
                if tries_at_one >= 2:
                    raise SystemExit(f'compile failed; see {port.log / "compile.log"}')
            jobs = max(1, jobs // 2)
            log(f'compile stopped; resuming with {jobs} jobs')
            time.sleep(10)
    log('compile: ' + str(native / 'ps2recomp/ps2xRuntime/ps2EntryRunner.exe'))


def free_ram_gb():
    if os.name == 'nt':
        import ctypes

        class Mem(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [(n, ctypes.c_uint64) for n in
                        ('total', 'avail', 'pt', 'pa', 'vt', 'va', 'x')]
        m = Mem()
        m.len = ctypes.sizeof(Mem)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.avail / 1024 ** 3
    try:
        for line in open('/proc/meminfo'):
            if line.startswith('MemAvailable:'):
                return int(line.split()[1]) / 1024 ** 2
    except OSError:
        pass
    return 8.0


def game_exe(port):
    exe = port.build / 'native/ps2recomp/ps2xRuntime' / ('ps2EntryRunner.exe' if os.name == 'nt' else 'ps2EntryRunner')
    if not exe.is_file():
        raise SystemExit('not built yet: run the "all" step')
    return exe


def game_env(port, args):
    """Environment of a run: the game's defaults, observation of missing code, run-time translation."""
    layout = port.files / '.disc/layout.tsv'
    if not layout.is_file():
        inst = Path(args.installer) if args.installer else port.dir / 'installer.json'
        run([sys.executable, REPO / 'tools/port-installer/prepare_folder.py', '--root', port.dir, '--config', inst])
    env = {k.upper(): v for k, v in os.environ.items()} if os.name == 'nt' else dict(os.environ)
    for k, v in port.toml.get('env', {}).items():
        env.setdefault(k, str(v))
    env.setdefault('PS2X_GS', 'hw')
    env.setdefault('PS2X_FPS_LIMIT', '240')
    port.observed.mkdir(exist_ok=True)
    (port.observed / 'vu1').mkdir(exist_ok=True)
    env.setdefault('PS2X_OBSERVE_DIR', str(port.observed))
    # Saves next to the build data (<port>/mc0, or the --work folder), never inside game_files.
    env.setdefault('PS2X_MC_ROOT', str(port.observed.parent / 'mc0'))
    (port.observed.parent / 'mc0').mkdir(exist_ok=True)
    env.setdefault('GOW_VU1_CAPTURA', str(port.observed / 'vu1'))
    # Guest code without a translation is translated and compiled while the game runs (needs the compiler).
    if not args.no_hot:
        vc = vc_env()
        if vc:
            for k, v in vc.items():
                if k.upper() == 'PATH':
                    env[k] = v
                else:
                    env.setdefault(k, v)
        env.setdefault('PS2X_HOTRECOMP', str(HERE / 'hotrecomp.py'))
        env.setdefault('PS2X_HOTRECOMP_PYTHON', sys.executable)
    return env


def step_play(port, args):
    exe = game_exe(port)
    r = subprocess.run([str(exe), str(port.elf_path)], cwd=port.observed.parent, env=game_env(port, args))
    sys.exit(r.returncode)


def step_smoke(port, args):
    """Run the game for --seconds without input; keep a frame every 10 s and report what happened
    (build/smoke/report.json): frames with picture, distinct frames, fps, missing and run-time translated code."""
    exe = game_exe(port)
    out = port.build / 'smoke'
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    env = game_env(port, args)
    frame = out / 'frame.png'
    env['PS2X_FRAME_DUMP'] = str(frame)
    env.setdefault('PS2X_FRAME_DUMP_TICKS', '30')
    logf = (out / 'run.log').open('w', encoding='utf-8', errors='replace')
    proc = subprocess.Popen([str(exe), str(port.elf_path)], cwd=port.observed.parent, env=env, stdout=logf, stderr=subprocess.STDOUT)
    start, shots = time.time(), []
    while time.time() - start < args.seconds and proc.poll() is None:
        time.sleep(1)
        if int(time.time() - start) % 10 == 0 and frame.is_file():
            shot = out / f'shot_{int(time.time() - start):04d}.png'
            if not shot.exists():
                try:
                    shutil.copyfile(frame, shot)
                    shots.append(shot)
                except OSError:
                    pass
    exited = proc.poll()
    if exited is None:
        proc.kill()
        proc.wait()
    logf.close()
    text = (out / 'run.log').read_text(encoding='utf-8', errors='replace')
    report = {'seconds': round(time.time() - start), 'exited_early': exited is not None, 'exit_code': exited,
              'missing_functions': len(set(re.findall(r'No exact recompiled function for guest PC (0x[0-9a-f]+)', text))),
              'hot_loaded': text.count('native code loaded'), 'hot_failed': text.count('translation failed'),
              'errors': sorted(set(re.findall(r'(Error during program execution: [^\n]*|Unhandled[^\n]*|terminate called[^\n]*)', text)))[:10],
              'fps': [int(x) for x in re.findall(r'\[perf\] game_fps=(\d+)', text)][-10:]}
    try:
        from PIL import Image, ImageStat
        lit, digests = 0, set()
        for shot in shots:
            img = Image.open(shot).convert('RGB')
            if max(ImageStat.Stat(img).extrema[c][1] for c in range(3)) > 16:
                lit += 1
            digests.add(hashlib.sha1(img.resize((64, 48)).tobytes()).hexdigest())
        report.update(frames=len(shots), frames_with_picture=lit, distinct_frames=len(digests))
    except ImportError:
        report.update(frames=len(shots))
    (out / 'report.json').write_text(json.dumps(report, indent=1))
    log('smoke: ' + json.dumps(report))


STEPS = {'tools': step_tools, 'ghidra': step_ghidra, 'analyze': step_analyze, 'config': step_config,
         'generate': step_generate, 'gaps': step_gaps, 'sdk': step_sdk, 'finish': step_finish, 'iop': step_iop,
         'vu1': step_vu1, 'compile': step_compile, 'play': step_play, 'smoke': step_smoke}
PREPARE = ['tools', 'ghidra', 'analyze', 'config', 'generate', 'gaps', 'sdk', 'finish', 'iop', 'vu1']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('step', choices=list(STEPS) + ['prepare', 'all'])
    parser.add_argument('--port', default='.', help='port folder (game_files/ inside); default: current folder')
    parser.add_argument('--jobs', type=int, default=0, help='parallel compile jobs (default: from free memory)')
    parser.add_argument('--configure-only', action='store_true', help='compile: configure the build only')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--work', help='folder for build/ and observed/ (default: the port folder)')
    parser.add_argument('--installer', help='installer.json of the disc (default: <port>/installer.json)')
    parser.add_argument('--no-hot', action='store_true', help='play: no run-time translation of missing code')
    parser.add_argument('--seconds', type=int, default=120, help='smoke: how long to run')
    args = parser.parse_args()
    port = Port(args.port, args.work)
    for pre in port.toml.get('profile', {}).get('pre', []) if args.step in ('prepare', 'all') else []:
        run([sys.executable if c == '{python}' else str(port.files) if c == '{files}' else c for c in pre], cwd=port.dir)
    steps = PREPARE if args.step == 'prepare' else PREPARE + ['compile'] if args.step == 'all' else [args.step]
    for s in steps:
        log(f'== {s}')
        STEPS[s](port, args)


if __name__ == '__main__':
    main()
