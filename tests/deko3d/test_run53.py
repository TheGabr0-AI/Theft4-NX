#!/usr/bin/env python3
"""Offline test of run 53 (devlog section AA).

    python tests/deko3d/test_run53.py

STORE / INDEX / SKIN / LODBIAS: dk_run53_probe.cpp (see its header): the geometry store with trust per epoch, one-pass
hashes and copies keyed by guest address over guest memory in out-of-order 1 MiB slices; indices with their lo/hi from
the store; the CPU skin check on rigid and broken palettes; the LOD bias decode.
Also re-runs test_run52.py (store, rectangle lists, and through test_run51 the mixer, NAK and the older tests).
Compiled with the host clang of Visual Studio (THEFT4_VS_ROOT).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
DK = os.path.join(SDK, 'src', 'graphics', 'gta4_native', 'deko3d')


def main():
    failures = []
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
        return 2
    out = ''
    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, 'probe.exe')
        bat = os.path.join(work, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
            f.write(f'"{clang}" -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{DK}" '
                    f'-I "{os.path.dirname(DK)}" -I "{os.path.join(SDK, "thirdparty", "xxHash")}" -DXXH_INLINE_ALL '
                    f'"{os.path.join(HERE, "dk_run53_probe.cpp")}" "{os.path.join(DK, "dk_geometry_store.cpp")}" '
                    f'-o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        if p.returncode != 0 or not os.path.isfile(exe):
            sys.stdout.write(p.stdout[-4000:])
            sys.stderr.write(p.stderr[-4000:])
            failures.append('the store probe did not compile')
        else:
            out = subprocess.run([exe], capture_output=True, text=True, timeout=300).stdout
            sys.stdout.write(out)
    checks = [
        (r'STORE draws (\d+) bound (\d+) wrong (\d+) overwritten (\d+) copy_bound (\d+) trusted (\d+)',
         lambda m: m[3] == '0' and m[4] == '0' and int(m[5]) > 0 and int(m[6]) > 0,
         'the store bound wrong or overwritten bytes, or never served a copy or a trusted hit'),
        (r'INDEX lookups (\d+) hits (\d+) wrong_bytes (\d+) wrong_range (\d+)',
         lambda m: int(m[2]) > 0 and m[3] == '0' and m[4] == '0', 'indices from the store differ from the conversion'),
        (r'SKIN rounds (\d+) errors (\d+) flagged_good (\d+) missed_bad (\d+)',
         lambda m: m[2] == '0' and m[3] == '0' and m[4] == '0', 'the CPU skin check is wrong on rigid or broken palettes'),
        (r'LODBIAS values (\d+) errors (\d+)', lambda m: m[2] == '0', 'the LOD bias decode is wrong'),
    ]
    for pattern, good, message in checks:
        m = re.search(pattern, out)
        if not m or not good(m):
            failures.append(message)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run52.py')], capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or ['?'])[-1]
    print(f'test_run52.py: {tail}')
    if r.returncode != 0:
        failures.append('test_run52.py failed')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
