#!/usr/bin/env python3
"""Offline test of dk_dksh.h (the per-warp scratch the console reads from a DKSH before loading it).

    python tests/deko3d/test_dksh_scratch.py [--real <deko3d-out dir>]

Compiles tests/deko3d/dksh_scratch_probe.cpp with the host clang of Visual Studio (THEFT4_VS_ROOT, as
test_runtime_package.py), runs the synthetic cases, and with --real checks every DKSH of the game's package (made on
the PC, never in the repository) against an independent Python reading of the same bytes: the counts of programs
over 2048 bytes (deko3d's default) and over the queue's size, and the largest value, must be equal.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'deko3d'))
import gta4_deko3d_runtime_check as fmt  # noqa: E402

DK_DIR = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native', 'deko3d')
QUEUE = 96 << 10

EXPECTED = {
    'zero': (1, 0), 'default_queue': (1, 2048), 'largest_seen': (1, 92672), 'exact_queue': (1, QUEUE),
    'over_queue': (1, QUEUE + 512), 'two_programs': (1, 512), 'no_programs': (0, 0), 'bad_magic': (0, 0),
    'truncated_header': (0, 0), 'empty': (0, 0), 'field_ends_at_control_end': (1, 7),
    'field_past_control_end': (0, 0), 'programs_off_huge': (0, 0), 'control_past_file': (0, 0),
}


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dksh_scratch_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-parameter '
                f'-I "{os.path.join(HERE, "stubs")}" -I "{DK_DIR}" "{os.path.join(HERE, "dksh_scratch_probe.cpp")}" '
                f'"{os.path.join(DK_DIR, "dk_shader_package.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def python_scan(path):
    """Counts from the package bytes without the C++ reader: (blobs, over_2048, over_queue, max)."""
    info = fmt.check_file(path)
    with open(path, 'rb') as f:
        data = f.read()
    blobs_at = info['blobs_at']
    over_2048 = over_queue = peak = 0
    for b in range(info['blobs']):
        off, _ = struct.unpack_from('<II', data, blobs_at + 8 * b)
        magic, _, control, _, programs_off, programs = struct.unpack_from('<6I', data, off)
        assert magic == 0x48534B44 and programs >= 1
        scratch = struct.unpack_from('<I', data, off + programs_off + 20)[0]
        over_2048 += scratch > 2048
        over_queue += scratch > QUEUE
        peak = max(peak, scratch)
    return info['blobs'], over_2048, over_queue, peak


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--real', help='folder with the game\'s gta4_deko3d_runtime.bin (version 2)')
    args = ap.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
            return 2
        out = subprocess.run([exe, 'synthetic'], capture_output=True, text=True, timeout=60).stdout
        seen = {}
        for m in re.finditer(r'CASE (\S+) ok=(\d) scratch=(\d+)', out):
            seen[m[1]] = (int(m[2]), int(m[3]))
        for name, want in EXPECTED.items():
            if seen.get(name) != want:
                failures.append(f'synthetic {name}: got {seen.get(name)}, expected {want}')
        print(f'synthetic cases: {len(EXPECTED) - sum(1 for f in failures if f.startswith("synthetic"))}/{len(EXPECTED)} as expected')
        if args.real:
            blobs, o2048, oq, peak = python_scan(os.path.join(args.real, 'gta4_deko3d_runtime.bin'))
            res = subprocess.run([exe, args.real], capture_output=True, text=True, timeout=600).stdout
            m = re.search(r'PACKAGE blobs=(\d+) parsed=(\d+) failed=(\d+) over_2048=(\d+) over_queue=(\d+) max=(\d+)', res)
            if not m:
                failures.append(f'real: probe printed no PACKAGE line: {res[-300:]}')
            else:
                got = tuple(int(x) for x in m.groups())
                want = (blobs, blobs, 0, o2048, oq, peak)
                print(f'real package: C++ {got} / Python {want}  (blobs, parsed, failed, over 2048, over queue, max)')
                if got != want:
                    failures.append(f'real: C++ {got} != Python {want}')
                if oq:
                    failures.append(f'real: {oq} programs need more than the queue gives ({QUEUE} B)')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
