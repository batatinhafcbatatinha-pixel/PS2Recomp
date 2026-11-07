"""Bounded boot diagnostic of the current Ben10 build; never infers playable status."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=float, default=20)
args = parser.parse_args()
if args.seconds <= 0:
    parser.error('--seconds must be positive')
location = root / 'build-location.txt'
build = Path(location.read_text(encoding='utf-8-sig').strip()) if location.is_file() else root / 'build/native/ps2recomp/ps2xRuntime'
binary = build / 'ps2EntryRunner.exe'
elf = root / 'game_files/SLUS_218.15'
for path in (binary, elf):
    if not path.is_file():
        raise SystemExit(f'Required input missing: {path}')
run = root / 'runs/smoke'
run.mkdir(parents=True, exist_ok=True)
command = [str(binary), str(elf)]
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
started = time.monotonic()
with (run / 'runtime.log').open('w', encoding='utf-8') as log:
    proc = subprocess.Popen(command, cwd=root, startupinfo=startup,
        stdout=log, stderr=subprocess.STDOUT,
        env={**os.environ, 'PS2X_CD_IMAGE': '', 'PS2X_WINDOW_MODE': 'windowed'})
    reason = 'exited'
    try:
        while proc.poll() is None:
            if time.monotonic() - started >= args.seconds:
                reason = 'time limit'
                break
            if (run / 'runtime.log').stat().st_size >= 32 * 1024**2:
                reason = 'log limit'
                break
            time.sleep(.25)
    finally:
        if proc.poll() is None:
            subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)
        proc.wait()
report = dict(command=command, binarySha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    exitCode=proc.returncode, stopReason=reason, seconds=round(time.monotonic()-started, 2),
    limitations=['Process/log evidence only; timeout is not successful completion.',
                 'No visual, gameplay, control or save validation.'])
(root / 'analysis/runtime-smoke.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
