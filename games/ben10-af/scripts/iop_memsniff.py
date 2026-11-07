"""Sniff process memory for IOP-RAM markers (read-only debug helper).

Usage: python scripts/iop_memsniff.py [PNAME] [PATTERN ...]
Defaults to ps2EntryRunner.exe and a few markers. Prints hits and a guessed
IOP RAM base (host address of IOP offset 0 for the IOMGR IRX load).
"""
import ctypes
import ctypes.wintypes as wt
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('BaseAddress', ctypes.c_void_p),
        ('AllocationBase', ctypes.c_void_p),
        ('AllocationProtect', wt.DWORD),
        ('RegionSize', ctypes.c_size_t),
        ('State', wt.DWORD),
        ('Protect', wt.DWORD),
        ('Type', wt.DWORD),
    ]

k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def pids_of(name):
    import subprocess
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True)
    pids = []
    for line in out.stdout.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == name.lower():
            pids.append(int(parts[1]))
    return pids


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else 'ps2EntryRunner.exe'
    pats = [p.encode() for p in sys.argv[2:]] or [b'876BCDC03DA6C663', b'data.pak\x00', b'LoadingScreens\x00']
    pids = pids_of(name)
    if not pids:
        print('no process', name)
        return
    pid = pids[0]
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('OpenProcess failed', ctypes.get_last_error())
        return
    print('pid', pid)
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION()
    hits = {p: [] for p in pats}
    scanned = 0
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        size = mbi.RegionSize
        if not size:
            break
        if (mbi.State == MEM_COMMIT and not (mbi.Protect & PAGE_NOACCESS) and not (mbi.Protect & PAGE_GUARD)
                and size <= 0x4000000):
            buf = ctypes.create_string_buffer(size)
            got = ctypes.c_size_t()
            if k32.ReadProcessMemory(h, ctypes.c_void_p(mbi.BaseAddress), buf, size, ctypes.byref(got)):
                data = buf.raw[:got.value]
                scanned += len(data)
                for p in pats:
                    i = data.find(p)
                    while i != -1:
                        hits[p].append(mbi.BaseAddress + i)
                        i = data.find(p, i + 1)
        base = int(mbi.BaseAddress or addr)
        addr = base + size
    print('scanned MB', scanned // (1024 * 1024))
    for p, hs in hits.items():
        print(p, 'hits', len(hs))
        for a in hs[:12]:
            print('  ', hex(a))
    # guess iop base from data.pak hit: IOMGR vaddr of "data.pak" is 0x4b20 (module base 0x27500)
    p = b'data.pak\x00'
    for a in hits.get(p, [])[:8]:
        print('iop base guess', hex(a - 0x27500 - 0x4b20), 'from', hex(a))


if __name__ == '__main__':
    main()
