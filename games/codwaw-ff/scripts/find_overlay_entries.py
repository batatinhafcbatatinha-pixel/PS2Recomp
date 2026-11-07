"""Produce supplemental candidates for unregistered EE stack-entry prologues.

Candidates are static evidence only; boot tests establish which are used.
Existing entrypoints are never overridden by their supplemental registration.
"""
import bisect
import csv
import pathlib
import re
import struct

root = pathlib.Path(__file__).resolve().parent.parent
elf = (root / "analysis/mpeg-relocated.elf").read_bytes()
_, offset, base, _, size, _, _, _ = struct.unpack_from("<8I", elf, 52)
registration = (root / "project/supplemental-mpeg/register_functions.cpp").read_text()
known = {int(value, 16) for value in re.findall(r"// (0x[0-9a-f]+)", registration)}
starts = {int(value, 16) for value in re.findall(r"void sub_\w+_0x([0-9a-f]+)\(", (root / "project/supplemental-mpeg/ps2_recompiled_functions.h").read_text())}
candidates = [base + index for index in range(0, size, 4)
              if struct.unpack_from("<I", elf, offset + index)[0] & 0xffff8000 == 0x27bd8000
              and base + index not in known]
boundaries = sorted(starts | set(candidates) | {base + size})
with (root / "analysis/mpeg-extra-functions.csv").open("w", newline="") as stream:
    writer = csv.writer(stream)
    writer.writerow(["Name", "Start", "End", "Size"])
    for start in candidates:
        end = boundaries[bisect.bisect_right(boundaries, start)]
        writer.writerow([f"sub_{start:08X}_EXTRA", hex(start), hex(end), end - start])
print(f"Wrote {len(candidates)} candidate ranges")
