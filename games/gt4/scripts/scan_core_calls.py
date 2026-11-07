"""Collect JAL targets of CORE.ELF that land in the launcher/SDK range (read-only)."""
import collections
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_core_elf import CORE, decode, parse  # noqa: E402
from local_tools import build_location  # noqa: E402

LABELS = Path(__file__).resolve().parents[1] / "config/ghidra-all-labels.csv"


def main():
    data = decode(CORE.read_bytes())
    _h1, _h2, entry, sections = parse(data)
    code = sections[1][1]
    base = sections[1][0]
    targets = collections.Counter()
    words = struct.unpack_from(f"<{len(code) // 4}I", code, 0)
    for i, raw in enumerate(words):
        if (raw >> 26) not in (2, 3):  # j / jal
            continue
        pc = base + i * 4
        target = ((pc + 4) & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)
        if 0x01000000 <= target < 0x01080000:
            targets[target] += 1
    print(f"CORE code 0x{base:08x}-0x{base + len(code):08x}; "
          f"JAL/J targets in 0x01000000-0x01080000: {len(targets)} unique")
    known = {}
    with LABELS.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            known[int(row["Start"], 16)] = row["Name"]
    named = [t for t in targets if t in known]
    print(f"targets matching a SCUS label: {len(named)}")
    for target, count in sorted(targets.items(), key=lambda kv: -kv[1])[:25]:
        print(f"  0x{target:08x} x{count:<5d} {known.get(target, '<unmapped>')}")
    unmapped = sorted(t for t in targets if t not in known)
    print(f"unmapped targets: {len(unmapped)}")
    print("first unmapped:", " ".join(f"0x{t:08x}" for t in unmapped[:20]))


if __name__ == "__main__":
    main()
