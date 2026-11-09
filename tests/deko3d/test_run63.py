#!/usr/bin/env python3
"""Offline test of run 63 (devlog section AK): run 62 read, what writes over the characters after them.

    python tests/deko3d/test_run63.py

OVERWRITE (dk_run63_overwrite.cpp, Visual Studio clang, -Wall -Werror): dk_overwrite.h counts an operation only on a
  surface a character wrote in the same frame and before its resolve, lists the ones that are not depth-tested draws
  once, forgets the surfaces at a new frame, and names character draws on a depth surface the scenery never uses.
EVIDENCE (run 62's log): the stage counters of every phase: the characters' triangles all assembled (1.000), the
  clipper's share within 0.05 of the scenery's, at least 0.98 of their fragments written, and the verdict
  "the triangles reach the targets" in baseline, cull off and skinned depth always.
RECORDER: dk_recorder.cpp notes the tracker at the draw, the clear, the resolve, the placement and the hand-off.
CONFIG: out/console-runs/run63-build/game.toml (when it exists).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native', 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')


def overwrite_test(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('OVERWRITE skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    exe = os.path.join(work, 'overwrite.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++20 -O2 -Wall -Werror -I "{DK}" "{os.path.join(HERE, "dk_run63_overwrite.cpp")}" '
                f'-o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('OVERWRITE: dk_run63_overwrite.cpp did not compile')
        return
    run = subprocess.run([exe], capture_output=True, text=True)
    lines = [l for l in run.stdout.splitlines() if l.startswith('OVERWRITE ')]
    sys.stdout.write(''.join(l + '\n' for l in lines))
    if run.returncode != 0 or any(l.endswith(' FAIL') for l in lines) or len(lines) < 17:
        failures.append('OVERWRITE: a tracker check failed')


LINE = re.compile(r'run 62 stage counters, phase "([^"]+)" \(all cycles\), (skinned|static) G-buffer[^:]*: (\d+) draws '
                  r'sampled \((\d+) short at assembly\), triangles expected (\d+); input primitives \d+ \(([\d.]+) of '
                  r'expected\), clipper input \d+ \(([\d.]+) of input\), clipper output \d+ \(([\d.]+) of clipper input\), '
                  r'fragment invocations \d+ \(([\d.]+) a triangle out\), samples passed \d+ \(([\d.]+) a fragment\)')


def evidence_test(failures):
    path = os.path.join(RUNS, 'run62-logs', 'game.log')
    if not os.path.isfile(path):
        failures.append('EVIDENCE skipped: no run 62 log')
        return
    log = open(path, encoding='utf-8', errors='replace').read()
    last = {}
    for m in LINE.finditer(log):
        last[(m[1], m[2])] = m
    verdicts = dict(re.findall(r'run 62 stage counters, phase "([^"]+)" \(all cycles\): verdict: ([^\n]+)', log))
    for phase in ('baseline', 'cull off', 'skinned depth always'):
        c, s = last.get((phase, 'skinned')), last.get((phase, 'static'))
        if not (c and s):
            failures.append(f'EVIDENCE: run 62 phase "{phase}" lines missing')
            continue
        print(f'EVIDENCE {phase}: characters {c[3]} draws, assembled {c[6]}, clipper out {c[8]}, samples a fragment '
              f'{c[10]}; scenery {s[3]} draws, clipper out {s[8]}, samples a fragment {s[10]}; verdict: '
              f'{verdicts.get(phase, "?")}')
        if c[4] != '0' or float(c[6]) != 1.0 or abs(float(c[8]) - float(s[8])) > 0.05 or float(c[10]) < 0.98:
            failures.append(f'EVIDENCE: run 62 phase "{phase}" shows a stage losing the characters')
        if 'reach the targets' not in verdicts.get(phase, ''):
            failures.append(f'EVIDENCE: run 62 phase "{phase}" verdict differs')


def recorder_test(failures):
    text = open(os.path.join(DK, 'dk_recorder.cpp'), encoding='utf-8').read()
    hooks = {
        'draw': 'overwrite_.SkinnedDraw(colours.data(), colours.size(), depth_handle);',
        'clear': 'overwrite_.Note(OverwriteKind::kClear, c.depth_stencil.handle);',
        'resolve': 'overwrite_.Resolved(c.source.handle);',
        'placement': 'overwrite_.Note(OverwriteKind::kPlacement, d.handle);',
        'hand-off': 'overwrite_.Note(OverwriteKind::kDepthHandoff, h.destination.handle);',
        'frame': 'overwrite_.NewFrame(frames_.load());',
    }
    missing = [k for k, v in hooks.items() if v not in text]
    print(f'RECORDER tracker hooks: {len(hooks) - len(missing)} of {len(hooks)}' + (f' (missing {missing})' if missing else ''))
    if missing:
        failures.append('RECORDER: a tracker hook is missing')


def config_test(failures):
    toml = os.path.join(RUNS, 'run63-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run63-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_gpu': '"deko3d"', 'nx_dk_anatomy': 'false', 'nx_dk_ab': 'false'}
    for k, v in want.items():
        m = re.search(rf'^{k}\s*=\s*(\S+)', text, re.M)
        ok = bool(m) and m.group(1) == v
        print(f'CONFIG {k} = {m.group(1) if m else "(missing)"}: {"ok" if ok else "WRONG"}')
        if not ok:
            failures.append(f'CONFIG: {k} is not {v}')


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        overwrite_test(work, failures)
    evidence_test(failures)
    recorder_test(failures)
    config_test(failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run62.py')], capture_output=True, text=True)
    print('test_run62.py:', (r.stdout.strip().splitlines() or ['?'])[-1])
    if r.returncode != 0:
        failures.append('test_run62.py failed')
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 63 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
