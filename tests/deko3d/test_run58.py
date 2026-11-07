#!/usr/bin/env python3
"""Offline test of run 58 (devlog section AF).

    python tests/deko3d/test_run58.py

AB: dk_ab.h (dk_run58_probe.cpp): the phases start after two gameplay windows in a row, cut-scenes and loading screens
neither move nor count, the first window of each phase settles, the capture is asked for the last one, the order is
baseline / keep targets off / placement off / store off / copies off / cull off and the totals add up over the cycles.
EVIDENCE: what run 58's choices rest on, read again from run 56's and run 57's logs:
  - the freezes: run 57's anatomy frames stalled the game for seconds each (nx_dk_anatomy is off in run 58);
  - the exposure: run 57's luminance chain (640x360 ... 1x1) is no longer all zero (run 56's was), so the placements stay;
  - the CPU: run 57's GPU was busy well under 100 % of the play windows' time and most draws were copied at submit.
CONFIG: out/console-runs/run58-build/game.toml (when it exists): anatomy off, A/B on, captures on.
Skipped parts are reported, not passed.
"""
import os
import re
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
DK = os.path.join(SDK, 'src', 'graphics', 'gta4_native', 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')


def ab_test(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('AB skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    exe = os.path.join(work, 'probe58.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O2 -Wall -Werror -I "{DK}" "{os.path.join(HERE, "dk_run58_probe.cpp")}" '
                f'-o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('AB: the run 58 probe did not compile')
        return
    r = subprocess.run([exe], capture_output=True, text=True)
    sys.stdout.write(''.join('AB ' + line + '\n' for line in r.stdout.splitlines()))
    if r.returncode != 0:
        failures.append('AB: the schedule failed a check')


def read(run):
    path = os.path.join(RUNS, f'run{run}-logs', 'game.log')
    return open(path, encoding='utf-8', errors='replace').read() if os.path.isfile(path) else None


def evidence_test(failures):
    log56, log57 = read(56), read(57)
    if not log56 or not log57:
        failures.append('EVIDENCE skipped: no run 56 / run 57 log')
        return
    # The freezes: the anatomy frames' stalls.
    stalls = [int(x) for x in re.findall(r'anatomy frame \d+ done: \d+ images, the frame stalled (\d+) ms', log57)]
    long_stalls = [s for s in stalls if s >= 1000]
    print(f'EVIDENCE run 57 anatomy stalls: {stalls} ms ({len(long_stalls)} of a second or more)')
    if not long_stalls:
        failures.append('EVIDENCE: no long anatomy stall in run 57 (the freeze explanation does not hold)')
    # The exposure: the 1x1 ... 640x360 chain in the anatomy lines (run 56 all zero, run 57 not).
    def chain(log):
        out = []
        for m in re.finditer(r'anatomy f\d+ #\d+ colour (\w+) -> \w+: (\d+)x(\d+) host format \d+: R ([-\d.e+]+)\.\.([-\d.e+]+)',
                             log):
            w, h = int(m.group(2)), int(m.group(3))
            if (w, h) in ((640, 360), (320, 180), (160, 90), (80, 45), (40, 40), (20, 20), (10, 10), (5, 5), (1, 1)):
                out.append(max(abs(float(m.group(4))), abs(float(m.group(5)))))
        return out
    c56, c57 = chain(log56), chain(log57)
    zero56 = sum(1 for v in c56 if v == 0)
    zero57 = sum(1 for v in c57 if v == 0)
    print(f'EVIDENCE luminance chain images: run 56 {len(c56)} ({zero56} all zero), run 57 {len(c57)} ({zero57} all zero)')
    if not c57 or zero57 * 2 > len(c57) or zero56 * 2 < len(c56):
        failures.append('EVIDENCE: the luminance chain is not zero in run 56 and alive in run 57')
    # The CPU: GPU busy share of the play windows and the share of draws copied at submit.
    busy = []
    for m in re.finditer(r'(\d+\.\d) frames/s, (\d+) draws per frame.*?GPU time from timestamps in the last \d+ s: \d+ works '
                         r'finished, busy (\d+) ms, idle or uploads between works (\d+) ms', log57, re.S):
        if int(m.group(2)) > 1500:
            busy.append(int(m.group(3)) / max(1, int(m.group(3)) + int(m.group(4))))
    copied = re.findall(r'front end, run 49 \(totals\): draws copied at submit (\d+)', log57)
    draws = re.findall(r'deko3d: front end: commands \d+, draws (\d+)', log57)
    share = int(copied[-1]) / max(1, int(draws[-1])) if copied and draws else 0
    gpu = statistics.median(busy) if busy else 1
    print(f'EVIDENCE run 57 play windows: GPU busy median {gpu:.0%} of the time ({len(busy)} windows); draws copied at '
          f'submit {share:.0%} of all draws')
    if gpu >= 0.9 or share < 0.5:
        failures.append('EVIDENCE: run 57 does not look CPU-bound with most draws copied at submit')


def config_test(failures):
    toml = os.path.join(RUNS, 'run58-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run58-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_dk_anatomy': 'false', 'nx_dk_ab': 'true', 'nx_gpu': '"deko3d"'}
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
        ab_test(work, failures)
    evidence_test(failures)
    config_test(failures)
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 58 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
