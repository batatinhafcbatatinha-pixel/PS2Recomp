"""Boot hidden with a scripted pad file and keep frame snapshots.

Pad masks (pressed bits): Select 0x1, Start 0x8, Up 0x10, Right 0x20, Down 0x40, Left 0x80, Triangle 0x1000,
Circle 0x2000, Cross 0x4000, Square 0x8000.
Usage: python scripts/drive.py SECONDS [START_DELAY] [BUTTON_HEX ...]   (buttons cycle every 3 s after START_DELAY)
Snapshots: runs/drive/frame-<t>.png; log: runs/drive/runtime.log. Never infers playable status.
"""
import argparse
import json
import hashlib
import os
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('seconds', nargs='?', type=float, default=120)
parser.add_argument('delay', nargs='?', type=float, default=20)
parser.add_argument('buttons', nargs='*', type=lambda value: int(value, 16))
parser.add_argument('--binary-dir', type=Path)
parser.add_argument('--run-name', default='drive')
args = parser.parse_args()
if args.seconds <= 0 or args.delay < 0:
    parser.error('Invalid duration')
if Path(args.run_name).name != args.run_name or args.run_name in ('.', '..'):
    parser.error('run-name must be one directory name')
limit, delay = args.seconds, args.delay
buttons = args.buttons or [0x4000]
build = args.binary_dir or Path((root / 'build-location.txt').read_text(encoding='utf-8-sig').strip())
run = root / 'runs' / args.run_name
run.mkdir(parents=True, exist_ok=True)
pad, frame, log_path = run / 'pad.txt', run / 'frame.png', run / 'runtime.log'
pad.write_text('0\n')
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
env = {**os.environ, 'PS2X_CD_IMAGE': '', 'PS2X_WINDOW_MODE': 'windowed', 'PS2X_PAD_FILE': str(pad), 'PS2X_FRAME_DUMP': str(frame)}
with log_path.open('w', encoding='utf-8') as log:
    proc = subprocess.Popen([str(build / 'ps2EntryRunner.exe'), str(root / 'game_files/SLUS_218.15')],
                            cwd=root, startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, env=env)


def snapshot(tag):
    if frame.exists():
        try:
            (run / f'frame-{tag}.png').write_bytes(frame.read_bytes())
        except OSError:
            pass


start = time.monotonic()
stop_reason = 'exited'
try:
    step = 0
    while time.monotonic() - start < limit and proc.poll() is None:
        elapsed = time.monotonic() - start
        if elapsed >= delay:
            pad.write_text(f'{buttons[step % len(buttons)]:x}\n')
            time.sleep(0.3)
            pad.write_text('0\n')
            step += 1
            time.sleep(2.7)
        else:
            time.sleep(3)
        snapshot(f'{int(time.monotonic() - start):03d}')
finally:
    if proc.poll() is None:
        stop_reason = 'time limit' if time.monotonic() - start >= limit else 'terminated by test'
        subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)
    proc.wait()
report = dict(seconds=round(time.monotonic() - start, 2), exitCode=proc.returncode,
              stopReason=stop_reason, discImage=None, binary=str(build / 'ps2EntryRunner.exe'),
              binarySha256=hashlib.sha256((build / 'ps2EntryRunner.exe').read_bytes()).hexdigest(),
              limitations=['Scripted input and FPS do not prove successful gameplay.'])
(run / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report))
