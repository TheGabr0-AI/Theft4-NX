#!/usr/bin/env python3
"""Offline test of run 60 (devlog section AH).

    python tests/deko3d/test_run60.py

PROBE (dk_run60_probe.cpp, clang -Wall -Werror): DrawDataHash gives the same hash for the same draw whatever the range's
padding bytes hold (runs 49-59 hashed them: the regression), still sees a real byte change, a change of range and of the
indices; dk_ab.h's mask 257 runs baseline / probe shaders; no mask is still run 58's six phases.
EVIDENCE (run 59's log): every draw the recorder found "changed since submit" had equal parts (indices, streams 0-2); the
logged skinned and G-buffer draws among them name no part; the copies at submit those detections caused (promoted buffer
objects, rewritten pages) were millions of draws; the race measured with the copies on was a padding artefact.
CONFIG: out/console-runs/run60-build/game.toml (when it exists).
Skipped parts are reported, not passed.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NATIVE = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native')
DK = os.path.join(NATIVE, 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')


def probe_test(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('PROBE skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    exe = os.path.join(work, 'probe60.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O2 -Wall -Werror -I "{DK}" -I "{NATIVE}" '
                f'"{os.path.join(HERE, "dk_run60_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('PROBE: the run 60 probe did not compile')
        return
    r = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(''.join('PROBE ' + line + '\n' for line in r.stdout.splitlines()))
    if r.returncode != 0:
        failures.append('PROBE: a check failed')


def evidence_test(failures):
    path = os.path.join(RUNS, 'run59-logs', 'game.log')
    if not os.path.isfile(path):
        failures.append('EVIDENCE skipped: no run 59 log')
        return
    log = open(path, encoding='utf-8', errors='replace').read()
    totals = re.findall(r'run 59 submit race \(totals\): changed since submit (\d+) of (\d+) sampled draws re-checked; by '
                        r'part: indices (\d+), stream 0 (\d+), stream 1 (\d+), stream 2 (\d+).*?draws recorded from copies '
                        r'at submit: locked (\d+), rewritten after the first fill (\d+), promoted buffer (\d+), rewritten '
                        r'pages (\d+)', log)
    if not totals:
        failures.append('EVIDENCE: no run 59 totals line')
        return
    t = [int(x) for x in totals[-1]]
    changed, checked, parts, promoted, pages = t[0], t[1], sum(t[2:6]), t[8], t[9]
    print(f'EVIDENCE run 59: {changed} of {checked} sampled draws "changed since submit", of which with a changed part '
          f'{parts}; draws copied for promoted buffers {promoted}, for rewritten pages {pages}')
    if changed < 1000 or parts != 0:
        failures.append('EVIDENCE: run 59 does not show changes without a changed part')
    if promoted + pages < 1_000_000:
        failures.append('EVIDENCE: the false detections did not cost millions of copies')
    named = re.findall(r'run 59 draw \d+ read bytes the title changed after submitting it: vertex shader [0-9A-F]+'
                       r'( \(skinned\))?( \(G-buffer\))?.*?changed: ([^(]*)\(', log)
    skinned = sum(1 for n in named if n[0])
    empty = sum(1 for n in named if not n[2].strip())
    print(f'EVIDENCE run 59 logged draws: {len(named)} ({skinned} skinned, the characters\' shaders among them), '
          f'{empty} name no changed part')
    if not named or empty != len(named) or not skinned:
        failures.append('EVIDENCE: the logged draws are not all part-less, or none is skinned')


def config_test(failures):
    toml = os.path.join(RUNS, 'run60-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run60-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    # nx_dk_skin_diff counts only in the diagnostic mode (main.cpp): without it there is no probe, nothing to swap.
    want = {'nx_dk_anatomy': 'false', 'nx_dk_ab': 'true', 'nx_dk_ab_phases': '257', 'nx_dk_skin_diff': '32',
            'nx_dk_diagnostics': 'true', 'nx_gpu': '"deko3d"'}
    for k, v in want.items():
        m = re.search(rf'^{k}\s*=\s*(\S+)', text, re.M)
        ok = bool(m) and m.group(1) == v
        print(f'CONFIG {k} = {m.group(1) if m else "(missing)"}: {"ok" if ok else "WRONG"}')
        if not ok:
            failures.append(f'CONFIG: {k} is not {v}')
    m = re.search(r'^nx_dk_capture_s\s*=\s*(\d+)', text, re.M)
    if not m or int(m.group(1)) <= 0:
        failures.append('CONFIG: captures off (the A/B phases need them)')


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        probe_test(work, failures)
    evidence_test(failures)
    config_test(failures)
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 60 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
