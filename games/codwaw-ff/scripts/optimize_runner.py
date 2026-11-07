"""Writes an optimized copy of the recompiled functions: project/runner -> project/runner-opt.

The recompiler stores ctx->pc before every instruction and gives every instruction a label and a case in the entry switch,
so any instruction can be entered and the C++ compiler can optimize nothing across instructions. Almost none of that is
used. This pass keeps what can be observed:
  - a label stays only where execution can really enter: goto targets, return addresses, whatever follows an instruction
    that is not plain data flow, delay slots, and every address another function names. Functions with indirect jumps
    (jump tables) keep all their labels.
  - the pc store goes from instructions that only compute and access memory. Their memory accesses store the pc on the slow
    path, so a hardware register access sees the same ctx->pc as before.
  - memory accesses test one range inline; everything else goes out of line.
An entry at an address without a label stops with [opt:missing-label] (ps2_runner_opt.h) instead of running the wrong code.

usage: python -I optimize_runner.py [--stats] [--apply]
  --stats  only report; --apply also points project/runner-chunks at runner-opt (the originals stay in runner-chunks.orig).
"""
import collections
import multiprocessing
import pathlib
import re
import shutil
import struct
import sys

root = pathlib.Path(__file__).resolve().parent.parent / 'project'
src = root / 'runner'
out = root / 'runner-opt'
LABEL = re.compile(r'^label_([0-9a-f]+):\s*$')
CASE = re.compile(r'^\s*case 0x([0-9a-f]+)u: goto label_[0-9a-f]+;\s*$')
GOTO = re.compile(r'goto label_([0-9a-f]+);')
PC_STORE = re.compile(r'^    ctx->pc = 0x([0-9a-fA-F]+)u;\s*$')
PC_DEEP = re.compile(r'^\s{8,}ctx->pc = 0x[0-9a-fA-F]+u;\s*$')
PC_ANY = re.compile(r'ctx->pc (?:=|==|!=) 0x([0-9a-fA-F]+)u')
DISPATCH = re.compile(r'dispatchGuestBranch\(rdram, ctx, (0x[0-9a-fA-F]+u?|[A-Za-z_]\w*), 0x[0-9a-fA-F]+u, 0x([0-9a-fA-F]+)u')
IDENT = re.compile(r'\b[A-Za-z_][A-Za-z_0-9]*\b')
MEM = re.compile(r'\b(READ8|READ16|READ32|READ64|READ128|WRITE8|WRITE16|WRITE32|WRITE64|WRITE128)\(')
# Plain data flow: these names neither read ctx->pc nor run guest or kernel code.
PURE_WORDS = set('''
ctx f r hi lo sa fcr31 fcr0 uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t float double bool bits const static_cast
sizeof std memcpy auto true false void __m128i tmp value val result addr
'''.split())
PURE_PREFIX = ('GPR_', 'SET_GPR_', 'READ', 'WRITE', 'ADD32', 'SUB32', 'AND', 'OR', 'XOR', 'NOR', 'SLL', 'SRL', 'SRA', 'FPU_', 'MUL', 'DIV', 'SLT',
               'SLTU', 'MOVN', 'MOVZ', 'PS2_', 'ps2_', '_mm_', 'EXTEND', 'CLZ', 'LUI')


def segments(lines):
    """(start of the body, [(address, first line, end line)])"""
    first = next((i for i, l in enumerate(lines) if LABEL.match(l)), None)
    if first is None:
        return None, []
    marks = [(i, int(LABEL.match(l).group(1), 16)) for i, l in enumerate(lines) if i >= first and LABEL.match(l)]
    return first, [(a, i, marks[k + 1][0] if k + 1 < len(marks) else len(lines)) for k, (i, a) in enumerate(marks)]


def pure(body, words):
    """body: the lines of an instruction after its pc store"""
    for line in body:
        code = line.split('//')[0]
        if not code.strip():
            continue
        for bad in ('goto', 'return', 'throw', 'runtime', 'ctx->pc', 'dispatch', 'eeCheckpoint', 'if (', 'while', 'for ('):
            if bad in code:
                return False
        for word in IDENT.findall(code):
            if word in PURE_WORDS or word.startswith(PURE_PREFIX) or re.fullmatch(r'0[xX][0-9a-fA-F]+[uU]?L*|[0-9]+[uUlLfF]*', word):
                continue
            words[word] += 1
            return False
    return True


def code_addresses():
    """Addresses the game can compute: words of the executable that point into its code, and lui + addiu/ori pairs in the code.
    A real function can sit inside a region the recompiler treated as one function and be reached only through such a pointer.
    False positives only keep a label."""
    elf = (root.parent / 'game_files/SLUS_217.46').read_bytes()
    phoff, phsize, phnum = struct.unpack_from('<I', elf, 28)[0], struct.unpack_from('<H', elf, 42)[0], struct.unpack_from('<H', elf, 44)[0]
    segments_ = []
    for i in range(phnum):
        kind, offset, vaddr, _, filesz, _, flags, _ = struct.unpack_from('<8I', elf, phoff + i * phsize)
        if kind == 1 and filesz:
            segments_.append((offset, vaddr, filesz, flags))
    code = [(s[1], s[1] + s[2]) for s in segments_ if s[3] & 1]
    inside = lambda a: any(lo <= a < hi for lo, hi in code)
    found = set()
    for offset, _, filesz, flags in segments_:
        for word, in struct.iter_unpack('<I', elf[offset:offset + (filesz & ~3)]):
            if not word & 3 and inside(word):
                found.add(word)
        if flags & 1:
            high = {}
            for index, (word,) in enumerate(struct.iter_unpack('<I', elf[offset:offset + (filesz & ~3)])):
                op, rs, rt, imm = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 0xFFFF
                if op == 0x0F:
                    high[rt] = (imm << 16, index)
                elif op in (0x09, 0x0D, 0x19) and rs in high and index - high[rs][1] <= 64:  # addiu, ori, daddiu
                    value = (high[rs][0] | imm if op == 0x0D else high[rs][0] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
                    if not value & 3 and inside(value):
                        found.add(value)
    return found


def strip_delay_slots(lines, stats):
    """in_delay_slot and branch_pc matter only to an exception raised in the delay slot, and those come from memory accesses
    and system calls. A delay slot of plain arithmetic cannot raise one."""
    ignored = collections.Counter()
    remove = set()
    for i, line in enumerate(lines):
        if line.strip() != 'ctx->in_delay_slot = true;' or i + 2 >= len(lines) or not lines[i + 1].strip().startswith('ctx->branch_pc = '):
            continue
        end = next((j for j in range(i + 2, min(i + 8, len(lines))) if lines[j].strip() == 'ctx->in_delay_slot = false;'), None)
        if end is None:
            continue
        body = lines[i + 2:end]
        if any('READ' in b.split('//')[0] or 'WRITE' in b.split('//')[0] for b in body) or not pure(body, ignored):
            continue
        remove.update((i, i + 1, end))
        if PC_DEEP.match(lines[i - 1]):
            remove.add(i - 1)
        stats['delay-slots-stripped'] += 1
    return [l for k, l in enumerate(lines) if k not in remove]


def scan(path):
    """Addresses this file names, for every other file."""
    names = set()
    text = path.read_text(errors='replace')
    for m in PC_ANY.finditer(text):
        names.add(int(m.group(1), 16))
    for m in DISPATCH.finditer(text):
        if m.group(1).startswith('0x'):
            names.add(int(m.group(1).rstrip('u'), 16))
        names.add(int(m.group(2), 16))
    return names


def named_elsewhere(path):
    text = path.read_text(errors='replace')
    names = set()
    # only stores inside branch sequences (deeper indentation) and dispatch arguments name an entry
    for m in re.finditer(r'^\s{8,}ctx->pc = 0x([0-9a-fA-F]+)u;', text, re.M):
        names.add(int(m.group(1), 16))
    for m in DISPATCH.finditer(text):
        if m.group(1).startswith('0x'):
            names.add(int(m.group(1).rstrip('u'), 16))
        names.add(int(m.group(2), 16))
    return names


def transform(args):
    path, external, write = args
    lines = path.read_text(errors='replace').split('\n')
    stats = collections.Counter()
    words = collections.Counter()
    first, segs = segments(lines)
    if first is None:
        if write:
            (out / path.name).write_text('\n'.join(lines), encoding='utf-8')
        return stats, words
    text = '\n'.join(lines)
    indirect = 'GuestBranchKind::IndirectJump' in text or 'GuestBranchKind::Jump' in text or '"JR"' in text
    entry = segs[0][0]
    simple = {}
    for address, start, end in segs:
        chunk = lines[start + 1:end]
        store = next((k for k, l in enumerate(chunk) if PC_STORE.match(l)), None)
        before = [l for l in chunk[:store]] if store is not None else chunk
        ok = store is not None and all(l.strip().startswith('//') or not l.strip() for l in before) and \
             int(PC_STORE.match(chunk[store]).group(1), 16) == address and pure(chunk[store + 1:], words)
        simple[address] = ok
    keep = {entry}
    keep.update(int(a, 16) for a in GOTO.findall('\n'.join(lines[first:])))
    for m in re.finditer(r'^\s{8,}ctx->pc = 0x([0-9a-fA-F]+)u;', text, re.M):
        keep.add(int(m.group(1), 16))
    for m in re.finditer(r'ctx->pc == 0x([0-9a-fA-F]+)u', text):
        keep.add(int(m.group(1), 16))
    for m in DISPATCH.finditer(text):
        keep.add(int(m.group(2), 16))
    order = [s[0] for s in segs]
    for k, (address, start, end) in enumerate(segs):
        # nothing falls through after an unconditional jump: what follows it, past the delay slot, is entered some other way
        comment = lines[start + 1] if start + 1 < end else ''
        if re.search(r'\s(jr|j|b|eret)\s', comment):
            keep.update((address + 8, address + 12, address + 16))
    for k, address in enumerate(order):
        if not simple[address]:
            keep.add(address)
            if k + 1 < len(order):
                keep.add(order[k + 1])
        if address in external:
            keep.add(address)
    if indirect:
        keep.update(order)
        stats['indirect-functions'] += 1
    output = []
    drop = set()
    for address, start, end in segs:
        chunk = lines[start + 1:end]
        if address not in keep:
            drop.add(start)
            stats['labels-removed'] += 1
        else:
            stats['labels-kept'] += 1
        if simple[address]:
            stats['pc-stores-removed'] += 1
            store = next(k for k, l in enumerate(chunk) if PC_STORE.match(l))
            drop.add(start + 1 + store)
            for k in range(store + 1, len(chunk)):
                lines[start + 1 + k] = MEM.sub(lambda m: f'OPT_{m.group(1)}(0x{address:x}u, ', lines[start + 1 + k])
        else:
            stats['pc-stores-kept'] += 1
    for i, line in enumerate(lines):
        if i in drop:
            continue
        if i < first:
            m = CASE.match(line)
            if m and int(m.group(1), 16) not in keep:
                continue
            if line.strip() == 'default: break;':
                line = f'        default: Ps2OptNoLabel(ctx, 0x{entry:x}u, 0x{order[-1]:x}u); break;'
            if line.startswith('#include "ps2_runtime_macros.h"'):
                output.append(line)
                output.append('#include "ps2_runtime.h"')
                output.append('#include "ps2_runner_opt.h"')
                continue
        output.append(line)
    output = strip_delay_slots(output, stats)
    if write:
        (out / path.name).write_text('\n'.join(output), encoding='utf-8')
    return stats, words

def main():
    write = '--stats' not in sys.argv
    files = sorted(src.glob('*.cpp'))
    print(len(files), 'files')
    with multiprocessing.Pool(6) as pool:
        external = set()
        for names in pool.imap_unordered(named_elsewhere, files, chunksize=200):
            external |= names
        print('addresses named', len(external))
        computed = code_addresses()
        print('addresses the game can compute', len(computed))
        external |= computed
        if write:
            out.mkdir(exist_ok=True)
            shutil.copy(pathlib.Path(__file__).resolve().parent / 'ps2_runner_opt.h', out / 'ps2_runner_opt.h')
        total = collections.Counter(); words = collections.Counter()
        for stats, used in pool.imap_unordered(transform, [(f, external, write) for f in files], chunksize=200):
            total.update(stats); words.update(used)
    print(dict(total))
    print('names that kept an instruction impure:', words.most_common(50))
    if '--apply' in sys.argv:
        chunks = root / 'runner-chunks'
        backup = root / 'runner-chunks.orig'
        if not backup.exists():
            shutil.copytree(chunks, backup)
        for f in sorted(backup.glob('*.cpp')):
            (chunks / f.name).write_text(f.read_text().replace('"../runner/', '"../runner-opt/'), encoding='utf-8')
        print('chunks now use runner-opt')


if __name__ == '__main__':
    main()
