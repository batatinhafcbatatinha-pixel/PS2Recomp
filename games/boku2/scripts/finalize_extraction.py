"""Verify the decoded DVD and extracted files, then remove only working copies."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
identity = json.loads((ROOT / 'analysis/chd-identity.json').read_text())
iso = ROOT / 'analysis/boku2-extraction.iso'
working_chd = ROOT / 'analysis/boku2-working.chd'
assert iso.stat().st_size == identity['logicalBytes']
with iso.open('rb') as source:
    raw_sha1 = hashlib.file_digest(source, 'sha1').hexdigest()
assert raw_sha1 == identity['expectedDataSha1'], 'Decoded DVD differs from original CHD data hash'
print('Decoded DVD SHA-1 matches original CHD header', flush=True)
subprocess.run([sys.executable, str(ROOT / 'scripts/extract_game_files.py')], check=True)
report_path = ROOT / 'analysis/game-files-verification.json'
report = json.loads(report_path.read_text())
inventory = json.loads((ROOT / 'analysis/iso-inventory.json').read_text())
assert report['allFilesVerified']
assert report['fileCount'] == inventory['fileCount']
assert report['resourceBytes'] == inventory['resourceBytes']
original = Path(identity['sourceChd'])
stat = original.stat()
assert stat.st_size == identity['sourceBytes'] and stat.st_mtime_ns == identity['sourceMtimeNs']
report.update(sourceChd=str(original), sourceChdSha256=identity['sha256'],
              originalChdPreserved=True, decodedDvdSha1=raw_sha1,
              decodedDvdMatchesChdDataSha1=True)
report.pop('sourceIsoStillRequired', None)
report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
for temporary in (iso, working_chd):
    assert temporary.resolve().parent == (ROOT / 'analysis').resolve()
    assert temporary.resolve() != original.resolve()
    temporary.unlink()
report.update(sourceIsoDeleted=True, temporaryWorkingChdDeleted=True)
report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
media_path = ROOT / 'source-media.json'
media = json.loads(media_path.read_text())
media.update(status='DVD decoded and all 926 files extracted and verified; no game runtime or gameplay test',
             platform='PlayStation 2: BOOT2 SYSTEM.CNF and ELF32 little-endian MIPS executable SCPS_150.26',
             gameFiles=str(ROOT / 'game_files'), fileCount=report['fileCount'],
             resourceBytes=report['resourceBytes'], originalPreserved=True,
             sha256=identity['sha256'])
media_path.write_text(json.dumps(media, indent=2) + '\n', encoding='utf-8')
print(f'Completed: {report["fileCount"]} files verified; original CHD preserved; temporary CHD and ISO removed', flush=True)
