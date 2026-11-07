"""SDK functions: which ones the runtime serves natively, and finding the ones the analyzer misses.

Policy (the same for every game): a matched SDK function is replaced by the runtime's native handler only when that
handler really implements it (it is not a TODO stub) and the function talks to the IOP side (SIF RPC, CD, pad,
memory card, MPEG, file I/O, threads/semaphores of the kernel). EE-only library code (GS/GIF packet builders, IPU
setup, the SDK thread start-up) keeps the game's own translated code, which is exact by construction.

Signatures (sdk_signatures.json) find SDK functions the analyzer does not: a hash of the function's instruction words with
address fields masked (J/JAL targets, LUI and %lo immediates), learned from games where the function is known; no
instruction bytes are stored. A
signature binds only when it matches exactly once. Short functions are found relative to an anchor function of the
same library area, checked against their masked words. The layout differs between SDK releases and link orders (the
same sceCdSearchFile wrapper sits 0x478 bytes before sceCdInit in FlatOut 2 and elsewhere in GTA LCS), so an entry
keeps every anchor it was learned with and each one is tried.
"""
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
SIGNATURES = HERE / 'sdk_signatures.json'

# EE-side library code whose translated guest version is right, and whose runtime stub is not (see notes).
GUEST_ALWAYS = {
    'sceIpuInit',            # runtime stub hard-codes one game's addresses
    'sceGsSetDefClear',      # runtime stub is an empty no-op
    'InitThread',            # guest creates the SDK top thread and lowers main to priority 1
    'sceGifPkAddGsAD', 'sceGifPkCloseGifTag',  # the native builder keeps the open DMA tag's qwc live
    'sceGsSetDefLoadImage', 'sceGsExecLoadImage', 'sceGsSetDefStoreImage', 'sceGsExecStoreImage',
    '_sceSifSendCmd', '_sceSifCmdIntrHdlr',    # internal variants with another ABI
}
GUEST_PREFIXES = ('sceGs', 'sceGifPk')


def runtime_lists(repo):
    call_list = (repo / 'ps2xRuntime/include/ps2_call_list.h').read_text()

    def names(macro):
        body = re.search(rf'#define {macro}\(X\)(.*?)(?:\n#define|\Z)', call_list, re.S)[1]
        return set(re.findall(r'X\((\w+)\)', body))

    todo = set()
    for source in (repo / 'ps2xRuntime/src/lib').rglob('*.cpp'):
        todo |= set(re.findall(r'TODO_NAMED\("(\w+)"', source.read_text(errors='replace')))
    return names('PS2_SYSCALL_LIST'), names('PS2_STUB_LIST'), todo


def mask_word(w):
    op = w >> 26
    if op in (2, 3):                     # j / jal: absolute target
        return w & 0xFC000000, 0xFC000000
    if op == 15:                         # lui: high half of an address
        return w & 0xFFFF0000, 0xFFFF0000
    rs = (w >> 21) & 31
    # addiu / ori / loads and stores may carry the %lo of an address; stack-relative ones never do.
    if (op in (9, 13) or op >= 0x20) and rs != 29:
        return w & 0xFFFF0000, 0xFFFF0000
    return w, 0xFFFFFFFF


def function_words(elf, start, limit=512):
    """Words of the function at start up to its first `jr ra` + delay slot (or limit)."""
    out = []
    a = start
    while len(out) < limit:
        w = elf.word(a)
        out.append(w)
        if w == 0x03E00008:
            out.append(elf.word(a + 4))
            break
        a += 4
    return out


def masked_bytes(words):
    return b''.join(mask_word(w)[0].to_bytes(4, 'little') for w in words)


def signature(words):
    """No code is stored: the number of words, the masked first word (to find candidates) and a hash of all of them."""
    import hashlib
    return {'n': len(words), 'first': f'{mask_word(words[0])[0]:08x}', 'sha1': hashlib.sha1(masked_bytes(words)).hexdigest()}


def matches_at(elf, addr, sig):
    import hashlib
    words = [elf.word(addr + 4 * i) for i in range(sig['n'])]
    return f'{mask_word(words[0])[0]:08x}' == sig['first'] and hashlib.sha1(masked_bytes(words)).hexdigest() == sig['sha1']


def entry_anchors(entry):
    return entry.get('anchors') or ([entry['anchor']] if entry.get('anchor') else [])


def load_db():
    return json.loads(SIGNATURES.read_text(encoding='utf-8')) if SIGNATURES.is_file() else {'functions': []}


def find_by_signature(elf, known, db=None):
    """known: {name: addr} already found (analyzer). Returns {name: addr} found by signatures."""
    db = db or load_db()
    found = {}
    code = elf.code_ranges()
    words = {}
    for lo, hi in code:
        for a in range(lo, hi, 4):
            words[a] = elf.word(a)
    first = {}
    for entry in db['functions']:
        if entry['name'] in known or entry['name'] in found:
            continue
        sig = entry['sig']
        hit = None
        for anchor in entry_anchors(entry):
            base = known.get(anchor['name']) or found.get(anchor['name'])
            if base is not None and matches_at(elf, base + anchor['offset'], sig):
                hit = base + anchor['offset']
                break
        if hit is not None:
            found[entry['name']] = hit
            continue
        if sig['n'] < 8:
            continue  # too short to be unique on its own: only its anchors can place it
        first = int(sig['first'], 16)
        hits = []
        for a, w in words.items():
            if mask_word(w)[0] == first and matches_at(elf, a, sig):
                hits.append(a)
                if len(hits) > 1:
                    break
        if len(hits) == 1:
            found[entry['name']] = hits[0]
    return found


def learn(elf, named, analyzer_known):
    """New signature entries for named = {name: addr} (functions identified in one game). The nearest analyzer-known
    function of the same library area (before or after it) becomes its anchor: a short function (under 8 words) is
    only ever found through its anchor, and a wrapper is often placed before the functions the analyzer knows
    (FlatOut 2's sceCdSearchFile sits 0x478 bytes before sceCdInit)."""
    out = []
    for name, addr in named.items():
        words = function_words(elf, addr)
        entry = {'name': name, 'sig': signature(words), 'learned_from': elf.path.name}
        close = sorted((abs(a - addr), a, n) for n, a in analyzer_known.items() if a != addr and abs(a - addr) < 0x4000)
        if close:
            _, a, n = close[0]
            entry['anchors'] = [{'name': n, 'offset': addr - a}]
        out.append(entry)
    return out


def bindings(repo, matches, exclude=(), extra=()):
    """matches: ['name@0xADDR', ...] from the analyzer, signatures and the profile. Returns (wrappers, entries, skipped)."""
    syscalls, stubs, todo = runtime_lists(repo)
    exclude = set(exclude) | GUEST_ALWAYS
    wrappers, entries, skipped, seen = [], [], [], set()
    for match in list(matches) + list(extra):
        name, addr = match.split('@')
        addr = int(addr, 16)
        base = name.lstrip('_')
        if name in exclude or base in exclude or base in todo or base.startswith(GUEST_PREFIXES) or addr in seen:
            skipped.append(match)
            continue
        name = base if base in syscalls | stubs else name
        ns = 'ps2_syscalls' if name in syscalls else 'ps2_stubs' if name in stubs else None
        if ns is None:
            skipped.append(match)
            continue
        seen.add(addr)
        fn = f'PortSdk_{name}_0x{addr:x}'
        wrappers.append(f'static void {fn}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {{\n'
                        f'    ctx->pc = getRegU32(ctx, 31);\n    {ns}::{name}(rdram, ctx, runtime);\n}}')
        entries.append((addr, fn))
    return wrappers, entries, skipped
