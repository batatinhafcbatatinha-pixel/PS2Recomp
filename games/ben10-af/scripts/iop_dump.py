"""Dump emulated IOP RAM (2 MB) from a running ps2EntryRunner.exe.

Locates the RAM base by searching for the IOMGR string "data.pak" (IOMGR is
loaded at IOP 0x27500, string vaddr 0x4b20 -> IOP address 0x2c020).
Read-only; used for offline analysis of the DATA.PAK directory tree.

Usage: python scripts/iop_dump.py [OUTFILE]
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
from pathlib import Path

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
RAM_SIZE = 0x200000
IOMGR_STRING_OFFSET = 0x27500 + 0x4B20


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wt.DWORD), ('RegionSize', ctypes.c_size_t),
                ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


def pid_of(name='ps2EntryRunner.exe'):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == name.lower():
            return int(parts[1])
    return None


def regions(h):
    addr = 0
    out = []
    mbi = MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = int(mbi.BaseAddress or addr)
        size = int(mbi.RegionSize or 0)
        if size == 0:
            break
        out.append((base, size, mbi.State, mbi.Protect))
        addr = base + size
    return out


def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return b''
    return buf.raw[:got.value]


def main():
    out_path = Path(sys.argv[1] if len(sys.argv) > 1 else 'analysis/iop-ram.bin')
    pid = pid_of()
    if pid is None:
        print('ps2EntryRunner.exe not running')
        return 1
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('OpenProcess failed', ctypes.get_last_error())
        return 1
    print('pid', pid)
    base = None
    regs = regions(h)
    for a, sz, state, prot in regs:
        if state != MEM_COMMIT or (prot & (PAGE_NOACCESS | PAGE_GUARD)) or sz > 0x4000000:
            continue
        data = read(h, a, sz)
        i = data.find(b'data.pak\x00')
        if i != -1:
            base = a + i - IOMGR_STRING_OFFSET
            print('data.pak @', hex(a + i), '-> iop base', hex(base))
            break
    if base is None:
        print('IOMGR string not found')
        return 1
    blob = bytearray(RAM_SIZE)
    for a, sz, state, prot in regs:
        if state != MEM_COMMIT or (prot & (PAGE_NOACCESS | PAGE_GUARD)):
            continue
        lo, hi = max(a, base), min(a + sz, base + RAM_SIZE)
        if hi <= lo:
            continue
        data = read(h, lo, hi - lo)
        if data:
            blob[lo - base:lo - base + len(data)] = data
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    print('wrote', out_path, len(blob))
    return 0


if __name__ == '__main__':
    sys.exit(main())
