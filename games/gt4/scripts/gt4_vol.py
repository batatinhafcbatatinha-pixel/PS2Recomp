"""Read-only reader for the Gran Turismo 4 PS2 volume (RoFS / GT4.VOL).

Format notes (public GT modding documentation, reimplemented here):
  * the first 0x40 bytes are a fake GT3-style table of contents;
  * the real ToC lives at a page boundary (page 0x2159 for retail GT4);
  * header: magic 0xAD90B9AC (big-endian), version 0x00030001 (LE),
    compressed ToC length, page count, page length (0x800), entry count;
  * page offsets are XOR-encrypted with 0x14AC327A * (index + 1);
  * each page is a raw deflate stream, then XOR 0x55;
  * entries are node records at the bottom of each entry page.

Nothing here writes to the disc image: the volume is opened read-only.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

PAGE = 0x800
TOC_MAGIC = 0xAD90B9AC
TOC_VERSION = 0x00030001
OFFSET_KEY = 0x14AC327A
DATA_KEY = 0x55
PS2ZIP_MAGIC = b"\xc5\xee\xf7\xff"

RECORD_DIRECTORY = 0
RECORD_FILE = 1
RECORD_COMPRESSED = 2


@dataclass
class Record:
    name: str
    kind: int
    parent: int
    node_id: int = 0
    page_offset: int = 0
    size: int = 0
    compressed_size: int = 0
    modified: int = 0
    path: str = ""

    @property
    def is_dir(self) -> bool:
        return self.kind == RECORD_DIRECTORY


@dataclass
class Volume:
    path: Path
    base: int = 0
    page_count: int = 0
    entry_count: int = 0
    page_length: int = PAGE
    compressed_toc_length: int = 0
    records: list[Record] = field(default_factory=list)
    pages: list[tuple[int, int]] = field(default_factory=list)

    # ---------------------------------------------------------------- header
    @classmethod
    def open(cls, path: Path) -> "Volume":
        vol = cls(path=Path(path))
        vol._read_header()
        vol._read_page_table()
        return vol

    def _read_header(self) -> None:
        with self.path.open("rb") as f:
            for index in range(min(self.path.stat().st_size // PAGE, 10000)):
                f.seek(index * PAGE)
                head = f.read(0x14)
                if len(head) < 0x14:
                    break
                magic = struct.unpack_from(">I", head, 0)[0]
                version = struct.unpack_from("<I", head, 4)[0]
                if magic == TOC_MAGIC and version == TOC_VERSION:
                    self.base = index * PAGE
                    (self.compressed_toc_length, self.page_count) = struct.unpack_from("<iI", head, 8)
                    (self.page_length, self.entry_count) = struct.unpack_from("<HH", head, 0x10)
                    return
        raise SystemExit(f"no GT4 volume ToC found in {self.path}")

    def data_offset(self) -> int:
        return self.base + self.page_count * self.page_length

    # ------------------------------------------------------------ page table
    def _read_page_table(self) -> None:
        with self.path.open("rb") as f:
            f.seek(self.base + 0x40)
            raw = f.read((self.entry_count + 1) * 4)
        values = struct.unpack_from(f"<{self.entry_count + 1}I", raw, 0)
        offsets = [v ^ ((OFFSET_KEY * (i + 1)) & 0xFFFFFFFF) for i, v in enumerate(values)]
        self.pages = []
        for i in range(1, len(offsets)):
            self.pages.append((self.base + offsets[i - 1], offsets[i] - offsets[i - 1]))

    def _read_page(self, offset: int, length: int) -> bytes:
        with self.path.open("rb") as f:
            f.seek(offset)
            raw = f.read(length)
        stream = zlib.decompressobj(-15)
        data = stream.decompress(raw) + stream.flush()
        return bytes(b ^ DATA_KEY for b in data)

    # -------------------------------------------------------------- records
    def read_records(self) -> list[Record]:
        if self.records:
            return self.records
        for offset, length in self.pages:
            data = self._read_page(offset, length)
            flag, count = struct.unpack_from("<hh", data, 0)
            if flag != 0:
                continue
            end = len(data)
            for i in range(count // 2):
                pos = end - (i * 8 + 8)
                entry_offset, entry_length, meta_offset, _meta_length = struct.unpack_from("<hhhh", data, pos)
                parent = struct.unpack_from(">I", data, entry_offset)[0]
                name = data[entry_offset + 4:entry_offset + entry_length].decode("latin-1")
                kind = data[meta_offset]
                record = Record(name=name, kind=kind, parent=parent)
                if kind == RECORD_DIRECTORY:
                    record.node_id = struct.unpack_from("<i", data, meta_offset + 1)[0]
                elif kind == RECORD_FILE:
                    record.page_offset, record.modified, record.size = struct.unpack_from("<III", data, meta_offset + 1)
                elif kind == RECORD_COMPRESSED:
                    (record.page_offset, record.modified, record.compressed_size,
                     record.size) = struct.unpack_from("<IIII", data, meta_offset + 1)
                else:
                    continue
                self.records.append(record)
        by_id = {r.node_id: r for r in self.records if r.is_dir}
        for record in self.records:
            parts = [record.name]
            parent = by_id.get(record.parent)
            seen = 0
            while parent is not None and seen < 64:
                parts.append(parent.name)
                parent = by_id.get(parent.parent)
                seen += 1
            record.path = "/".join(reversed(parts))
        return self.records

    # ------------------------------------------------------------- payloads
    def file_offset(self, record: Record) -> int:
        return self.data_offset() + record.page_offset * self.page_length

    def packed_size(self, record: Record) -> int:
        return record.compressed_size if record.kind == RECORD_COMPRESSED else record.size

    def read_file(self, record: Record) -> bytes:
        offset = self.file_offset(record)
        size = self.packed_size(record)
        with self.path.open("rb") as f:
            f.seek(offset)
            data = f.read(size)
        if data[:4] == PS2ZIP_MAGIC:
            data = zlib.decompress(data[8:], -15)
        return data

    def preview(self, record: Record, size: int = 16) -> bytes:
        with self.path.open("rb") as f:
            f.seek(self.file_offset(record))
            return f.read(size)
