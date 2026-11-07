#!/usr/bin/env python3
"""Offline test of run 42 (devlog section M): dk_view_swizzle.h.

    python tests/deko3d/test_view_swizzle.py

1. ComposeViewSwizzle (moved out of dk_recorder.cpp) equals the ViewSwizzle of commit 17f3a18 for all 4,096 fetch swizzles
   and four host channel tables; the reference text in the probe is checked verbatim against `git show 17f3a18`.
2. A texture read from guest memory still gets what it got: A8R8G8B8 (0x60A) gives R, G, B, A from the stored B, G, R, A;
   DXT (0x688) is as stored; single-channel formats still replicate.
3. A resolved colour image is viewed as stored whatever the title's fetch swizzle: the same R, G, B, A the console gets
   from memory order + 0x60A, and the old behaviour (0x60A on the host image) is the red/blue exchange of run 41.
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
HEAD_FILE = 'glue/rexglue-sdk-main/src/graphics/gta4_native/deko3d/dk_recorder.cpp'
BASE_COMMIT = '17f3a18'


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dk_run42_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{DK}" '
                f'"{os.path.join(HERE, "dk_run42_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def squash(text):
    return re.sub(r'\s+', ' ', text).strip()


def reference_check():
    """The 17f3a18 ViewSwizzle body must be what the probe's reference computes (same steps, codes written as numbers)."""
    p = subprocess.run(['git', '-C', ROOT, 'show', f'{BASE_COMMIT}:{HEAD_FILE}'], capture_output=True, text=True,
                       encoding='utf-8')
    if p.returncode != 0:
        return f'git show {BASE_COMMIT} failed: {p.stderr.strip()[:200]}'
    base = p.stdout
    i = base.index('void ViewSwizzle(uint32_t guest, uint16_t host, DkImageSwizzle out[4]) {')
    body = squash(base[i:base.index('\n}\n', i) + 3])
    needed = ['(guest >> (i * 3)) & 0x7', 'if (v <= 3)', '(host >> (v * 3)) & 0x7', 'else if (v == 4)',
              'DkImageSwizzle_Zero', 'DkImageSwizzle_One']
    missing = [n for n in needed if squash(n) not in body]
    return None if not missing else 'the 17f3a18 ViewSwizzle lacks: ' + ', '.join(missing)


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
            return 2
        out = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout
        sys.stdout.write(out)
        m = re.search(r'COMPOSE cases (\d+) differences (\d+)', out)
        if not m or m[2] != '0' or m[1] != '16384':
            failures.append('compose differs from the 17f3a18 ViewSwizzle')
        for key in ('GUESTMEM 0x60A', 'DXT 0x688', 'R8 0x688'):
            line = next((l for l in out.splitlines() if l.startswith(key)), '')
            if not line.endswith('ok'):
                failures.append(key + ': ' + (line or 'no line'))
        m = re.search(r'RESOLVED cases (\d+) bad (\d+)', out)
        if not m or m[2] != '0' or m[1] != '8000':
            failures.append('resolved colour views are not as stored')
        if 'OLD resolved 0x60A: seen 30 120 200 255 swapped' not in out:
            failures.append('the old behaviour is not the red/blue exchange')
    problem = reference_check()
    if problem:
        print('reference text: ' + problem)
        failures.append(problem)
    else:
        print(f'reference text: the ViewSwizzle of {BASE_COMMIT} has the steps of the probe reference')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
