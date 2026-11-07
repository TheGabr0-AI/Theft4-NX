#!/usr/bin/env python3
"""Offline test of run 55 (devlog section AC).

    python tests/deko3d/test_run55.py

POOL: native_draw_snapshot.h's DrawDataPool (dk_run55_probe.cpp): every recycled copy as a fresh one, 0 allocations per
draw once warm (run 50's made a shared_ptr control block per copy), and no entry handed out while another thread holds it.
RULES: dk_draw_rules.h: the depth-only rule's truth table; the depth bias factors bit for bit against the Vulkan path's.
FLAGS: gta4_deko3d_runtime.py's pixel shader flags from the DKSH's shader program header (synthetic DKSH), and on the real
run 55 package (THEFT4_DK_PACKAGE55, default the private deko3d-out-s55): validated by gta4_deko3d_runtime_check, every
pixel shader's flags valid, KillsPixels only where the text has a discard, and the package equal to s47's but for the
flags words.
PROBE54: probe_compare.py on run 54's console probe (out/console-runs/run54-logs/probe): it must run (run 54's tool
stopped at dk_sel) and find the GPU's values equal to the PC's.
PROBE2: the version 2 file (every vertex's position, the indices, the viewport) through write/read, the position
comparison (same; one altered vertex found) and the mesh picture (a PNG).
INSTRUMENT: the console's instrumentation with positions on the 12 skinned shaders, compiled by uam.
Then re-runs test_run54.py (and through it runs 49-53).
Skipped parts (no private package, no uam, no run 54 logs) are reported, not passed.
"""
import glob
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
import gta4_deko3d_runtime as runtime  # noqa: E402
import gta4_deko3d_runtime_check as fmt  # noqa: E402
import probe_compare as pc  # noqa: E402

PRIVATE = os.environ.get('THEFT4_PRIVATE', r'C:\dev\Theft4\eu-private')
S55 = os.environ.get('THEFT4_DK_PACKAGE55', os.path.join(PRIVATE, 'deko3d-out-s55'))
S47 = os.path.join(PRIVATE, 'deko3d-out-s47')
SKINNED = ['5E7C32C4EF6771C0', 'AD3E412AF420299A', '9A16E1CEAD4ABA0D', 'BB0ADBE2B62216F5', '33FF561C41682B0C',
           '9023A3B988FC0EB2', 'EF7ED6F38B1F9211', 'ACB02F4220B7A583', '05E742EB16F8CE3F', 'A2D8E2F3F87A09AE',
           '6A8A2868D092BFD7', 'B773DA7363A9F395']


def build_probe(work, failures):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        failures.append('no Visual Studio clang (THEFT4_VS_ROOT): the C++ parts did not run')
        return None
    exe = os.path.join(work, 'probe55.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++20 -O2 -D_CRT_SECURE_NO_WARNINGS -I "{DK}" -I "{GTA}" -I "{os.path.join(SDK, "include")}" '
                f'"{os.path.join(HERE, "dk_run55_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('the run 55 probe did not compile')
        return None
    return exe


def run_exe(exe, args, key, failures):
    r = subprocess.run([exe] + args, capture_output=True, text=True, timeout=900)
    sys.stdout.write(r.stdout)
    m = re.search(key + r'[^\n]* errors (\d+)', r.stdout)
    if r.returncode != 0 or not m or m.group(1) != '0':
        failures.append(f'{args[0]}: {r.stdout[-400:]} {r.stderr[-400:]}')


def synthetic_dksh(ptype, kills, depth, sph_type=2):
    """A one-program DKSH as uam writes it: header, program header at 24, code at control_sz, SPH at the entry point."""
    control = 256
    entry = 0x30
    data = bytearray(control + entry + 0x50 + 16)
    struct.pack_into('<6I', data, 0, 0x48534B44, 24, control, len(data) - control, 24, 1)
    struct.pack_into('<II', data, 24, ptype, entry)
    word0 = sph_type | (3 << 5) | (5 << 10) | ((1 << 15) if kills else 0)
    struct.pack_into('<I', data, control + entry, word0)
    struct.pack_into('<I', data, control + entry + 76, 2 if depth else 0)
    return bytes(data)


def flags_test(failures):
    cases = [((1, True, False), fmt.FLAG_KILLS), ((1, False, True), fmt.FLAG_WRITES_DEPTH),
             ((1, False, False), 0), ((1, True, True), fmt.FLAG_KILLS | fmt.FLAG_WRITES_DEPTH),
             ((0, True, False), None)]
    bad = [c for c, want in cases if runtime.dksh_pixel_flags(synthetic_dksh(*c)) != want]
    bad += ['vertex SPH'] if runtime.dksh_pixel_flags(synthetic_dksh(1, True, False, sph_type=1)) is not None else []
    bad += ['short'] if runtime.dksh_pixel_flags(b'DKSH') is not None else []
    print(f'FLAGS synthetic DKSH: {len(cases) + 2 - len(bad)} of {len(cases) + 2} read right')
    if bad:
        failures.append(f'synthetic DKSH flags wrong: {bad}')
    path = os.path.join(S55, 'gta4_deko3d_runtime.bin')
    if not os.path.isfile(path):
        failures.append(f'FLAGS: no run 55 package ({path}): the real part did not run')
        return
    info = fmt.check_file(path)
    data = open(path, 'rb').read()
    taglen = struct.unpack_from('<I', data, 16)[0]
    n = struct.unpack_from('<I', data, 8)[0]
    table = (32 + taglen + 7) // 8 * 8
    texts_dir = os.path.join(S55, 'glsl_opt')
    pixel = valid = kills = depth = kills_without_discard = no_discard_but_kills = 0
    for k in range(n):
        rec = fmt.RECORD.unpack_from(data, table + 48 * k)
        h, stage, late, flags = rec[0], rec[1], rec[2], rec[12]
        if stage != 1:
            continue
        pixel += 1
        valid += bool(flags & fmt.FLAGS_VALID)
        kills += bool(flags & fmt.FLAG_KILLS)
        depth += bool(flags & fmt.FLAG_WRITES_DEPTH)
        name = os.path.join(texts_dir, f'ps_{h:016X}{"_late" if late else ""}.glsl')
        if os.path.isfile(name):
            text = re.sub(r'//[^\n]*', '', open(name, encoding='utf-8').read())
            has_discard = re.search(r'\bdiscard\b', text) is not None
            if flags & fmt.FLAG_KILLS and not has_discard:
                kills_without_discard += 1
            if flags & fmt.FLAG_WRITES_DEPTH and 'gl_FragDepth' not in text:
                no_discard_but_kills += 1
    print(f'FLAGS run 55 package: validated ({info["shaders"]} shaders, {info["blobs"]} DKSH); pixel shaders {pixel}, '
          f'flags valid {valid}, KillsPixels without the alpha test {kills}, depth written {depth}; KillsPixels with no '
          f'discard in the text {kills_without_discard}, depth written with no gl_FragDepth {no_discard_but_kills}')
    if valid != pixel or kills_without_discard or no_discard_but_kills:
        failures.append('run 55 package flags inconsistent with the texts')
    old = os.path.join(S47, 'gta4_deko3d_runtime.bin')
    if os.path.isfile(old):
        a = open(old, 'rb').read()
        diff = [i for i in range(len(data)) if a[i] != data[i]] if len(a) == len(data) else None
        outside = None if diff is None else [i for i in diff if not (table <= i < table + 48 * n and
                                                                     40 <= (i - table) % 48 < 44)]
        print(f'FLAGS against s47: {"different size" if diff is None else f"{len(diff)} bytes differ, {len(outside)} outside the flags words"}')
        if diff is None or outside:
            failures.append('run 55 package differs from s47 outside the flags words')


def probe54_test(failures):
    files = sorted(glob.glob(os.path.join(ROOT, 'out', 'console-runs', 'run54-logs', 'probe', '*.probe')))
    if not files:
        failures.append('PROBE54: no run 54 probe file: not run')
        return None
    p = pc.read_probe(open(files[0], 'rb').read())
    results = pc.compare(p)
    same = sum(r[1] == 'same' for r in results)
    print(f'PROBE54 {os.path.basename(files[0])}: {same} of {len(results)} vertices same as the PC '
          f'({p.slots} values each)')
    if same != len(results) or not results:
        failures.append(f'PROBE54: {results}')
    return p


def probe2_test(p54, work, failures):
    if p54 is None:
        failures.append('PROBE2: needs the run 54 probe as its base: not run')
        return
    p = pc.read_probe(pc.write_probe(p54))
    p.version = 2
    p.positions = [list(v[-1]) for v in p54.values]  # gl_Position is the last value of each vertex
    p.primitive = 4
    p.indices = [0, 1, 2, 0, 2, 3]
    p.viewport = (0.0, 0.0, 1280.0, 720.0, 0.0, 1.0)
    p.target = (1280, 720)
    p.depth_enable, p.depth_function, p.depth_write, p.cull_mode, p.pixel_shader = 1, 3, 1, 0, 0x1234
    q = pc.read_probe(pc.write_probe(p))
    ok = (q.version == 2 and q.positions == p.positions and q.indices == p.indices and q.target == (1280, 720)
          and q.pixel_shader == 0x1234 and abs(q.viewport[2] - 1280.0) < 1e-6)
    pcs, counts, worst = pc.compare_positions(q)
    png = os.path.join(work, 'mesh.png')
    pc.draw_mesh(q, pcs, png)
    png_ok = open(png, 'rb').read(8) == b'\x89PNG\r\n\x1a\n'
    q.positions[2] = [pc.f32_bits(pc.bits_f32(q.positions[2][0]) + 0.5)] + q.positions[2][1:]
    _, counts2, worst2 = pc.compare_positions(q)
    print(f'PROBE2 round trip {"ok" if ok else "WRONG"}; positions {counts}; triangles {len(pc.triangles(q))}; '
          f'PNG {"ok" if png_ok else "WRONG"}; one vertex moved: {counts2}, found at vertex {worst2[0] if worst2 else None}')
    if not ok or counts['same'] != len(p.positions) or not png_ok or counts2['differ'] != 1 or not worst2 or worst2[0] != 2:
        failures.append('PROBE2 failed')
    strip = pc.ProbeFile()
    strip.primitive, strip.indices = 6, [0, 1, 2, 3, 0xFFFFFFFF, 4, 5, 6]
    tris = pc.triangles(strip)
    if tris != [[0, 1, 2], [2, 1, 3], [4, 5, 6]]:
        failures.append(f'PROBE2 strip triangles {tris}')


def instrument_test(exe, work, failures):
    uam = os.environ.get('THEFT4_UAM', os.path.join(ROOT, 'out', 'deko3d_tools', 'uam-build', 'uam.exe'))
    glsl_dir = os.environ.get('THEFT4_DK_GLSL_DIR', os.path.join(S47, 'glsl'))
    files = [os.path.join(glsl_dir, f'vs_{h}.glsl') for h in SKINNED if os.path.isfile(os.path.join(glsl_dir, f'vs_{h}.glsl'))]
    if not files or not os.path.isfile(uam):
        failures.append('INSTRUMENT: no package GLSL or no uam: not run')
        return
    out_dir = os.path.join(work, 'instr')
    os.makedirs(out_dir)
    lst = os.path.join(work, 'list.txt')
    open(lst, 'w').write('\n'.join(files))
    r = subprocess.run([exe, 'instrument', out_dir, lst], capture_output=True, text=True)
    compiled, bad = 0, []
    for f in files:
        src = os.path.join(out_dir, os.path.basename(f) + '.vert')
        text = open(src, encoding='utf-8').read() if os.path.isfile(src) else ''
        if 'u + uint(gl_VertexID)] = floatBitsToUint(gl_Position);' not in text:
            bad.append((os.path.basename(f), 'no position store'))
            continue
        q = subprocess.run([uam, '-s', 'vert', '-o', src + '.dksh', src], capture_output=True, text=True)
        if q.returncode == 0 and os.path.isfile(src + '.dksh'):
            compiled += 1
        else:
            bad.append((os.path.basename(f), (q.stdout + q.stderr)[-200:]))
    print(f'INSTRUMENT with every position: {compiled} of {len(files)} skinned shaders compiled by uam')
    if bad or 'INSTRUMENTED' not in r.stdout:
        failures.append(f'INSTRUMENT: {bad[:2]}')


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = build_probe(work, failures)
        if exe:
            run_exe(exe, ['pool'], 'POOL', failures)
            run_exe(exe, ['rules'], 'RULES', failures)
        flags_test(failures)
        p54 = probe54_test(failures)
        probe2_test(p54, work, failures)
        if exe:
            instrument_test(exe, work, failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run54.py')], capture_output=True, text=True)
    tail = r.stdout.strip().split('\n')[-1] if r.stdout.strip() else ''
    print(f'test_run54.py: {"OK" if r.returncode == 0 else "FAILED"} ({tail[:200]})')
    if r.returncode != 0:
        failures.append('test_run54.py failed: ' + (r.stdout[-1500:] + r.stderr[-800:]))
    if failures:
        print('FAILED:\n  ' + '\n  '.join(failures))
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
