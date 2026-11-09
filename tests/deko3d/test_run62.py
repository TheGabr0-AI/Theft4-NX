#!/usr/bin/env python3
"""Offline test of run 62 (devlog section AJ): run 61 read, the stage counters around the characters' draws.

    python tests/deko3d/test_run62.py

STAGE (dk_run62_stage.cpp, Visual Studio clang, -Wall -Werror): dk_stage_counters.h's report layout fits the core's
  64 KiB block after the 48,000 bytes of pass stamps (32 draws a slot, 16-byte aligned, no overlap), the sampler takes
  one draw in four while there is room, the verdicts name the stage that loses triangles (assembly, clipper, after the
  pixel shader, none, counters not counting), and dk_ab.h runs mask 545 as baseline, cull off, skinned depth always.
EVIDENCE (run 61's log, out/console-runs/61-logs or run61-logs): the "skinned depth always" phase ran and changed
  draws, its captures were taken, no draw was skipped, no shader was missing, the s61 package was the one loaded
  (2,074 shaders, 7,445 DKSH, 100,311,296 bytes); the audio windows' share of real time.
PROBE (run 60's probe files, out/console-runs/run60-logs/probe): the characters' G-buffer meshes as the GPU placed
  them: no NaN, every vertex inside 0 <= z <= w, every index inside the mesh (the clipper has nothing to remove).
RECORDER: dk_recorder.cpp resets the five counters before the draw and reports them after it.
CONFIG: out/console-runs/run62-build/game.toml (when it exists).
Skipped parts are reported, not passed.
"""
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(ROOT, 'tools', 'deko3d')
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
DK = os.path.join(SDK, 'src', 'graphics', 'gta4_native', 'deko3d')
RUNS = os.path.join(ROOT, 'out', 'console-runs')
sys.path.insert(0, TOOLS)
import probe_compare as pc  # noqa: E402

# The characters' G-buffer vertex shaders of runs 55-61 (torso, head, hair, body, legs).
CHARACTERS = ['9A16E1CEAD4ABA0D', 'A2D8E2F3F87A09AE', 'EF7ED6F38B1F9211', '6A8A2868D092BFD7', '9023A3B988FC0EB2']


def stage_test(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('STAGE skipped: no Visual Studio clang (THEFT4_VS_ROOT)')
        return
    exe = os.path.join(work, 'stage.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++20 -O2 -Wall -Werror -I "{DK}" "{os.path.join(HERE, "dk_run62_stage.cpp")}" '
                f'-o "{exe}"\r\n')
    p = subprocess.run([os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'cmd.exe'), '/c', bat],
                       capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('STAGE: dk_run62_stage.cpp did not compile')
        return
    run = subprocess.run([exe], capture_output=True, text=True)
    lines = [l for l in run.stdout.splitlines() if l.startswith('STAGE ')]
    sys.stdout.write(''.join(l + '\n' for l in lines))
    if run.returncode != 0 or any(l.endswith(' FAIL') for l in lines) or len(lines) < 15:
        failures.append('STAGE: a stage-counter check failed')


def run61_log():
    for name in ('61-logs', 'run61-logs'):
        path = os.path.join(RUNS, name, 'game.log')
        if os.path.isfile(path):
            return path
    return None


def evidence_test(failures):
    path = run61_log()
    if not path:
        failures.append('EVIDENCE skipped: no run 61 log')
        return
    log = open(path, encoding='utf-8', errors='replace').read()
    always = [int(n) for n in re.findall(r'skinned G-buffer depth compare always \(run 61; (\d+) draws', log)]
    captures = re.findall(r'capture of phase "skinned depth always" \(cycle (\d+)\) at frame (\d+)', log)
    skipped = re.findall(r'drawn (\d+) \(skipped by reason: ([^)]*)\)', log)
    missing = len(re.findall(r'run 61 shader not in the package', log))
    package = re.search(r'gta4_deko3d_runtime\.bin: (\d+) shaders, (\d+) DKSH, .*?\((\d+) variants, (\d+) without DKSH, '
                        r'(\d+) bytes\)', log)
    audio = [int(n) for n in re.findall(r'audio: \d+ frames in the window \((\d+)% of real time\)', log)]
    if not (always and captures and skipped and package and audio):
        failures.append('EVIDENCE: run 61 lines missing')
        return
    print(f'EVIDENCE run 61: depth-always phase begun {len(always)} times, draws with it before the last {max(always)}; '
          f'{len(captures)} captures of the phase; drawn {skipped[-1][0]}, skipped by reason: {skipped[-1][1]}; '
          f'missing shaders {missing}; package {package[1]} shaders, {package[2]} DKSH, {package[3]} variants, '
          f'{package[4]} without DKSH, {package[5]} bytes; audio {min(audio)}-{max(audio)} % of real time over '
          f'{len(audio)} windows')
    if max(always) == 0 or len(captures) < 3:
        failures.append('EVIDENCE: the depth-always phase changed no draw or was not captured')
    if skipped[-1][1] != 'none' or missing:
        failures.append('EVIDENCE: run 61 skipped draws or missed shaders')
    if (package[1], package[2], package[4], package[5]) != ('2074', '7445', '0', '100311296'):
        failures.append('EVIDENCE: run 61 did not load the s61 package')


def probe_test(failures):
    folder = os.path.join(RUNS, 'run60-logs', 'probe')
    if not os.path.isdir(folder):
        failures.append('PROBE skipped: no run 60 probe files')
        return
    seen = 0
    for name in sorted(os.listdir(folder)):
        m = re.match(r'vs_([0-9A-F]{16})_', name)
        if not m or m[1] not in CHARACTERS:
            continue
        p = pc.read_probe(open(os.path.join(folder, name), 'rb').read())
        pos = [[struct.unpack('<f', struct.pack('<I', c))[0] for c in v] for v in p.positions]
        bad = sum(1 for x, y, z, w in pos if any(math.isnan(c) or math.isinf(c) for c in (x, y, z, w)) or w <= 0
                  or z < 0 or z > w)
        outside = sum(1 for i in p.indices if i >= len(pos))
        seen += 1
        print(f'PROBE {m[1]}: {len(pos)} vertices, {len(p.indices) // 3} triangles, outside the clip volume in z or '
              f'NaN {bad}, indices outside the mesh {outside}')
        if bad or outside or not pos:
            failures.append(f'PROBE: {m[1]} has vertices the clipper would remove')
    if seen < 4:
        failures.append('PROBE: fewer than four character meshes found')


def recorder_test(failures):
    text = open(os.path.join(DK, 'dk_recorder.cpp'), encoding='utf-8').read()
    reset = re.search(r'if \(stage_sampled\) \{\s*for \(DkCounter c : kStageDk\) \{\s*dkCmdBufResetCounter\(cmd, c\);',
                      text)
    order = text.find('dkCmdBufResetCounter(cmd, c)') < text.find('++draws_drawn_;') < \
        text.find('dkCmdBufReportCounter(cmd, kStageDk[c]')
    harvest = 'HarvestStageCounters(uint32_t(work_ % kSlots));' in text
    print(f'RECORDER counters reset before the draw {bool(reset)}, reported after it {order}, harvested at the slot\'s '
          f'reopening {harvest}')
    if not (reset and order and harvest):
        failures.append('RECORDER: the stage counters do not bracket the draw')


def config_test(failures):
    toml = os.path.join(RUNS, 'run62-build', 'game.toml')
    if not os.path.isfile(toml):
        failures.append('CONFIG skipped: no run62-build/game.toml yet')
        return
    text = open(toml, encoding='utf-8').read()
    want = {'nx_gpu': '"deko3d"', 'nx_dk_anatomy': 'false', 'nx_dk_ab': 'true', 'nx_dk_ab_phases': '545'}
    for k, v in want.items():
        m = re.search(rf'^{k}\s*=\s*(\S+)', text, re.M)
        ok = bool(m) and m.group(1) == v
        print(f'CONFIG {k} = {m.group(1) if m else "(missing)"}: {"ok" if ok else "WRONG"}')
        if not ok:
            failures.append(f'CONFIG: {k} is not {v}')


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        stage_test(work, failures)
    evidence_test(failures)
    probe_test(failures)
    recorder_test(failures)
    config_test(failures)
    skipped = [f for f in failures if 'skipped' in f]
    real = [f for f in failures if 'skipped' not in f]
    for f in failures:
        print(('SKIP ' if 'skipped' in f else 'FAIL ') + f)
    print('run 62 offline test:', 'FAIL' if real else ('OK (with skips)' if skipped else 'OK'))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
