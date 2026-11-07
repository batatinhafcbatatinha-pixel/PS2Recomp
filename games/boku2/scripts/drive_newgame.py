"""Boot hidden, wait for the title menu, press New Game with a scripted pad file and keep frame snapshots.

Pad masks (pressed bits): Start 0x8, Up 0x10, Down 0x40, Circle 0x2000, Cross 0x4000.
Usage: python scripts/drive_newgame.py [SECONDS] [BUTTON_HEX ...]   (default buttons: Start, then Circle)
Snapshots: runs/drive/frame-<t>.png; log: runs/drive/runtime.log. Never infers playable status.
"""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('seconds', nargs='?', type=float, default=150)
parser.add_argument('buttons', nargs='*', type=lambda value: int(value, 16))
parser.add_argument('--boot-seconds', type=float, help='Fixed boot wait; bypass FPS heuristic (does not prove title readiness)')
parser.add_argument('--no-image', action='store_true', help='Test extracted-file reads without a disc image')
parser.add_argument('--run-name', default='drive', help='Separate output directory under runs')
args = parser.parse_args()
limit = args.seconds
if limit <= 0 or (args.boot_seconds is not None and args.boot_seconds < 0):
    parser.error('Durations must be positive (boot wait may be zero)')
if Path(args.run_name).name != args.run_name or args.run_name in ('.', '..'):
    parser.error('run-name must be a directory name')
buttons = args.buttons or [0x8, 0x2000]
build = Path(os.environ.get('BOKU2_BIN_DIR') or (root / 'build-location.txt').read_text(encoding='utf-8-sig').strip())
image = ''  # This port only reads extracted files, including in diagnostic runs.
run = root / 'runs' / args.run_name
run.mkdir(parents=True, exist_ok=True)
pad, frame, log_path = run / 'pad.txt', run / 'frame.png', run / 'runtime.log'
pad.write_text('0\n')
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
env = {**os.environ, 'PS2X_CD_IMAGE': '', 'PS2X_WINDOW_MODE': 'windowed', 'PS2X_PAD_FILE': str(pad), 'PS2X_FRAME_DUMP': str(frame)}
with log_path.open('w', encoding='utf-8') as log:
    proc = subprocess.Popen([str(build / 'ps2EntryRunner.exe'), str(root / 'game_files/SCPS_150.26')],
                            cwd=root, startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, env=env)


def press(mask, hold=0.3, gap=2.0):
    pad.write_text(f'{mask:x}\n')
    time.sleep(hold)
    pad.write_text('0\n')
    time.sleep(gap)


def snapshot(tag):
    if frame.exists():
        (run / f'frame-{tag}.png').write_bytes(frame.read_bytes())


start = time.monotonic()
step = 0
stop_reason = 'exited'
try:
    # Title menu: game_fps reaches ~30 once it is up.
    while time.monotonic() - start < limit and proc.poll() is None:
        if args.boot_seconds is not None:
            if time.monotonic() - start >= args.boot_seconds:
                break
        else:
            text = log_path.read_text(errors='replace')
            if any(float(fps) >= 20 for fps in re.findall(r'game_fps=([0-9.]+)', text)):
                break
        time.sleep(min(.25, max(0, limit - (time.monotonic() - start))))
    snapshot(f'{int(time.monotonic() - start):03d}-boot-wait')
    while time.monotonic() - start < limit and proc.poll() is None:
        press(buttons[step % len(buttons)], gap=2.5)
        step += 1
        snapshot(f'{int(time.monotonic() - start):03d}-p{step}')
finally:
    if proc.poll() is None:
        stop_reason = 'time limit'
        subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], capture_output=True)
    proc.wait()
report = dict(seconds=round(time.monotonic() - start, 2), exitCode=proc.returncode,
              stopReason=stop_reason, discImage=image, buttonSteps=step, binary=str(build / 'ps2EntryRunner.exe'),
              limitations=['FPS and scripted input do not prove title readiness or playable status.'])
(run / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report))
