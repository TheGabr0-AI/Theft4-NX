#!/usr/bin/env python3
"""Offline test of run 49 (devlog section V): native_index_policy.h and native_draw_snapshot.h.

    python tests/deko3d/test_run49.py

dk_run49_probe.cpp checks the index conversion against a reference written from Gta4NativeGraphicsSystem's rules (24-bit
indices, restart only on line strips / triangle strips / fans and for 16-bit indices only with a 16-bit reset value, the
reset value turned into all ones) over 20,000 random draws, that the front end's scan gives the recorder's range, and
counts how often run 48's rules gave another vertex range. Then 10,000 random draws over fake guest buffers: the submit
hash equals the record hash when nothing changed, differs when a byte inside the range or an index changed, equals when
a byte outside the range changed, and the copy keeps the submitted bytes.
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
                    f'"{os.path.join(HERE, "dk_run49_probe.cpp")}" -o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        if p.returncode != 0 or not os.path.isfile(exe):
            print('the probe did not compile')
            return 1
        out = subprocess.run([exe], capture_output=True, text=True, timeout=300).stdout
        sys.stdout.write(out)
    failures = []
    m = re.search(r'INDEX draws (\d+) errors (\d+) scan_errors (\d+) restart_on (\d+) masked (\d+) '
                  r'run48_range_differs (\d+)', out)
    if not m:
        failures.append('no INDEX line')
    else:
        if m[2] != '0':
            failures.append('the conversion differs from the Vulkan path\'s rules')
        if m[3] != '0':
            failures.append('the front end\'s scan gives another range than the recorder\'s conversion')
        if int(m[4]) == 0 or int(m[5]) == 0:
            failures.append('restart or masking never exercised')
    s = re.search(r'SNAPSHOT rounds (\d+) false_alarms (\d+) missed_inside (\d+) outside_rounds (\d+) alarms_outside (\d+) '
                  r'copy_errors (\d+)', out)
    if not s:
        failures.append('no SNAPSHOT line')
    else:
        if s[2] != '0':
            failures.append('a hash differs with nothing changed')
        if s[3] != '0':
            failures.append('a change inside the range was not seen')
        if int(s[4]) == 0 or s[5] != '0':
            failures.append('a change outside the range raised an alarm (or was never tried)')
        if s[6] != '0':
            failures.append('the copy does not hold the submitted bytes')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
