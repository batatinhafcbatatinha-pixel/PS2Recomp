"""Boot the runner and drive the menus to a campaign start with a scripted pad file.

Presses X through the intros, then alternates Start and X through the menus until the
mission loader binds MPEG again and the menu presses stop.
Stops on a missing-code report or after the time limit, keeping the process window optional.
Usage: python scripts/drive_campaign.py TAG [SECONDS] [--keep]
"""
import os
import hashlib
import json
import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
tag = sys.argv[1]
limit = float(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2][0].isdigit() else 600.0
keep = '--keep' in sys.argv
pad = root / f"analysis/pad-{tag}.txt"
err = root / f"analysis/boot-{tag}.log"
out = root / f"analysis/boot-{tag}-out.log"
pad.write_text("0\n")
env = dict(os.environ)
env.update(PS2X_CD_IMAGE="", PS2X_PAD_FILE=str(pad), PS2X_FRAME_DUMP=f"analysis/frame-{tag}.png", PS2X_WINDOW_MODE="windowed",
           PS2X_VU_DUMP=f"analysis/vu1-code-{tag}.bin")
command = [str(root / "build/game/ps2xRuntime/ps2EntryRunner.exe"), str(root / "game_files/SLUS_217.46")]
startup = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=4)  # visible, keeps the focus off
proc = subprocess.Popen(startupinfo=startup, args=command,
                        cwd=root, env=env, stdout=out.open("w"), stderr=err.open("w"))


def log():
    return err.read_text(errors="replace")


def press(mask, hold=0.4, gap=1.6):
    pad.write_text(f"{mask:x}\n")
    time.sleep(hold)
    pad.write_text("0\n")
    time.sleep(gap)


start = time.time()
stage = "title"
binds = 0
while time.time() - start < limit and proc.poll() is None:
    text = log()
    missing = [l for l in text.splitlines() if "missing-target" in l and "target=0x0 " not in l]
    if missing or "Unhandled" in text:
        stage = "missing"
        break
    binds = text.count("[mpeg:bind]")
    if stage == "title":
        if binds >= 1:
            # Skip the intro video, then leave the title with Start.
            for _ in range(6):
                press(0x4000, gap=2.6)
            press(0x8, gap=3)
            stage = "menus"
            menu_presses = 0
        else:
            press(0x4000, gap=5)
    elif stage == "menus":
        # Save warning, profile, campaign, New Game, difficulty (real-time menus).
        if menu_presses >= 8:
            stage = "loading"
            pad.write_text("0\n")
        else:
            press(0x4000, gap=2.6)
            menu_presses += 1
            frame = root / f"analysis/frame-{tag}.png"
            if frame.exists():
                (root / f"analysis/frame-{tag}-menu{menu_presses}.png").write_bytes(frame.read_bytes())
    else:
        # Keep a few snapshots of the mission screen; the dump file is overwritten.
        # DRIVE_STICK="rx ry lx ly" (hex) holds the sticks for a few seconds at each snapshot, to frame the real scene.
        stick = os.environ.get("DRIVE_STICK")
        if stick:
            pad.write_text(f"0 {stick}\n")
            time.sleep(float(os.environ.get("DRIVE_STICK_SECONDS", "1.5")))
            pad.write_text("0\n")
        time.sleep(30)
        frame = root / f"analysis/frame-{tag}.png"
        if frame.exists():
            snapshot = root / f"analysis/frame-{tag}-{int(time.time() - start):04d}.png"
            snapshot.write_bytes(frame.read_bytes())
elapsed = time.time() - start
print(f"stage={stage} binds={binds} elapsed={elapsed:.0f}s alive={proc.poll() is None}")
for line in log().splitlines():
    if "missing-target" in line or "Unhandled" in line or "exception" in line.lower():
        print(line[:600])
if not keep and proc.poll() is None:
    proc.kill()
    proc.wait()

report = dict(command=command, isoArgument=False, inheritedIsoEnvironment=False,
              stage=stage, mpegBinds=binds, elapsed=round(elapsed, 2),
              stopReason='time limit' if elapsed >= limit else 'missing code' if stage == 'missing' else 'exited',
              exitCode=proc.poll(),
              binarySha256=hashlib.sha256(pathlib.Path(command[0]).read_bytes()).hexdigest(),
              limitations=['Script stage and log evidence do not prove gameplay; inspect saved frames.'])
(root / f'analysis/report-{tag}.json').write_text(json.dumps(report, indent=2) + '\n')
