#!/usr/bin/env python3
"""Offline test of run 59 (devlog section AG).

    python tests/deko3d/test_run59.py

PROBE (dk_run59_probe.cpp, compiled with clang -Wall -Werror): native_draw_snapshot.h's VolatilePages (pages, views,
word boundaries, the 4 GiB end), DrawDataPartHashes (the part that changed is named), dk_ab.h's phase mask (baseline /
pages off / pages only; no mask = run 58's six), and a model of the race: buffer objects new every frame in a ring of
memory, the recorder a frame behind; promoting handles (run 58) never catches the next frame's objects, marking pages
(run 59) does.
EVIDENCE: run 58's log, window by window: the share of the sampled, not copied draws that the recorder found changed since
submit in each A/B phase (with the copies and without), and the buffer objects the front end kept promoting.
CONFIG: out/console-runs/run59-build/game.toml (when it exists).
Skipped parts are reported, not passed.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
NATIVE = os.path.join(SDK, 'src', 'graphics', 'gta4_native')
DK = os.path.join(NATIVE, 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')


def probe_test(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('PROBE skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    exe = os.path.join(work, 'probe59.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O2 -Wall -Werror -I "{DK}" -I "{NATIVE}" '
                f'"{os.path.join(HERE, "dk_run59_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('PROBE: the run 59 probe did not compile')
        return
    r = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(''.join('PROBE ' + line + '\n' for line in r.stdout.splitlines()))
    if r.returncode != 0:
        failures.append('PROBE: a check failed')


def race_by_phase(log):
    """{phase: [windows, re-checked, changed, buffers promoted]} from the totals lines, window by window."""
    phase, prev, prev_fe, fe = 'before', None, None, (0, 0, 0, 0)
    out = {}
    for line in log.splitlines():
        if 'deko3d: ' not in line:
            continue
        t = line.split('deko3d: ', 1)[1]
        m = re.match(r'run 58 A/B phase "([^"]+)" begins', t)
        if m:
            phase = m.group(1)
            continue
        m = re.search(r'front end, run 49 \(totals\): draws copied at submit (\d+) \((\d+) MB.*sampled draws hashed (\d+); '
                      r'buffers copied at submit (\d+)', t)
        if m:
            cur = tuple(int(x) for x in m.groups())
            fe = tuple(a - b for a, b in zip(cur, prev_fe)) if prev_fe else (0, 0, 0, 0)
            prev_fe = cur
            continue
        m = re.search(r'run 49 \(totals\): draws from bytes copied at submit (\d+).*sampled draws checked again (\d+), '
                      r'changed since submit (\d+)', t)
        if m:
            cur = tuple(int(x) for x in m.groups())
            d = tuple(a - b for a, b in zip(cur, prev)) if prev else (0, 0, 0)
            prev = cur
            a = out.setdefault(phase, [0, 0, 0, 0])
            a[0] += 1
            a[1] += d[1]
            a[2] += d[2]
            a[3] += fe[3]
    return out


def evidence_test(failures):
    path = os.path.join(RUNS, 'run58-logs', 'game.log')
    if not os.path.isfile(path):
        failures.append('EVIDENCE skipped: no run 58 log')
        return
    phases = race_by_phase(open(path, encoding='utf-8', errors='replace').read())
    for name, (windows, checked, changed, promoted) in phases.items():
        print(f'EVIDENCE run 58 {name:16s}: {windows:3d} windows, {changed:6d} of {checked:6d} sampled draws changed since '
              f'submit ({100 * changed / max(1, checked):4.1f} %), {promoted} buffer objects promoted')
    with_copies = [v for k, v in phases.items() if k not in ('copies off', 'before')]
    on_changed = sum(v[2] for v in with_copies) / max(1, sum(v[1] for v in with_copies))
    off = phases.get('copies off')
    off_changed = off[2] / max(1, off[1]) if off else 0
    print(f'EVIDENCE with the copies: {on_changed:.1%} of the sampled draws drawn from changed bytes; without: '
          f'{off_changed:.1%}')
    if not off or off_changed < 0.4:
        failures.append('EVIDENCE: run 58 without the copies does not show the race (< 40 %)')
    if on_changed < 0.05:
        failures.append('EVIDENCE: with the copies the race is gone already (< 5 %): run 59\'s premise does not hold')
    if any(v[3] < 1000 for v in with_copies):
        failures.append('EVIDENCE: a phase promoted fewer than 1,000 buffer objects (the promotions converge)')


def config_test(failures):
    toml = os.path.join(RUNS, 'run59-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run59-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_dk_anatomy': 'false', 'nx_dk_ab': 'true', 'nx_dk_ab_phases': '193', 'nx_gpu': '"deko3d"'}
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
    print('run 59 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
