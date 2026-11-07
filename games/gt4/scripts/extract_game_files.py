"""Extract and verify GT4's two ISO9660 volumes (DVD9) without touching the source ISO.

The USA v2.00 disc stores two ISO9660 volumes:
  * layer 1 volume, volume descriptors at sector 16 (SYSTEM.CNF, SCUS_973.28,
    CORE.GT4, IOPRP300.IMG, IRX/, NET/, EPSON/, GT4.VOL)
  * layer 2 volume, whose sector 0 starts 16 sectors before the physical layer
    break (descriptors at sector 1317056, root directory at 1317301,
    GT4L1.VOL)
Extents of the layer 2 volume are relative to its own sector 0.

Never deletes or writes to the ISO. Existing files with the same length are
re-read and compared instead of overwritten.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys

WORK = Path(__file__).resolve().parents[1]
DEFAULT_ISO = Path(r"D:\ISO\Gran Turismo 4 (USA) (v2.00)\Gran Turismo 4 (USA) (v2.00).iso")
SECTOR = 2048
ISO_SECTORS = 2594960
ELF_MACHINE = 8  # MIPS
VOLUMES = (
    dict(name=1, base=0, descriptor=16),
    dict(name=2, base=1317040, descriptor=1317056),
)


def build_output():
    location = (WORK / "build-location.txt").read_text(encoding="utf-8").strip()
    return Path(location) / "game_files"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=DEFAULT_ISO)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--report", type=Path, default=WORK / "analysis/game-files-verification.json")
    args = parser.parse_args()
    iso = args.iso
    output = (args.output or build_output()).resolve()
    identity = iso.stat()
    if identity.st_size != ISO_SECTORS * SECTOR:
        raise SystemExit(f"Unexpected ISO size {identity.st_size} for {iso}")

    files = {}
    with iso.open("rb") as source:
        def read(offset, count):
            if offset < 0 or offset + count > identity.st_size:
                raise ValueError("Extent outside ISO")
            source.seek(offset)
            data = source.read(count)
            if len(data) != count:
                raise ValueError("Truncated ISO")
            return data

        def record(data):
            if len(data) < 34 or data[0] != len(data):
                raise ValueError("Invalid ISO9660 directory record")
            sector, size = struct.unpack_from("<I", data, 2)[0], struct.unpack_from("<I", data, 10)[0]
            if sector != struct.unpack_from(">I", data, 6)[0] or size != struct.unpack_from(">I", data, 14)[0]:
                raise ValueError("ISO9660 endian fields disagree")
            if data[26] or data[27]:
                raise ValueError("Interleaved extents need explicit support")
            sector += data[1]  # optional extended-attribute sectors
            n = data[32]
            if 33 + n > len(data):
                raise ValueError("Invalid filename length")
            return sector, size, data[25], data[33:33 + n]

        for volume in VOLUMES:
            base = volume["base"]
            pvd = read(volume["descriptor"] * SECTOR, SECTOR)
            if pvd[:7] != b"\x01CD001\x01" or struct.unpack_from("<H", pvd, 128)[0] != SECTOR:
                raise ValueError(f"Expected ISO9660 2048-byte volume on layer {volume['name']}")
            directories = set()

            def walk(raw_record, prefix="", depth=0):
                sector, size, flags, name = record(raw_record)
                if depth > 64 or (sector, size) in directories or size > 64 * 1024 ** 2:
                    raise ValueError("Invalid directory structure")
                directories.add((sector, size))
                data = read((base + sector) * SECTOR if sector < base else sector * SECTOR, size)
                offset = 0
                while offset < len(data):
                    length = data[offset]
                    if not length:
                        offset = (offset // SECTOR + 1) * SECTOR
                        continue
                    extent, count, attr, raw_name = record(data[offset:offset + length])
                    offset += length
                    if raw_name in (b"\0", b"\1"):
                        continue
                    name = raw_name.decode("ascii").split(";", 1)[0]
                    if not name or name in (".", "..") or any(c in name for c in '\\/:*?"<>|'):
                        raise ValueError("Unsafe ISO9660 path")
                    path = prefix + name
                    if attr & 2:
                        walk(data[offset - length:offset], path + "/", depth + 1)
                    else:
                        key = path.lower()
                        if key in files and not files[key]["continuation"]:
                            raise ValueError("Conflicting ISO9660 path: " + path)
                        entry = files.setdefault(key, dict(path=path, bytes=0, extents=[], continuation=False,
                                                           volume=volume["name"], base=base))
                        absolute = base + extent
                        if absolute * SECTOR + count > identity.st_size:
                            raise ValueError("File extent outside source")
                        entry["extents"].append(dict(sector=absolute, bytes=count))
                        entry["bytes"] += count
                        entry["continuation"] = bool(attr & 0x80)

            walk(pvd[156:156 + pvd[156]])
        if any(e["continuation"] for e in files.values()):
            raise ValueError("Unfinished multiextent file")
        for required in ("system.cnf", "scus_973.28", "gt4.vol", "gt4l1.vol"):
            if required not in files:
                raise ValueError("Expected GT4 file not found: " + required)
        digest = hashlib.sha256()
        for extent in files["scus_973.28"]["extents"]:
            digest.update(read(extent["sector"] * SECTOR, extent["bytes"]))
        elf_header = read(files["scus_973.28"]["extents"][0]["sector"] * SECTOR, 20)
        if elf_header[:6] != b"\x7fELF\x01\x01" or int.from_bytes(elf_header[18:20], "little") != ELF_MACHINE:
            raise ValueError("Expected ELF32 little-endian MIPS executable")
        elf_sha256 = digest.hexdigest()

        output.mkdir(parents=True, exist_ok=True)
        required = sum(e["bytes"] for e in files.values() if not (output / e["path"]).is_file())
        if shutil.disk_usage(output).free - required < 0:
            raise RuntimeError("Insufficient disk space for selected files")
        verified = []
        ordered = sorted(files.values(), key=lambda e: e["extents"][0]["sector"])
        for index, entry in enumerate(ordered, 1):
            target = (output / entry["path"]).resolve()
            if output not in target.parents:
                raise ValueError("Output escapes game_files")
            if target.exists() and target.stat().st_size != entry["bytes"]:
                raise ValueError("Existing file has different length; preserving it: " + entry["path"])
            if shutil.disk_usage(output).free - (0 if target.exists() else entry["bytes"]) < 0:
                raise RuntimeError("Insufficient disk space for next file")
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + ".extracting")
            creating = not target.exists()
            source_hash, file_hash = hashlib.sha256(), hashlib.sha256()
            with (temporary.open("xb") if creating else target.open("rb")) as out:
                for extent in entry["extents"]:
                    source.seek(extent["sector"] * SECTOR)
                    left = extent["bytes"]
                    while left:
                        blob = source.read(min(left, 8 * 1024 ** 2))
                        if not blob:
                            raise ValueError("Truncated source file")
                        source_hash.update(blob)
                        if creating:
                            out.write(blob)
                        else:
                            data = out.read(len(blob))
                            if data != blob:
                                raise ValueError("Existing file differs; preserving it: " + entry["path"])
                            file_hash.update(data)
                        left -= len(blob)
            if creating:
                with temporary.open("rb") as out:
                    while blob := out.read(8 * 1024 ** 2):
                        file_hash.update(blob)
            if source_hash.digest() != file_hash.digest():
                raise ValueError("Extracted content hash mismatch")
            if creating:
                temporary.rename(target)
            verified.append(dict(path=entry["path"], bytes=entry["bytes"], sha256=file_hash.hexdigest(),
                                 volume=entry["volume"], extents=entry["extents"]))
            print(f"[{index}/{len(ordered)}] {entry['path']} {entry['bytes']} bytes", flush=True)
    after = iso.stat()
    if (identity.st_size, identity.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError("Source ISO changed during extraction")
    report = dict(sourceIso=str(iso), sourceIsoBytes=identity.st_size, gameFiles=str(output),
                  fileCount=len(verified), resourceBytes=sum(e["bytes"] for e in verified),
                  allFilesVerified=True, bootElf="SCUS_973.28", bootElfSha256=elf_sha256,
                  files=verified, sourceIsoDeleted=False,
                  volumes=[dict(name=v["name"], sector0=v["base"]) for v in VOLUMES])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Completed: {len(verified)} files, {report['resourceBytes']} bytes; source ISO retained")


if __name__ == "__main__":
    sys.exit(main())
