#!/usr/bin/env python3
"""Offline test of run 51 (devlog section Y).

    python tests/deko3d/test_run51.py

MIXER: dk_run51_probe.cpp: the NEON voice loop (tools/switch-game/nx_audio_mixer.h, simde's NEON emulation on the PC)
against a transliteration of the generated EU sub_8216DBB0 over 4,000 random voices (500 with source and destination
overlapping): guest memory equal byte for byte.
NAK: tools/switch-game/nak_flip_patch.py on the portlibs libvulkan.a (DEVKITPRO, default C:/devkitPro): the IsNum /
IsNan entries of the 8 set-op legalize jump tables patched to OrdEq's target, 16 bytes changed and nothing else, the
Maxwell ones (sm20 FSet/FSetP, which sm50 shares, and sm50 DSetP) among them. Skipped (not failed) without the archive.
Also re-runs test_run50.py (which re-runs test_run49.py, test_constant_diff.py, test_packet_ring.py).
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
SWITCH_GAME = os.path.join(ROOT, 'tools', 'switch-game')


def run_probe(failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('THEFT4_VS_ROOT: no Visual Studio with clang found; the mixer probe did not run')
        return
    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, 'probe.exe')
        bat = os.path.join(work, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@echo off\r\n')
            f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
            f.write(f'"{clang}" -std=c++20 -O2 -mssse3 -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -Wno-unused-parameter '
                    f'-I "{SWITCH_GAME}" -I "{os.path.join(SDK, "thirdparty", "simde")}" '
                    f'"{os.path.join(HERE, "dk_run51_probe.cpp")}" -o "{exe}"\r\n')
        p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
        if p.returncode != 0 or not os.path.isfile(exe):
            sys.stdout.write(p.stdout[-4000:])
            sys.stderr.write(p.stderr[-4000:])
            failures.append('the mixer probe did not compile')
            return
        out = subprocess.run([exe], capture_output=True, text=True, timeout=300).stdout
        sys.stdout.write(out)
    m = re.search(r'MIXER rounds (\d+) overlapping (\d+) errors (\d+)', out)
    if not m or m[3] != '0' or int(m[1]) < 4000 or int(m[2]) == 0:
        failures.append('the NEON mixer differs from the generated sub_8216DBB0')


def run_nak(failures):
    lib = os.path.join(os.environ.get('DEVKITPRO_WIN', r'C:\devkitPro'), 'portlibs', 'switch', 'lib', 'libvulkan.a')
    if not os.path.isfile(lib):
        print(f'NAK skipped: {lib} not found')
        return
    with tempfile.TemporaryDirectory() as work:
        out = os.path.join(work, 'libvulkan.a')
        p = subprocess.run([sys.executable, os.path.join(SWITCH_GAME, 'nak_flip_patch.py'), lib, out],
                           capture_output=True, text=True)
        report = p.stdout
        if p.returncode != 0 or not os.path.isfile(out):
            failures.append('nak_flip_patch.py failed: ' + report[-500:])
            return
        a = open(lib, 'rb').read()
        b = open(out, 'rb').read()
        changed = [i for i in range(len(a)) if a[i] != b[i]] if len(a) == len(b) else None
    patched = report.count('(panic) ->')
    maxwell = sum(1 for line in report.splitlines()
                  if '(panic) ->' in line and ('4sm20' in line or '4sm50' in line))
    refused = report.count('NOT patched')
    print(f'NAK patched {patched} tables (Maxwell {maxwell}), refused {refused}, '
          f'bytes changed {len(changed) if changed is not None else "size differs"}')
    if changed is None or len(changed) != 2 * patched or patched != 8 or maxwell != 3 or refused:
        failures.append('the NAK flip patch did not change exactly the IsNum/IsNan bytes of the 8 tables')


def main():
    failures = []
    run_probe(failures)
    run_nak(failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run50.py')], capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or ['?'])[-1]
    print(f'test_run50.py: {tail}')
    if r.returncode != 0:
        failures.append('test_run50.py failed')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
