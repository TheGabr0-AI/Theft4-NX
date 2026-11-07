#!/usr/bin/env python3
"""Offline tests of run 41 (devlog section L).

    python tests/deko3d/test_resolve_clear.py

1. native_resolve_clear.h: which surfaces the clears that ride on a Resolve apply to, for the flags the title sends at
   the end of the deferred phase (768 = clear colour and depth on a colour resolve, 273, 274, 20, 16) and the edge cases.
   The rule is the Vulkan path's RecordResolveClears (graphics_system.cpp): bit 8 on the colour target the resolve read,
   bit 9 on the device's depth/stencil whatever was resolved, both refused with a clear-format override.
2. dk_vertex_convert.h: the vertex fix-ups of the cached-scratch path (run 40) against the in-place algorithm of run 39
   on random streams (3,000 cases per seed, three seeds), byte for byte; and the reference text in the probe is checked
   to be the text of commit 17f3a18's Draw() (taken from git), not a paraphrase.
Compiled with the host clang of Visual Studio (THEFT4_VS_ROOT), like the other tests of this folder.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GTA4 = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native')
DK = os.path.join(GTA4, 'deko3d')
SDK_INCLUDE = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'include')
HEAD_FILE = 'glue/rexglue-sdk-main/src/graphics/gta4_native/deko3d/dk_recorder.cpp'
BASE_COMMIT = '17f3a18'


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dk_run41_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{DK}" -I "{GTA4}" '
                f'-I "{SDK_INCLUDE}" "{os.path.join(HERE, "dk_run41_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def squash(text):
    return re.sub(r'\s+', ' ', text).strip()


def brace_block(text, start):
    """The text from `start` (an opening line) to its matching closing brace."""
    i = text.index('{', start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[start:j + 1]
    raise ValueError('unbalanced')


def reference_text_check():
    """The probe's REFERENCE block must contain the fix loop and the element size table of commit 17f3a18."""
    p = subprocess.run(['git', '-C', ROOT, 'show', f'{BASE_COMMIT}:{HEAD_FILE}'], capture_output=True, text=True,
                       encoding='utf-8')
    if p.returncode != 0:
        return None, f'git show {BASE_COMMIT} failed: {p.stderr.strip()[:200]}'
    base = p.stdout
    loop = brace_block(base, base.index('for (const auto& [offset, fix] : s.fixes) {'))
    sizes = brace_block(base, base.index('switch (type) {', base.index('uint32_t ElementBytes(uint32_t type) {')))
    probe = open(os.path.join(HERE, 'dk_run41_probe.cpp'), encoding='utf-8').read()
    ref = probe[probe.index('// BEGIN REFERENCE'):probe.index('// END REFERENCE')]
    problems = []
    if squash(loop) not in squash(ref):
        problems.append('the fix loop of the probe is not the loop of ' + BASE_COMMIT)
    if squash(sizes) not in squash(ref):
        problems.append('the element size table of the probe is not the one of ' + BASE_COMMIT)
    return len(loop.splitlines()), problems


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
            return 2

        out = subprocess.run([exe, 'resolve'], capture_output=True, text=True, timeout=60).stdout
        lines = re.findall(r'PLAN (\S+) (\d{4}) (\S+)', out)
        for name, bits, verdict in lines:
            print(f'  plan {name}: colour/depth/device-depth/unrepresentable = {bits} {verdict}')
            if verdict != 'ok':
                failures.append(f'resolve plan {name}')
        if len(lines) != 12:
            failures.append(f'resolve plan: {len(lines)} cases printed, expected 12')
        print(f'resolve clears: {sum(1 for l in lines if l[2] == "ok")}/12 cases as expected')

        total = 0
        for seed in (1, 12345, 0xBEEF):
            res = subprocess.run([exe, 'vertex', str(seed)], capture_output=True, text=True, timeout=120).stdout
            m = re.search(r'VERTEX cases (\d+) differences (\d+)', res)
            if not m:
                failures.append(f'vertex seed {seed}: no result line')
                continue
            total += int(m[1])
            print(f'vertex fix-ups, seed {seed}: {m[1]} streams, {m[2]} different')
            if int(m[2]):
                failures.append(f'vertex seed {seed}: {m[2]} streams differ')
        print(f'vertex fix-ups: {total} streams compared in all')

    lines_in_loop, problems = reference_text_check()
    if lines_in_loop is None:
        print('reference text: not checked (' + problems + ')')
    else:
        print(f'reference text: the loop of {BASE_COMMIT} ({lines_in_loop} lines) and its size table ' +
              ('are in the probe verbatim' if not problems else 'DIFFER'))
        failures.extend(problems)

    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
