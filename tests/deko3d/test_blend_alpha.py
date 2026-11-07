#!/usr/bin/env python3
"""Offline test of run 48 (devlog section T): dk_blend_alpha.h, deko3d 0.5.0's alpha destination factor.

    python tests/deko3d/test_blend_alpha.py

1. The installed libdeko3d.a and libdeko3dd.a (the console's): dkCmdBufBindBlendStates reads the DkBlendState fields at
   bits 0, 3, 9, 15 and 18 and never bit 24 (dstAlphaBlendFactor): the bug the fix is for. Skipped without objdump.
2. The probe (dk_run48_blend_probe.cpp): for 8 targets x 19 x 19 destination factors x 4 variations, the registers the
   library writes followed by DkBlendAlphaDstWords equal the six registers banjo-nx's plume back end writes; the library
   alone differs exactly when the two destination factors differ.
Compiled with the host clang of Visual Studio (THEFT4_VS_ROOT), like the other tests of this folder.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native', 'deko3d')
OBJDUMP = os.environ.get('THEFT4_A64_OBJDUMP', r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-objdump.exe')
LIBS = [r'C:\devkitPro\libnx\lib\libdeko3d.a', r'C:\devkitPro\libnx\lib\libdeko3dd.a']


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dk_run48_blend_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{DK}" '
                f'"{os.path.join(HERE, "dk_run48_blend_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def library_fields(lib):
    """The bit positions dkCmdBufBindBlendStates extracts from the packed DkBlendState (ubfx lsb, and the and #0x7)."""
    p = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', lib], capture_output=True, text=True)
    if p.returncode != 0:
        return None
    m = re.search(r'<dkCmdBufBindBlendStates>:\n(.*?)\n\n', p.stdout, re.S)
    if not m:
        return None
    body = m[1]
    lsbs = {int(x) for x in re.findall(r'ubfx\s+\w+, \w+, #(\d+), #\d+', body)}
    if re.search(r'and\s+w\d+, w\d+, #0x7\b', body):
        lsbs.add(0)
    return lsbs


def main():
    failures = []
    if os.path.isfile(OBJDUMP):
        for lib in LIBS:
            fields = library_fields(lib)
            name = os.path.basename(lib)
            if fields is None:
                failures.append(f'{name}: dkCmdBufBindBlendStates not found')
                continue
            print(f'LIBRARY {name}: fields read at bits {sorted(fields)}')
            if 24 in fields:
                print(f'  {name} reads dstAlphaBlendFactor: this library is fixed (the run-48 write is then redundant)')
            elif not {0, 3, 9, 15, 18} <= fields:
                failures.append(f'{name}: unexpected field set {sorted(fields)}')
    else:
        print(f'no objdump at {OBJDUMP}: library check skipped')
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; the probe did not run')
            return 2
        out = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout
        sys.stdout.write(out)
        m = re.search(r'BLEND cases (\d+) ours_wrong (\d+) library_wrong (\d+) expected (\d+) malformed (\d+)', out)
        if not m:
            failures.append('no BLEND line')
        else:
            cases, ours, lib, expected, malformed = map(int, m.groups())
            if cases != 8 * 19 * 19 * 4 or ours or malformed:
                failures.append(f'ours differs from plume in {ours} of {cases} cases ({malformed} malformed)')
            if lib != expected:
                failures.append(f'the library model differs from plume in {lib} cases, expected {expected}')
        if 'WORDS target 3 ConstAlpha 2001079E 0000C003' not in out:
            failures.append('words for target 3, ConstAlpha are not 2001079E 0000C003')
    if failures:
        print('FAIL: ' + '; '.join(failures))
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
