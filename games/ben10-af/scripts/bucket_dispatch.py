"""Bound compiler fan-out for very large generated native entry dispatch switches.

Preserve every PC->label mapping and the entire translated instruction body.
No functions/instructions are removed or interpreted at runtime.
"""
import hashlib
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
records = []
pattern = re.compile(r'    switch \(ctx->pc\) \{\n(?P<cases>(?:        case 0x[0-9a-f]+u: goto label_[0-9a-f]+;\n)+)'
                     r'        default: break;\n    \}')
for source in (root / 'project/runner').glob('*.cpp'):
    if not source.name.startswith('FUN_') or source.stat().st_size < 4 * 1024 ** 2:
        continue
    text = source.read_text(encoding='utf-8')
    if 'BEN10_BUCKETED_NATIVE_ENTRY' in text:
        metadata = re.search(r'// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+)', text)
        start, end = map(lambda value: int(value, 16), metadata.groups())
        dispatch_end = text.index('\n\n    ctx->pc =', text.index('BEN10_BUCKETED_NATIVE_ENTRY'))
        entry = text[:dispatch_end]
        pairs = re.findall(r'case (0x[0-9a-f]+)u: goto (label_[0-9a-f]+);', entry)
        if [(int(a, 16), label) for a, label in pairs] != [(a, f'label_{a:x}') for a in range(start, end, 4)]:
            raise RuntimeError('Existing bucketed dispatch lost a conservative resume PC')
        body = text[dispatch_end:]
        labels = set(re.findall(r'^(label_[0-9a-f]+):', body, re.MULTILINE))
        if any(label not in labels for address, label in pairs):
            raise RuntimeError('Existing bucketed dispatch has a missing native body label')
        records.append(dict(file=source.name, entries=len(pairs),
            buckets=len({int(a, 16) >> 10 for a, label in pairs}), maxCasesPerBucket=256,
            outputSha256=hashlib.sha256(text.encode()).hexdigest(),
            translatedBodySha256=hashlib.sha256(body.encode()).hexdigest(),
            mappingMatchesOriginalConservativeRange=True, alreadyTransformed=True))
        continue
    match = pattern.search(text)
    if not match:
        continue
    pairs = re.findall(r'case (0x[0-9a-f]+)u: goto (label_[0-9a-f]+);', match['cases'])
    if len(pairs) < 16000:
        continue
    groups = {}
    for address, label in pairs:
        groups.setdefault(int(address, 16) >> 10, []).append((address, label))
    lines = ['    { // BEN10_BUCKETED_NATIVE_ENTRY: preserves all resumable PCs',
             '        const uint32_t ben10_resume_pc = ctx->pc;',
             '        switch (ben10_resume_pc >> 10u) {']
    for bucket, entries in groups.items():
        lines += [f'        case 0x{bucket:x}u:', '            switch (ben10_resume_pc) {']
        lines += [f'            case {address}u: goto {label};' for address, label in entries]
        lines += ['            default: break;', '            }', '            break;']
    lines += ['        default: break;', '        }', '    }']
    replacement = '\n'.join(lines)
    # Static equivalence obligations: exact address/label pairs, bucket
    # membership, unchanged instruction body, and unchanged default fallthrough.
    recovered = re.findall(r'case (0x[0-9a-f]+)u: goto (label_[0-9a-f]+);', replacement)
    if recovered != pairs or len({a for a, label in pairs}) != len(pairs):
        raise RuntimeError('Entry mapping mismatch; no source written')
    if any(int(a, 16) >> 10 != bucket for bucket, entries in groups.items() for a, label in entries):
        raise RuntimeError('Bucket mismatch; no source written')
    body = text[match.end():]
    output = text[:match.start()] + replacement + body
    if output[output.index(replacement) + len(replacement):] != body:
        raise RuntimeError('Translated instruction body mismatch; no source written')
    source.write_text(output, encoding='utf-8')
    records.append(dict(file=source.name, entries=len(pairs), buckets=len(groups),
        maxCasesPerBucket=max(map(len, groups.values())),
        originalSha256=hashlib.sha256(text.encode()).hexdigest(),
        outputSha256=hashlib.sha256(output.encode()).hexdigest(),
        translatedBodySha256=hashlib.sha256(body.encode()).hexdigest(),
        mappingAndInstructionBodyPreserved=True))
if records:
    (root / 'analysis/bucketed-dispatch.json').write_text(json.dumps(dict(
        transformed=records, gamesExecuted=False,
        rationale='Bound maximum direct switch fan-out to 256 without dropping resumable native entries.'), indent=2) + '\n')
print(f'Bucketed {len(records)} large native entry dispatch switch(es)')
