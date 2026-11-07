#!/usr/bin/env python3
"""Offline test of run 50 (devlog section W): dk_run50_probe.cpp.

    python tests/deko3d/test_run50.py

FILE: core/host_file_share.h against a model of Horizon's locking (a file open for writing refuses a second open and
stat with EIO): run 49's save sequence throws with the old check (the model reproduces the crash) and keeps the entry
with the new one; the second handle shares the descriptor, closed once with the last handle.
CONST: the front end's DiffRegisters and the recorder's ApplyConstantDelta over 12,800 random draws: ConstantBankHash of
both sides equal every time; a dropped delta is caught in the region it touched; each draw's version holds the bank of
its submit when read after the work was recorded.
INDEX: the branch-free paths of ConvertIndices / ScanIndices against a reference over 20,000 random draws.
POOL: 20,000 copies through DrawDataPool, each equal field by field to a fresh copy; the pool recycles and is bounded.
Also re-runs test_run49.py (the general index paths and the snapshot), test_constant_diff.py and test_packet_ring.py.
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
GTA = os.path.join(SDK, 'src', 'graphics', 'gta4_native')


def main():
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
        return 2
    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, 'probe.exe')
        bat = os.path.join(work, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
            f.write(f'"{clang}" -std=c++23 -O2 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{GTA}" '
                    f'-I "{os.path.join(SDK, "include")}" "{os.path.join(HERE, "dk_run50_probe.cpp")}" -o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        if p.returncode != 0 or not os.path.isfile(exe):
            print('the probe did not compile')
            return 1
        out = subprocess.run([exe], capture_output=True, text=True, timeout=300).stdout
        sys.stdout.write(out)
    failures = []
    m = re.search(r'FILE errors (\d+)', out)
    if not m or m[1] != '0':
        failures.append('host_file_share: the save sequence or the descriptor sharing is wrong')
    m = re.search(r'CONST draws (\d+) hash_mismatch (\d+) drops (\d+) caught (\d+) region_right (\d+) '
                  r'version_wrong (\d+) single_block_wrong (\d+)', out)
    if not m:
        failures.append('no CONST line')
    else:
        if m[2] != '0':
            failures.append('the bank hashes differ with nothing lost (a false alarm on the console)')
        if int(m[3]) == 0 or m[4] != m[3]:
            failures.append('a dropped delta was not caught')
        if m[5] != m[3]:
            failures.append('a dropped delta was reported in the wrong region')
        if m[6] != '0':
            failures.append('a version does not hold the bank of its draw')
    m = re.search(r'INDEX rounds (\d+) errors (\d+)', out)
    if not m or m[2] != '0':
        failures.append('the branch-free index paths differ from the reference')
    m = re.search(r'POOL rounds (\d+) errors (\d+)', out)
    if not m or m[2] != '0':
        failures.append('a recycled copy differs from a fresh one, or the pool is not bounded')
    for other in ('test_run49.py', 'test_constant_diff.py', 'test_packet_ring.py'):
        r = subprocess.run([sys.executable, os.path.join(HERE, other)], capture_output=True, text=True)
        tail = (r.stdout.strip().splitlines() or ['?'])[-1]
        print(f'{other}: {tail}')
        if r.returncode != 0:
            failures.append(f'{other} failed')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
