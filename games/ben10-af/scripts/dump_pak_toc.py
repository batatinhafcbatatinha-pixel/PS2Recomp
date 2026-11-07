"""Copy DATA.PAK's header + TOC (read-only) to analysis/pak-toc.bin for offline inspection."""
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
with (root / 'game_files/DATA.PAK').open('rb') as f:
    hdr = f.read(16)
    toc_size, dir_size, version, align = struct.unpack('<4I', hdr)
    f.seek(0)
    blob = f.read(version * align + toc_size)
(root / 'analysis/pak-toc.bin').write_bytes(blob)
print('header', [hex(x) for x in (toc_size, dir_size, version, align)], 'wrote', len(blob))
