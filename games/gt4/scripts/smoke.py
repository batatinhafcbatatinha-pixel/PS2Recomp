"""Bounded boot diagnostic of the current GT4 build: runs hidden for N seconds, keeps the log (and a frame dump)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

from local_tools import ROOT, build_location, game_files

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=float, default=30)
parser.add_argument('--name', default='smoke')
parser.add_argument('--show', action='store_true', help='visible window')
parser.add_argument('--env', action='append', default=[], help='extra NAME=VALUE')
args = parser.parse_args()
binary = build_location() / 'build/native/ps2recomp/ps2xRuntime/ps2EntryRunner.exe'
elf = game_files() / 'CORE.ELF'
for path in (binary, elf):
    if not path.is_file():
        raise SystemExit(f'Required input missing: {path}')
run = ROOT / 'runs' / args.name
run.mkdir(parents=True, exist_ok=True)
env = {**os.environ, 'PS2X_CD_IMAGE': '', 'PS2X_WINDOW_MODE': 'windowed', 'PS2X_FRAME_DUMP': str(run / 'frame.png')}
env.update(e.split('=', 1) for e in args.env)
startup = subprocess.STARTUPINFO()
if not args.show:
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
started = time.monotonic()
log_path = run / 'runtime.log'
with log_path.open('w', encoding='utf-8') as log:
    proc = subprocess.Popen([str(binary), str(elf)], cwd=ROOT, startupinfo=startup, stdout=log,
                            stderr=subprocess.STDOUT, env=env)
    reason = 'exited'
    try:
        while proc.poll() is None:
            if time.monotonic() - started >= args.seconds:
                reason = 'time limit'
                break
            if log_path.stat().st_size >= 64 * 1024**2:
                reason = 'log limit'
                break
            time.sleep(.25)
    finally:
        if proc.poll() is None:
            subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)
        proc.wait()
report = dict(exitCode=proc.returncode, stopReason=reason, seconds=round(time.monotonic() - started, 1),
              log=str(log_path))
(run / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
