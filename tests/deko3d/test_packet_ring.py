#!/usr/bin/env python3
"""Offline test of run 46 (devlog section R): native_inline_vector.h and native_packet_ring.h.

    python tests/deko3d/test_packet_ring.py

dk_run46_ring_probe.cpp checks the inline vector against std::vector (2,000 rounds, spills included), sends 2,000,000
RecorderDraw packets with their constant deltas (DiffRegisters) and a Present every 1,000 through a ring of 64 slots between two
threads (order, constants checksum, every packet arrives; the ring fills), counts the allocations per packet in the steady state
on both threads (expected 0 for deltas of up to 16 registers), and checks that a stopped ring refuses a waiting push.
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
            f.write(f'"{clang}" -std=c++23 -O2 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{GTA}" -I "{os.path.join(SDK, "include")}" '
                    f'"{os.path.join(HERE, "dk_run46_ring_probe.cpp")}" -o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        if p.returncode != 0 or not os.path.isfile(exe):
            print('the probe did not compile')
            return 1
        out = subprocess.run([exe], capture_output=True, text=True, timeout=300).stdout
        sys.stdout.write(out)
    m = re.search(r'RING inline_vector_errors (\d+) packets (\d+) errors (\d+) checksum_equal (\d) full_waits (\d+) '
                  r'producer_steady_allocations (\d+) consumer_steady_allocations (\d+) stopped_push_refused (\d) '
                  r'packet_bytes (\d+)', out)
    failures = []
    if not m:
        failures.append('no result line')
    else:
        if m[1] != '0':
            failures.append('the inline vector differs from std::vector')
        if m[3] != '0' or m[4] != '1':
            failures.append('packets lost, out of order or with other constants')
        if int(m[5]) == 0:
            failures.append('the ring never filled: the waiting push was not exercised')
        if m[6] != '0' or m[7] != '0':
            failures.append('allocations per packet in the steady state')
        if m[8] != '1':
            failures.append('a stopped ring took a push')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
