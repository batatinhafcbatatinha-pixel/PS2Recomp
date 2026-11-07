"""Write installer.json from the verified game-file report.

The shared port-installer reads `required_files` (revision check) and
`expected_files` (sizes). GT4 ships two ISO9660 volumes, so the ISO import path
of the shared installer cannot see the layer-2 volume; Install-Files.cmd uses
scripts/extract_game_files.py for the disc and this JSON for folder preparation.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "analysis/game-files-verification.json"
OUT = ROOT / "installer.json"
REQUIRED = ("SYSTEM.CNF", "SCUS_973.28", "CORE.GT4", "IOPRP300.IMG", "GT4.VOL", "GT4L1.VOL")


def main():
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    files = report["files"]
    by_path = {f["path"]: f for f in files}
    missing = [name for name in REQUIRED if name not in by_path]
    if missing:
        raise SystemExit(f"verification report lacks {missing}")
    data = dict(
        schema=1,
        name="Gran Turismo 4 (USA) v2.00",
        revision="PS2",
        filesystem="iso9660",
        destination="game_files",
        required_files={name: by_path[name]["sha256"] for name in REQUIRED},
        required_paths=[],
        expected_files={f["path"]: f["bytes"] for f in sorted(files, key=lambda f: f["path"])},
    )
    text = json.dumps(data, indent=2) + "\n"
    if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
        OUT.write_text(text, encoding="utf-8")
    print(f"installer.json: {len(data['expected_files'])} files, "
          f"{len(data['required_files'])} required, "
          f"{sum(data['expected_files'].values())} bytes")


if __name__ == "__main__":
    main()
