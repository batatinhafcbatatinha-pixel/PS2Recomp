"""Resume the native build until it finishes; stop only on a compiler/linker error.

Parallel jobs follow the free RAM (PORT_BUILD_JOBS overrides; about 1.6 GB per compiler, at most the CPU count);
a run that dies from lack of memory is retried with fewer jobs, down to one. Ninja resumes where it stopped.
Status lines go to analysis/build-status.txt. Run detached (Win32_Process.Create).
"""
import ctypes
import os
import re
import subprocess
import time

from local_tools import ROOT, build_location

build = build_location() / 'build/native'
status = ROOT / 'analysis/build-status.txt'
PER_JOB = 1.6
MIN_FREE = float(os.environ.get('BUILDSUP_MIN_FREE', '1.5'))


def say(text):
    with status.open('a', encoding='utf-8') as f:
        f.write(time.strftime('%H:%M:%S ') + text + '\n')


def free_gb():
    if os.name != 'nt':
        return 4.0

    class Status(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('avail', ctypes.c_ulonglong), ('pagetotal', ctypes.c_ulonglong), ('pageavail', ctypes.c_ulonglong),
                    ('virttotal', ctypes.c_ulonglong), ('virtavail', ctypes.c_ulonglong), ('extavail', ctypes.c_ulonglong)]
    s = Status()
    s.length = ctypes.sizeof(s)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s))
    return s.avail / 2**30


def ram_jobs():
    if os.environ.get('PORT_BUILD_JOBS'):
        return max(1, int(os.environ['PORT_BUILD_JOBS']))
    return max(1, min(os.cpu_count() or 1, int((free_gb() - MIN_FREE) / PER_JOB)))


jobs = ram_jobs()
for attempt in range(1, 41):
    say(f'start attempt={attempt} jobs={jobs} free={free_gb():.1f}GB')
    log = ROOT / 'analysis/build-attempt.log'
    with log.open('wb') as out:
        rc = subprocess.run(['cmake', '--build', str(build), '--target', 'ps2EntryRunner', '--parallel', str(jobs)],
                            stdout=out, stderr=subprocess.STDOUT).returncode
    text = log.read_text(errors='replace')
    with (ROOT / 'analysis/build-status.txt.log').open('a', encoding='utf-8') as f:
        f.write(text)
    if rc == 0:
        say('DONE')
        break
    memory = re.search(r'C1060|C1076|C3859|LNK1102|out of (heap|memory)', text)
    if re.search(r'error (C|LNK)\d{4}', text) and not memory:
        say(f'FAILED rc={rc} (compile error)')
        break
    jobs = max(1, min(jobs - 1 if memory else jobs, ram_jobs()))
    say(f'stopped rc={rc}; retrying')
    time.sleep(15)
else:
    say('FAILED after 40 attempts')
