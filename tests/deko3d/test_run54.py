#!/usr/bin/env python3
"""Offline test of run 54 (devlog section AB).

    python tests/deko3d/test_run54.py

INSTRUMENT: dk_shader_probe.h (the console's instrumentation of a vertex shader) on the package's vertex shaders
(THEFT4_DK_GLSL_DIR, default the private deko3d-out-s47/glsl): the skinned shaders of runs 52-53 must be instrumentable
and the instrumented GLSL must compile with uam (THEFT4_UAM, default out/deko3d_tools/uam-build/uam.exe).
INTERP: tools/deko3d/probe_compare.py's GLSL interpreter on the real character depth shader 5E7C32C4EF6771C0 against an
independent skinning (rows . (p, 1) weighted): 200 random rigid palettes and vertices.
COMPARE: synthetic probe files (write_probe / read_probe round trip): no difference when the "GPU" values are the
interpreter's, the first difference named exactly when one value is altered, "not run" for an untouched slot.
COVERAGE: how many of the package's vertex shaders the interpreter runs (texture fetches and loops excluded).
LOCK: the synchronous texture lock wait (completed by another thread; a wait nobody completes times out).
Also re-runs test_run53.py (and through it runs 49-52).
Skipped parts (no private package, no uam) are reported, not passed.
"""
import glob
import math
import os
import random
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
import probe_compare as pc  # noqa: E402

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
    exe = os.path.join(work, 'probe54.exe')
    bat = os.path.join(work, 'b.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -I "{DK}" -I "{GTA}" -I "{os.path.join(SDK, "include")}" '
                f'"{os.path.join(HERE, "dk_run54_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    if p.returncode != 0 or not os.path.isfile(exe):
        sys.stdout.write(p.stdout[-3000:] + p.stderr[-3000:])
        failures.append('the run 54 probe did not compile')
        return None
    return exe


def instrument_test(exe, work, glsl_dir, failures):
    uam = os.environ.get('THEFT4_UAM', os.path.join(ROOT, 'out', 'deko3d_tools', 'uam-build', 'uam.exe'))
    files = [os.path.join(glsl_dir, f'vs_{h}.glsl') for h in SKINNED if os.path.isfile(os.path.join(glsl_dir, f'vs_{h}.glsl'))]
    every = sorted(glob.glob(os.path.join(glsl_dir, 'vs_*.glsl')))
    out_dir = os.path.join(work, 'instr')
    os.makedirs(out_dir)
    list_file = os.path.join(work, 'glsl.txt')
    with open(list_file, 'w') as f:
        f.write(chr(10).join(every))
    r = subprocess.run([exe, 'instrument', out_dir, list_file], capture_output=True, text=True)
    m = re.search(r'INSTRUMENTED (\d+) REFUSED (\d+)', r.stdout)
    instrumented = {l.split()[1] for l in r.stdout.splitlines() if l.startswith('INSTR ') and l.split()[3] == '1'}
    missing = [os.path.basename(f) for f in files if os.path.basename(f) not in instrumented]
    print(f'INSTRUMENT {m[1] if m else "?"} of {len(every)} vertex shaders instrumented ({m[2] if m else "?"} refused); '
          f'skinned ones: {len(files) - len(missing)} of {len(files)}')
    if not m or missing:
        failures.append(f'skinned shaders not instrumented: {missing}')
    if not os.path.isfile(uam):
        failures.append(f'no uam ({uam}): the instrumented shaders were not compiled')
        return
    compiled, bad = 0, []
    for f in files:
        src = os.path.join(out_dir, os.path.basename(f) + '.vert')
        p = subprocess.run([uam, '-s', 'vert', '-o', src + '.dksh', src], capture_output=True, text=True)
        if p.returncode == 0 and os.path.isfile(src + '.dksh'):
            compiled += 1
        else:
            bad.append((os.path.basename(f), (p.stdout + p.stderr)[-300:]))
    print(f'INSTRUMENT uam compiled {compiled} of {len(files)} instrumented skinned shaders')
    if bad:
        failures.append(f'instrumented shaders uam refused: {bad[:2]}')


def make_ped_probe(glsl, rng):
    """A probe of the character depth shader with a random rigid palette and one vertex per probe slot."""
    p = pc.ProbeFile()
    p.hash, p.value, p.sequence = 0x5E7C32C4EF6771C0, 0, 7
    bank = [[0.0] * 4 for _ in range(256)]
    for b in range(64):
        # a random rotation (axis-angle) and translation
        ax = [rng.uniform(-1, 1) for _ in range(3)]
        n = math.sqrt(sum(x * x for x in ax)) or 1.0
        x, y, z = (a / n for a in ax)
        t = rng.uniform(-3, 3)
        c, s, C = math.cos(t), math.sin(t), 1 - math.cos(t)
        rows = [[c + x * x * C, x * y * C - z * s, x * z * C + y * s, rng.uniform(-2, 2)],
                [y * x * C + z * s, c + y * y * C, y * z * C - x * s, rng.uniform(-2, 2)],
                [z * x * C - y * s, z * y * C + x * s, c + z * z * C, rng.uniform(-2, 2)]]
        for k in range(3):
            bank[64 + 3 * b + k] = rows[k]
    bank[3] = [rng.uniform(-100, 100), rng.uniform(-100, 100), rng.uniform(-10, 10), 0.0]
    bank[60] = [1.0, 1.0, 1.0, 0.0]
    p.bank = [[pc.f32(v) for v in row] for row in bank]
    p.shared = [[0] * 4 for _ in range(64)]
    p.glsl = glsl
    verts = []
    data = b''
    for _ in range(4):
        pos = [rng.uniform(-0.8, 0.8) for _ in range(3)]
        w0 = rng.randrange(0, 256)
        w = [w0, 255 - w0, 0, 0]
        idx = [rng.randrange(64), rng.randrange(64), 0, 0]
        verts.append((pos, w, idx))
        host = lambda col: bytes([col[2], col[1], col[0], col[3]])  # D3DCOLOR after the word swap, read BGRA
        data += struct.pack('<3f', *pos) + host(w) + host(idx)
    p.buffers = [dict(stride=20, divisor=0, data=data)]
    A = lambda off, size, typ, bgra: dict(buffer=0, fixed=0, offset=off, size=size, type=typ, bgra=bgra)
    p.attribs = [A(0, 0x02, 7, 0), A(16, 0x0a, 2, 1), A(12, 0x0a, 2, 1)]  # POSITION, BLENDINDICES, BLENDWEIGHT
    p.vertices = len(verts)
    return p, verts


def skin_reference(bank, pos, w, idx):
    ref = [0.0, 0.0, 0.0]
    for wi, bi in zip(w, idx):
        if wi:
            for k in range(3):
                r = bank[64 + 3 * bi + k]
                ref[k] += (wi / 255) * (r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3])
    return ref


def interp_test(glsl_dir, failures):
    path = os.path.join(glsl_dir, 'vs_5E7C32C4EF6771C0.glsl')
    if not os.path.isfile(path):
        failures.append('INTERP skipped: no private GLSL of 5E7C32C4EF6771C0')
        return None
    glsl = open(path, encoding='utf-8').read()
    it = pc.Interpreter(glsl)
    rng = random.Random(54)
    worst, cases = 0.0, 0
    for _ in range(50):
        p, verts = make_ped_probe(glsl, rng)
        for v, (pos, w, idx) in enumerate(verts):
            env = it.run(p, v)
            skinned = env['_495'].a  # _452.xzy + c3.xyz
            ref = skin_reference(p.bank, pos, w, idx)
            ref = [ref[0] + p.bank[3][0], ref[1] + p.bank[3][1], ref[2] + p.bank[3][2]]
            worst = max(worst, max(abs(a - b) / max(1.0, abs(b)) for a, b in zip(skinned, ref)))
            cases += 1
    print(f'INTERP {cases} skinned vertices of the real shader against the reference: worst relative error {worst:.2e}')
    if worst > 1e-5:
        failures.append('the interpreter does not reproduce the shader\'s skinning')
    return glsl


def compare_test(glsl, failures):
    rng = random.Random(541)
    p, _ = make_ped_probe(glsl, rng)
    vars_ = []
    for m in re.finditer(r'^    (?:precise )?(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|bvec[234]) (_\w+)(?: = |;)',
                         glsl[glsl.find('void main()'):], re.M):
        vars_.append((m.group(1), m.group(2)))
    vars_.append(('vec4', 'gl_Position'))
    p.vars = vars_
    p.slots = len(vars_)
    it = pc.Interpreter(glsl)
    values = []
    for v in range(p.vertices):
        env = it.run(p, v)
        values.append([pc.bits_of(env[name]) for _, name in vars_])
    p.values = values
    p2 = pc.read_probe(pc.write_probe(p))
    clean = pc.compare(p2)
    same = all(r[1] == 'same' for r in clean)
    # one value altered: _150 (the bone registers) of vertex 1; gl_Position of vertex 2; vertex 3 never run
    k150 = next(i for i, (_, n) in enumerate(vars_) if n == '_150')
    p.values[1][k150] = list(p.values[1][k150])
    p.values[1][k150][0] = pc.f32_bits(pc.bits_f32(p.values[1][k150][0]) + 3.0)
    kpos = len(vars_) - 1
    p.values[2][kpos] = list(p.values[2][kpos])
    p.values[2][kpos][1] = pc.f32_bits(pc.bits_f32(p.values[2][kpos][1]) * 1.5 + 0.5)
    p.values[3] = [[0xFFFFFFFF] * 4 for _ in vars_]
    hurt = pc.compare(pc.read_probe(pc.write_probe(p)))
    ok = (hurt[0][1] == 'same' and hurt[1][1] == 'differs' and hurt[1][2][1] == '_150' and hurt[2][1] == 'differs'
          and hurt[2][2][1] == 'gl_Position' and hurt[3][1] == 'not run')
    print(f'COMPARE clean {"same" if same else "DIFFERS"}; altered: ' +
          ', '.join(f'v{r[0]} {r[1]}{" at " + r[2][1] if r[2] else ""}' for r in hurt))
    if not same or not ok:
        failures.append('the comparison does not find the first altered value')


def coverage_test(glsl_dir, failures):
    files = sorted(glob.glob(os.path.join(glsl_dir, 'vs_*.glsl')))
    ran, textured, other = 0, 0, []
    for f in files:
        glsl = open(f, encoding='utf-8').read()
        it = pc.Interpreter(glsl)
        if it.unsupported and 'texture' in it.unsupported:
            textured += 1
            continue
        p = pc.ProbeFile()
        p.bank = [[0.25, 0.5, 0.75, 1.0] for _ in range(256)]
        p.shared = [[0] * 4 for _ in range(64)]
        p.buffers = [dict(stride=64, divisor=0, data=bytes(64))]
        p.attribs = [dict(buffer=0, fixed=0, offset=0, size=0x01, type=7, bgra=0) for _ in range(16)]
        try:
            it.run(p, 0)
            ran += 1
        except pc.Unsupported as e:
            other.append((os.path.basename(f), str(e)[:60]))
    print(f'COVERAGE the interpreter runs {ran} of {len(files)} vertex shaders ({textured} fetch textures, '
          f'{len(other)} other: {other[:3]})')
    if files and ran + textured < 0.95 * len(files):
        failures.append('the interpreter fails on more than 5 % of the vertex shaders without textures')


def main():
    failures = []
    glsl_dir = os.environ.get('THEFT4_DK_GLSL_DIR', r'C:\dev\Theft4\eu-private\deko3d-out-s47\glsl')
    with tempfile.TemporaryDirectory() as work:
        exe = build_probe(work, failures)
        if exe:
            out = subprocess.run([exe, 'lock'], capture_output=True, text=True).stdout
            sys.stdout.write(out)
            if not re.search(r'LOCK errors 0', out):
                failures.append('the texture lock wait is wrong')
            if os.path.isdir(glsl_dir):
                instrument_test(exe, work, glsl_dir, failures)
            else:
                failures.append(f'INSTRUMENT skipped: no {glsl_dir}')
    if os.path.isdir(glsl_dir):
        glsl = interp_test(glsl_dir, failures)
        if glsl:
            compare_test(glsl, failures)
        coverage_test(glsl_dir, failures)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'test_run53.py')], capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or ['?'])[-1]
    print(f'test_run53.py: {tail}')
    if r.returncode != 0:
        failures.append('test_run53.py failed')
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
