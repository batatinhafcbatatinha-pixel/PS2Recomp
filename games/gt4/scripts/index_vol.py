"""Index GT4.VOL (and GT4L1.VOL) read-only and report code candidates.

Writes analysis/gt4-vol-index.json with every node, its absolute offset in the
volume, packed/real size, and whether the payload starts with an ELF magic.
No game, runner or launcher is executed.
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gt4_vol import RECORD_COMPRESSED, RECORD_FILE, Volume, PS2ZIP_MAGIC  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path((ROOT / "build-location.txt").read_text(encoding="utf-8").strip())
OUT = ROOT / "analysis/gt4-vol-index.json"


def main():
    vol = Volume.open(BUILD / "game_files/GT4.VOL")
    records = vol.read_records()
    files = [r for r in records if not r.is_dir]
    dirs = [r for r in records if r.is_dir]
    print(f"ToC at 0x{vol.base:x}; pages={vol.page_count} entries={vol.entry_count} "
          f"page_len=0x{vol.page_length:x} data_offset=0x{vol.data_offset():x}")
    print(f"nodes: {len(records)} ({len(dirs)} directories, {len(files)} files)")
    print(f"volume size: {vol.path.stat().st_size} bytes; data region starts at 0x{vol.data_offset():x}")

    by_kind = {0: 0, 1: 0, 2: 0}
    total_real = 0
    total_packed = 0
    elfs = []
    ps2zips = []
    over_end = []
    size = vol.path.stat().st_size
    for record in files:
        by_kind[record.kind] += 1
        total_real += record.size
        total_packed += vol.packed_size(record)
        offset = vol.file_offset(record)
        packed = vol.packed_size(record)
        if offset + packed > size:
            over_end.append((record.path, offset, packed, size))
        head = vol.preview(record, 8)
        if head[:4] == b"\x7fELF":
            elfs.append((record.path, offset, record.size, record.kind, head.hex()))
        if head[:4] == PS2ZIP_MAGIC:
            ps2zips.append((record.path, offset, packed, record.size))

    print(f"kinds: plain={by_kind[1]} compressed={by_kind[2]} dirs={by_kind[0]}")
    print(f"total real size: {total_real} ({total_real / 2**30:.2f} GiB), packed: {total_packed}")
    print(f"ELF files: {len(elfs)}")
    for row in elfs[:40]:
        print("  ELF", row)
    print(f"PS2Zip files: {len(ps2zips)}")
    for row in ps2zips[:20]:
        print("  ZIP", row)
    print(f"files crossing the GT4.VOL end: {len(over_end)}")
    for row in over_end[:20]:
        print("  OVER", row)

    top = sorted(files, key=lambda r: -r.size)[:20]
    for record in top:
        print(f"  {record.size:12d} {record.kind} {record.path}")

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps(dict(
        volume=str(vol.path), base=vol.base, page_count=vol.page_count,
        entry_count=vol.entry_count, page_length=vol.page_length,
        data_offset=vol.data_offset(), volume_size=size,
        file_count=len(files), dir_count=len(dirs),
        total_real=total_real, total_packed=total_packed,
        elf_files=[dict(path=r[0], offset=r[1], size=r[2], kind=r[3]) for r in elfs],
        records=[dict(path=r.path, kind=r.kind, parent=r.parent, node_id=r.node_id,
                      page_offset=r.page_offset, size=r.size,
                      compressed_size=r.compressed_size, modified=r.modified)
                 for r in records],
    ), indent=1) + "\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
