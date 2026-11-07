#!/usr/bin/env python3
"""Offline test of run 52 (devlog section Z).

    python tests/deko3d/test_run52.py

STORE: dk_run52_probe.cpp: the geometry store (glue/.../gta4_native/deko3d/dk_geometry_store.cpp, ported from nfsmw-nx)
over a simulated recorder and GPU: every range bound from the store holds exactly the host form of the guest bytes at
that moment, no block bound by a work changes before the work is done, and static data is served from the store.
RECT: dk_rect_list.h (rectangle lists as the Vulkan path): 20,000 rotated rectangles with their three corners in a
random order become the right quad, uv and colours kept.
Also re-runs test_run51.py (the NEON mixer on scattered guest memory, the NAK patch, test_run50.py and what it runs).
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
                    f'-I "{os.path.join(SDK, "thirdparty", "xxHash")}" -DXXH_INLINE_ALL '
                    f'"{os.path.join(HERE, "dk_run52_probe.cpp")}" "{os.path.join(DK, "dk_geometry_store.cpp")}" '
                    f'-o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        if p.returncode != 0 or not os.path.isfile(exe):
            sys.stdout.write(p.stdout[-4000:])
            sys.stderr.write(p.stderr[-4000:])
            failures.append('the store probe did not compile')
        else:
            out = subprocess.run([exe], capture_output=True, text=True, timeout=300).stdout
            sys.stdout.write(out)
    m = re.search(r'STORE draws (\d+) bound (\d+) wrong_content (\d+) overwritten_in_flight (\d+) '
                  r'static_hit_rate ([\d.]+) admitted (\d+) changes (\d+) evictions (\d+)', out)
    if not m:
        failures.append('no STORE line')
    else:
        if m[3] != '0':
            failures.append('a range bound from the store did not hold the guest bytes of the moment')
        if m[4] != '0':
            failures.append('a block changed while a work that bound it could still run')
        if float(m[5]) < 0.5:
            failures.append('static data is not served from the store (hit rate under 50 %)')
        if m[7] == '0':
            failures.append('the test never changed a stored range')
    m = re.search(r'RECT rounds (\d+) errors (\d+) old_rule_wrong (\d+)', out)
    if not m or m[2] != '0' or int(m[3]) == 0:
        failures.append('a rectangle list did not become its rectangle (or the test never put the corner off v0)')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run51.py')], capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or ['?'])[-1]
    print(f'test_run51.py: {tail}')
    if r.returncode != 0:
        failures.append('test_run51.py failed')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
