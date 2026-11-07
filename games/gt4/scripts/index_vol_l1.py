"""Index GT4L1.VOL (the second-layer volume) read-only."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gt4_vol import Volume  # noqa: E402
from local_tools import build_location  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "analysis/gt4-vol-l1-index.json"


def main():
    vol = Volume.open(build_location() / "game_files/GT4L1.VOL")
    records = vol.read_records()
    print(f"ToC 0x{vol.base:x} pages={vol.page_count} entries={vol.entry_count} "
          f"data_offset=0x{vol.data_offset():x} volume={vol.path.stat().st_size}")
    for record in records:
        kind = "dir" if record.is_dir else ("zip" if record.kind == 2 else "raw")
        print(f"  {kind:4s} parent={record.parent:3d} node={record.node_id:3d} "
              f"size={record.size:12d} packed={record.compressed_size:12d} {record.path}")
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps(dict(
        volume=str(vol.path), base=vol.base, page_count=vol.page_count,
        entry_count=vol.entry_count, data_offset=vol.data_offset(),
        volume_size=vol.path.stat().st_size,
        records=[dict(path=r.path, kind=r.kind, parent=r.parent, node_id=r.node_id,
                      page_offset=r.page_offset, size=r.size,
                      compressed_size=r.compressed_size, modified=r.modified)
                 for r in records]), indent=1) + "\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
