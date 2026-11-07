#!/usr/bin/env python3
"""Offline test of run 57 (devlog section AE).

    python tests/deko3d/test_run57.py [--full]

PLACEMENT: dk_placement.h (dk_run57_probe.cpp): the plan between EDRAM views of one placement (1X -> 4X is a 2x2
average, 2X a 1x2 one, 4X -> 1X replicates, identical views copy, another base / depth / a size outside the sample
space are refused) and the conversion pass's arithmetic on a 1280x720 image.
PASS: kPlacementPs compiled by the PC's uam (the console compiles the same text at start).
EVIDENCE: run 56's log (out/console-runs/run56-logs): every surface of the exposure chain is created by its first
resolve (its "render target" line is followed at once by its "resolved texture" line) and its anatomy image is all
zero, while the HDR scene it measures is not: what the placement model is for.
PACKAGE: test_runtime_package.py's fixtures (run 56's io mask flag; the stale run 55 fixture corrected).
--full: also tests/deko3d/test_run56.py (and through it runs 49-55; long, heavy on memory).
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
GTA = os.path.join(SDK, 'src', 'graphics', 'gta4_native')
DK = os.path.join(GTA, 'deko3d')
UAM = os.path.join(ROOT, 'out', 'deko3d_tools', 'uam-build', 'uam.exe')


def build_probe(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('PLACEMENT skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return None
    exe = os.path.join(work, 'probe57.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O2 -D_CRT_SECURE_NO_WARNINGS -I "{os.path.join(SDK, "thirdparty", "simde")}" '
                f'-I "{DK}" -I "{GTA}" -I "{os.path.join(SDK, "include")}" '
                f'"{os.path.join(HERE, "dk_run57_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('the run 57 probe did not compile')
        return None
    return exe


def pass_test(work, failures):
    if not os.path.isfile(UAM):
        failures.append('PASS skipped: no PC uam')
        return
    text = open(os.path.join(DK, 'dk_placement.h'), encoding='utf-8').read()
    m = re.search(r'kPlacementPs\[\] = R"\((.*?)\)";', text, re.S)
    if not m:
        failures.append('PASS: kPlacementPs not found')
        return
    src = os.path.join(work, 'placement.frag')
    open(src, 'w', encoding='utf-8').write(m.group(1))
    r = subprocess.run([UAM, '-s', 'frag', '-o', src + '.dksh', src], capture_output=True, text=True)
    ok = r.returncode == 0 and os.path.isfile(src + '.dksh')
    print(f'PASS kPlacementPs compiled by uam: {"yes" if ok else "NO"} {r.stdout.strip()[:200]} {r.stderr.strip()[:200]}')
    if not ok:
        failures.append('PASS: kPlacementPs does not compile')


def evidence_test(failures):
    log = os.path.join(ROOT, 'out', 'console-runs', 'run56-logs', 'game.log')
    if not os.path.isfile(log):
        failures.append('EVIDENCE skipped: no run 56 log')
        return
    lines = open(log, encoding='utf-8', errors='replace').read().split('\n')
    created_by_resolve, created_by_draw = [], []
    for i, line in enumerate(lines[:-1]):
        m = re.search(r'deko3d: render target (\w{8}): (\d+x\d+)', line)
        if not m:
            continue
        (created_by_resolve if 'resolved texture' in lines[i + 1] else created_by_draw).append(f'{m.group(1)} {m.group(2)}')
    zero = set()
    for line in lines:
        m = re.search(r'run 56 anatomy f\d+ #\d+ colour (\w{8}) -> \w{8}: \d+x\d+ host format \d+: (.*?); NaN', line)
        if m and re.fullmatch(r'(?:[RGBA] 0\.\.0 mean 0(?:, )?)+', m.group(2)):
            zero.add(m.group(1))
    chain = [s for s in created_by_resolve if s.split()[0] in zero]
    print(f'EVIDENCE run 56: {len(created_by_resolve)} colour surfaces created by their first resolve '
          f'({", ".join(created_by_resolve)}); {len(created_by_draw)} by a draw; all-zero anatomy images among the first: '
          f'{len(chain)}')
    if not any(s.startswith('402268C0') for s in chain):
        failures.append('EVIDENCE: the 640x360 view 402268C0 is not among the zero surfaces made by a resolve')


def main():
    failures = []
    work = tempfile.mkdtemp(prefix='theft4-run57-')
    exe = build_probe(work, failures)
    if exe:
        r = subprocess.run([exe, 'placement'], capture_output=True, text=True, timeout=300)
        sys.stdout.write(r.stdout)
        m = re.search(r'PLACEMENT errors (\d+)', r.stdout)
        if r.returncode != 0 or not m or m.group(1) != '0':
            failures.append('PLACEMENT: ' + r.stdout[-500:] + r.stderr[-300:])
    pass_test(work, failures)
    evidence_test(failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_runtime_package.py')], capture_output=True, text=True,
                       timeout=1800)
    print('PACKAGE fixtures (test_runtime_package.py): rc', r.returncode, '|', ' | '.join(r.stdout.strip().splitlines()[-2:]))
    if r.returncode != 0:
        failures.append('PACKAGE: ' + r.stdout[-1500:])
    if '--full' in sys.argv:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run56.py')], capture_output=True, text=True,
                           timeout=7200)
        print('test_run56.py: rc', r.returncode, '|', ' | '.join(r.stdout.strip().splitlines()[-3:]))
        if r.returncode != 0:
            failures.append('test_run56.py failed:\n' + r.stdout[-2000:])
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in skipped:
        print('SKIPPED:', f)
    if real:
        print('FAILED:')
        for f in real:
            print('  ' + f)
        return 1
    print('OK' + (' (with skipped parts)' if skipped else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
