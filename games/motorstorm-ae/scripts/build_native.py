"""Build native targets without executing the game; bound disk use while compiling."""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_tools import build_location

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('MSAE_BUILD_ROOT', str(build_location() / 'build/native'))).resolve()
GIB = 1024 ** 3
BUDGET = 16 * GIB  # a full build with the generated game code


def size():
    return sum(p.stat().st_size for p in BUILD.rglob('*') if p.is_file()) if BUILD.exists() else 0


def guard():
    if size() >= BUDGET:
        raise RuntimeError(f'Native build reached its {BUDGET // GIB} GiB budget')


def run(args, log):
    guard()
    with (ROOT / 'analysis' / log).open('w', encoding='utf-8') as output:
        child = subprocess.Popen([str(a) for a in args], stdout=output, stderr=subprocess.STDOUT)
        try:
            while child.poll() is None:
                guard()
                time.sleep(2)
        except BaseException:
            subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], capture_output=True)
            child.wait()
            raise
        if child.returncode:
            raise RuntimeError(f'Build command failed ({child.returncode}); see analysis/{log}')
    guard()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', default='ps2EntryRunner')
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--jobs', default='2', help='parallel compile jobs')
    args = parser.parse_args()
    subprocess.run([sys.executable, str(ROOT / 'scripts/bucket_dispatch.py')], check=True)
    prebuilt = os.environ.get('MSAE_PREBUILT_AOT', '')
    if not prebuilt:
        subprocess.run([sys.executable, str(ROOT / 'scripts/check_build_space.py')], check=True)
    guard()
    (ROOT / 'analysis').mkdir(exist_ok=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    temporary = BUILD / 'compiler-temp'
    temporary.mkdir(exist_ok=True)
    os.environ['TMP'] = os.environ['TEMP'] = str(temporary)
    configure = ['cmake', '-S', ROOT / 'project', '-B', BUILD, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 '-DMSAE_PREBUILT_AOT=' + prebuilt]
    run(configure,
        'native-configure.log')
    if not args.configure_only:
        run(['cmake', '--build', BUILD, '--target', args.target, '--parallel', str(args.jobs)], 'native-build.log')
    print(f'Native build bytes: {size()}; game was not executed')


if __name__ == '__main__':
    main()
