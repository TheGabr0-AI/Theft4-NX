#!/usr/bin/env python3
"""Offline test of run 56 (devlog section AD).

    python tests/deko3d/test_run56.py

ARENA: native_draw_snapshot.h's DrawCopyArena alone (random sizes, spans given back oldest first: no live span
overlapped) and DrawDataPool::TakeCopy with a recorder thread (dk_run56_probe.cpp): every copy equal to a fresh one and
to the bytes at submit when the recorder reads it, although the "title" rewrote the buffer right after.
FUTEX: nx_multi_wait.h with the Switch's semaphore and event logic, futex mode and condition-variable mode: 80,000
lock-free releases taken by 3 consumers, 20,000 event round trips with no lost wake-up, a timed wait that times out,
multi-object waiters next to single-object ones.
ANATOMY: dk_anatomy.h's conversions (half floats, NaN marking, depth/stencil).
PACKAGE: the run 56 package (THEFT4_DK_PACKAGE56, default the private deko3d-out-s56) validated; every vertex shader
with its zero mask and 2 pruned variants; every pruned DKSH's program header: position only writes no generic output,
live outputs none outside the mask; the C++ reader (test_runtime_package.py --real) opens it; the fixtures still refuse
an io mask without its flag.
STACKS: where.py --stacks on run 55's log with its ELF names the waits (skipped without addr2line or the logs).
Then test_runtime_package.py's fixtures, test_packet_ring.py and test_run55.py (and through it runs 49-54).
Skipped parts are reported, not passed.
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
GTA = os.path.join(SDK, 'src', 'graphics', 'gta4_native')
DK = os.path.join(GTA, 'deko3d')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'deko3d'))
import gta4_deko3d_runtime_check as fmt  # noqa: E402
import gta4_deko3d_vs_prune as prune  # noqa: E402

PRIVATE = os.environ.get('THEFT4_PRIVATE', r'C:\dev\Theft4\eu-private')
S56 = os.environ.get('THEFT4_DK_PACKAGE56', os.path.join(PRIVATE, 'deko3d-out-s56'))


def build_probe(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('no Visual Studio clang (THEFT4_VS_ROOT): the C++ parts did not run')
        return None
    exe = os.path.join(work, 'probe56.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -I "{DK}" -I "{GTA}" -I "{os.path.join(SDK, "include")}" '
                f'"{os.path.join(HERE, "dk_run56_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('the run 56 probe did not compile')
        return None
    return exe


def run_part(exe, part, key, failures):
    r = subprocess.run([exe, part], capture_output=True, text=True, timeout=900)
    sys.stdout.write(r.stdout)
    errors = re.findall(key + r'[^\n]* errors (\d+)', r.stdout)
    if r.returncode != 0 or not errors or any(e != '0' for e in errors):
        failures.append(f'{part}: rc {r.returncode} {r.stdout[-400:]} {r.stderr[-400:]}')


def package_test(failures):
    path = os.path.join(S56, 'gta4_deko3d_runtime.bin')
    if not os.path.isfile(path):
        failures.append(f'PACKAGE skipped: no {path}')
        return
    info = fmt.check_file(path)
    data = open(path, 'rb').read()
    _, _, nshaders, nblobs, taglen, _, _ = fmt.HEADER.unpack_from(data, 0)
    table = fmt.align(fmt.HEADER_BYTES + taglen, 8)
    blob_table = None
    vs = ps = vs_masked = ps_masked = pruned_ok = pruned_bad = 0
    records = [fmt.RECORD.unpack_from(data, table + i * fmt.RECORD_BYTES) for i in range(nshaders)]
    extra_end = max(max(r[8] + 4 * r[3], r[9] + 8 * r[4], r[10] + 32) for r in records)
    blobs_at = fmt.align(extra_end, 256)

    def blob(i):
        off, size = struct.unpack_from('<II', data, blobs_at + 8 * i)
        return data[off:off + size]

    for r in records:
        h, stage, late, ninputs, nvariants, used, mask, glsl, inputs_at, variants_at, types_at, outputs, flags, io = r
        variants = [struct.unpack_from('<II', data, variants_at + 8 * k) for k in range(nvariants)]
        if stage == 0:
            vs += 1
            vs_masked += bool(flags & fmt.FLAG_IO_MASK)
            normal = [v for v, b in variants if not v & (fmt.VARIANT_POSITION_ONLY | fmt.VARIANT_LIVE_OUTPUTS)]
            for value in normal:
                for bit, allowed in ((fmt.VARIANT_POSITION_ONLY, 0), (fmt.VARIANT_LIVE_OUTPUTS, ((1 << 18) - 1) & ~io)):
                    b = [bb for v, bb in variants if v == value | bit]
                    if not b or b[0] == fmt.NO_BLOB:
                        pruned_bad += 1
                        continue
                    written = prune.written_outputs(blob(b[0]))
                    if written is None or written & ~allowed:
                        pruned_bad += 1
                    else:
                        pruned_ok += 1
        else:
            ps += 1
            ps_masked += bool(flags & fmt.FLAG_IO_MASK)
    print(f'PACKAGE {path}: {info["shaders"]} shaders ({vs} vertex, {ps} pixel), {info["variants"]} variants, '
          f'{info["blobs"]} DKSH; io masks: {vs_masked} vertex, {ps_masked} pixel; pruned vertex variants checked in '
          f'their program headers: {pruned_ok} good, {pruned_bad} missing or wrong')
    if vs_masked != vs or ps_masked != ps or pruned_bad or pruned_ok != 2 * vs:
        failures.append('PACKAGE: masks or pruned variants incomplete')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_runtime_package.py'), '--real', S56,
                        '--expect-shaders', str(info['shaders']), '--expect-blobs', str(info['blobs'])],
                       capture_output=True, text=True, timeout=1800)
    tail = r.stdout.strip().splitlines()[-4:]
    print('PACKAGE C++ reader and fixtures (test_runtime_package.py --real):', *tail, sep='\n    ')
    if r.returncode != 0:
        failures.append('PACKAGE: test_runtime_package.py --real failed:\n' + r.stdout[-2000:] + r.stderr[-1000:])


def stacks_test(failures):
    log = os.path.join(ROOT, 'out', 'console-runs', 'run55-logs', 'game.log')
    elf = os.path.join(ROOT, 'out', 'console-runs', 'run55-build', 'switch-game-dk-debug.elf')
    if not (os.path.isfile(log) and os.path.isfile(elf)):
        failures.append('STACKS skipped: no run 55 log or ELF')
        return
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'switch-game', 'where.py'), '--stacks', '--top', '4',
                        '--elf', elf, log], capture_output=True, text=True, timeout=1800)
    if r.returncode != 0 and 'addr2line' in (r.stdout + r.stderr):
        failures.append('STACKS skipped: no addr2line')
        return
    m = re.search(r'== id 6: .*?waiting, by the first frame.*?\n\s+([\d.]+)%\s+\d+\s+(.*)', r.stdout, re.S)
    print('STACKS run 55, main thread (id 6), first waiter:', m.group(1) + '%', m.group(2) if m else '(none)')
    if not m or 'ReleaseSemaphore' not in m.group(2):
        failures.append('STACKS: the main thread\'s semaphore wait was not named')


def main():
    failures = []
    work = tempfile.mkdtemp(prefix='theft4-run56-')
    exe = build_probe(work, failures)
    if exe:
        run_part(exe, 'arena', 'ARENA', failures)
        run_part(exe, 'futex', r'FUTEX\(\d\)', failures)
        run_part(exe, 'anatomy', 'ANATOMY', failures)
    package_test(failures)
    stacks_test(failures)
    for test in ('test_packet_ring.py', 'test_run55.py'):
        r = subprocess.run([sys.executable, os.path.join(HERE, test)], capture_output=True, text=True, timeout=7200)
        print(f'{test}: rc {r.returncode}; ' + ' | '.join(r.stdout.strip().splitlines()[-2:]))
        if r.returncode != 0:
            failures.append(f'{test} failed:\n{r.stdout[-1500:]}{r.stderr[-800:]}')
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
