"""Extract/verify Ben10's ISO9660 files; never deletes the source ISO."""
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[1]
ISO = Path(r'D:\ISO\Ben 10 - Alien Force (USA) (En,Fr,De,Es,It)\Ben 10 - Alien Force (USA) (En,Fr,De,Es,It).iso')
OUTPUT = (ROOT / 'game_files').resolve()
SECTOR = 2048
ELF_SHA = '92004adcf85e877199be5b5ca017c640e95795017c0ca3c8d4ecbde52567cb61'


def main():
    identity = ISO.stat()
    files, directories = {}, set()
    with ISO.open('rb') as source:
        def read(offset, count):
            if offset < 0 or offset + count > identity.st_size:
                raise ValueError('Extent outside ISO')
            source.seek(offset)
            data = source.read(count)
            if len(data) != count:
                raise ValueError('Truncated ISO')
            return data

        pvd = read(16 * SECTOR, SECTOR)
        if pvd[:7] != b'\x01CD001\x01' or struct.unpack_from('<H', pvd, 128)[0] != SECTOR:
            raise ValueError('Expected ISO9660 2048-byte volume')

        def record(data):
            if len(data) < 34 or data[0] != len(data):
                raise ValueError('Invalid ISO9660 directory record')
            sector, size = struct.unpack_from('<I', data, 2)[0], struct.unpack_from('<I', data, 10)[0]
            if sector != struct.unpack_from('>I', data, 6)[0] or size != struct.unpack_from('>I', data, 14)[0]:
                raise ValueError('ISO9660 endian fields disagree')
            if data[26] or data[27]:
                raise ValueError('Interleaved extents need explicit support')
            sector += data[1]  # optional extended-attribute sectors
            n = data[32]
            if 33 + n > len(data):
                raise ValueError('Invalid filename length')
            return sector, size, data[25], data[33:33+n]

        def walk(raw_record, prefix='', depth=0):
            sector, size, flags, name = record(raw_record)
            if depth > 64 or (sector, size) in directories or size > 64 * 1024 ** 2:
                raise ValueError('Invalid directory structure')
            directories.add((sector, size))
            data = read(sector * SECTOR, size)
            offset = 0
            while offset < len(data):
                length = data[offset]
                if not length:
                    offset = (offset // SECTOR + 1) * SECTOR
                    continue
                extent, count, attr, raw_name = record(data[offset:offset+length])
                offset += length
                if raw_name in (b'\0', b'\1'):
                    continue
                name = raw_name.decode('ascii').split(';', 1)[0]
                if not name or name in ('.', '..') or any(c in name for c in '\\/:*?"<>|'):
                    raise ValueError('Unsafe ISO9660 path')
                path = prefix + name
                if attr & 2:
                    walk(data[offset-length:offset], path + '/', depth + 1)
                else:
                    key = path.lower()
                    if key in files and not files[key]['continuation']:
                        raise ValueError('Conflicting ISO9660 path: ' + path)
                    entry = files.setdefault(key, dict(path=path, bytes=0, extents=[], continuation=False))
                    if extent * SECTOR + count > identity.st_size:
                        raise ValueError('File extent outside source')
                    entry['extents'].append(dict(sector=extent, bytes=count))
                    entry['bytes'] += count
                    entry['continuation'] = bool(attr & 0x80)

        walk(pvd[156:156+pvd[156]])
        if any(e['continuation'] for e in files.values()):
            raise ValueError('Unfinished multiextent file')
        elf = files.get('slus_218.15')
        if not elf:
            raise ValueError('Expected Ben10 executable not found')
        digest = hashlib.sha256()
        for extent in elf['extents']:
            digest.update(read(extent['sector'] * SECTOR, extent['bytes']))
        if digest.hexdigest() != ELF_SHA:
            raise ValueError('Wrong Ben10 executable identity; nothing extracted')
        OUTPUT.mkdir(exist_ok=True)
        required = sum(e['bytes'] for e in files.values() if not (OUTPUT / e['path']).is_file())
        if shutil.disk_usage(OUTPUT).free - required < 0:
            raise RuntimeError('Insufficient disk space for selected files')
        verified = []
        for index, entry in enumerate(sorted(files.values(), key=lambda e:e['extents'][0]['sector']), 1):
            target = (OUTPUT / entry['path']).resolve()
            if OUTPUT not in target.parents:
                raise ValueError('Output escapes game_files')
            if target.exists() and target.stat().st_size != entry['bytes']:
                raise ValueError('Existing file has different length; preserving it: ' + entry['path'])
            if shutil.disk_usage(OUTPUT).free - (0 if target.exists() else entry['bytes']) < 0:
                raise RuntimeError('Insufficient disk space for next file')
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + '.extracting')
            creating = not target.exists()
            source_hash, file_hash = hashlib.sha256(), hashlib.sha256()
            with (temporary.open('xb') if creating else target.open('rb')) as output:
                for extent in entry['extents']:
                    source.seek(extent['sector'] * SECTOR)
                    left = extent['bytes']
                    while left:
                        blob = source.read(min(left, 4 * 1024 ** 2))
                        if not blob:
                            raise ValueError('Truncated source file')
                        source_hash.update(blob)
                        if creating:
                            output.write(blob)
                        else:
                            data = output.read(len(blob))
                            if data != blob:
                                raise ValueError('Existing file differs; preserving it: ' + entry['path'])
                            file_hash.update(data)
                        left -= len(blob)
            if creating:
                with temporary.open('rb') as output:
                    while blob := output.read(4 * 1024 ** 2):
                        file_hash.update(blob)
            if source_hash.digest() != file_hash.digest():
                raise ValueError('Extracted content hash mismatch')
            if creating:
                temporary.rename(target)
            verified.append(dict(path=entry['path'], bytes=entry['bytes'], sha256=file_hash.hexdigest(), extents=entry['extents']))
            if index % 50 == 0:
                print(f'Verified {index}/{len(files)} files', flush=True)
    after = ISO.stat()
    if (identity.st_size, identity.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('Source ISO changed during extraction')
    report = dict(sourceIso=str(ISO), sourceIsoBytes=identity.st_size, gameFiles=str(OUTPUT),
                  fileCount=len(verified), resourceBytes=sum(e['bytes'] for e in verified),
                  allFilesVerified=True, files=verified, sourceIsoDeleted=False,
                  sourceIsoStillRequired='Current runtime still performs raw sector reads; retain source until that dependency is removed.')
    (ROOT / 'analysis/game-files-verification.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(f'Completed: {len(verified)} files, {report["resourceBytes"]} bytes; source ISO retained')


if __name__ == '__main__':
    main()
