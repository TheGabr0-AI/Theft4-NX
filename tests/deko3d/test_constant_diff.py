#!/usr/bin/env python3
"""Offline test of run 45 (devlog section P): native_constant_diff.h, the constants diff on raw 64-byte groups.

    python tests/deko3d/test_constant_diff.py

The old register-by-register loop (kept in the header as DiffRegistersReference) and the new one run on 8,000 sequenced draws
of both banks (256 vertex and 224 pixel registers; draws that change nothing, a byte, a register, a block of four, twenty bytes
or the whole bank; the first draw of a device sends the whole bank): the runs, the values and the number of registers are equal
for every draw, and the new shadow is the guest's bytes. Compiled with the host clang of Visual Studio (THEFT4_VS_ROOT).
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
            f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{GTA}" -I "{os.path.join(SDK, "include")}" '
                    f'"{os.path.join(HERE, "dk_run45_constdiff_probe.cpp")}" -o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        if p.returncode != 0 or not os.path.isfile(exe):
            print('the probe did not compile')
            return 1
        out = subprocess.run([exe], capture_output=True, text=True, timeout=120).stdout
        sys.stdout.write(out)
    m = re.search(r'CONSTDIFF draws (\d+) differences (\d+) shadow_wrong (\d+) registers (\d+) runs (\d+)', out)
    failures = []
    if not m or m[1] != '8000' or m[2] != '0' or m[3] != '0' or int(m[4]) == 0:
        failures.append('the new diff differs from the old loop')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())