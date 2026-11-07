#!/usr/bin/env python3
"""Offline test of dk_depth_alias.h (the packed depth alias of run 38, devlog section I).

    python tests/deko3d/test_depth_alias.py

1. Compiles tests/deko3d/dk_depth_alias_probe.cpp with the host clang of Visual Studio (THEFT4_VS_ROOT, as
   test_dksh_scratch.py) and checks the decision "this fetch is the title's A8R8G8B8 view of a resolved depth".
2. Packs thousands of depths (edge values and a fixed pseudo-random set) with the C++ and with an independent Python
   version (single precision through struct) and compares: equal word for word, in both depth formats.
3. Semantics, not only equality: the 20e4 encoding is monotonic, its decode (Xenia's Float20e4To32, written here from
   the format) comes back within 2^-17 of the depth, 1.0 is 0xF00000 and 0 is 0; the bytes are R = stencil,
   G/B/A = depth bits 7..0 / 15..8 / 23..16, the layout the title's 0x60A swizzle reads (stencil in blue).
4. The pass's depth20e4 is the same text as Liberty Recompiled's packed_depth_alias_ps.glsl (the Vulkan path's).
5. Both shaders compile with the PC uam (out/deko3d_tools/uam-build/uam.exe) when it is there.
"""
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
DK_DIR = os.path.join(SDK, 'src', 'graphics', 'gta4_native', 'deko3d')
LRA_SHADER = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'src', 'graphics', 'gta4_native', 'packed_depth_alias_ps.glsl')
UAM = os.path.join(ROOT, 'out', 'deko3d_tools', 'uam-build', 'uam.exe')


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dk_depth_alias_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra '
                f'-I "{DK_DIR}" -I "{os.path.dirname(DK_DIR)}" -I "{os.path.join(SDK, "include")}" '
                f'"{os.path.join(HERE, "dk_depth_alias_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits_of(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def float_of(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def enc_20e4(depth):
    """Xenos Float32To20e4, truncating, from the format's definition (4-bit exponent, 20-bit mantissa)."""
    if not depth > 0.0:
        return 0
    b = bits_of(depth)
    if b >= 0x3FFFFFF8:
        return 0xFFFFFF
    if b < 0x38800000:
        shift = min(113 - (b >> 23), 24)
        b = (0x800000 | (b & 0x7FFFFF)) >> shift
    else:
        b = (b + 0xC8000000) & 0xFFFFFFFF
    return (b >> 3) & 0xFFFFFF


def dec_20e4(v):
    """Xenia's Float20e4To32: the value a 20e4 word stands for."""
    mantissa = v & 0xFFFFF
    exponent = (v >> 20) & 0xF
    if exponent == 0:
        if mantissa == 0:
            return 0.0
        exponent = 1
        while not mantissa & 0x100000:
            mantissa <<= 1
            exponent -= 1
        mantissa &= 0xFFFFF
    return float_of(((exponent + 112) << 23) | (mantissa << 3))


def pack(depth, stencil, float_depth):
    if float_depth:
        d = enc_20e4(depth)
    else:
        scaled = f32(min(max(depth, 0.0), 1.0) * 16777215.0)
        d = int(round(scaled))  # Python's round: to even
    return ((d << 8) | stencil) & 0xFFFFFFFF


def glsl_function(text, name):
    m = re.search(r'uint\s+' + name + r'\s*\(float depth\)\s*\{', text)
    assert m, name
    i = m.end()
    depth = 1
    while depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return re.sub(r'\s+', ' ', text[m.start():i]).strip()


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
            return 2

        # 1. the decision
        out = subprocess.run([exe, 'decide'], capture_output=True, text=True, timeout=60).stdout
        want = {'colour_fetch_of_depth': 1, 'as16_variant': 1, 'depth_fetch_of_depth': 0, 'depth24_fetch_of_depth': 0,
                'colour_fetch_of_colour': 0, 'other_colour_format': 0, 'width_differs': 0, 'height_differs': 0,
                'zero_size': 0}
        got = {m[1]: int(m[2]) for m in re.finditer(r'DECIDE (\S+) (\d)', out) if m[1] in want}
        for k, v in want.items():
            if got.get(k) != v:
                failures.append(f'decision {k}: got {got.get(k)}, expected {v}')
        if 'DECIDE_DEPTHFORMATS 1 1 0 0' not in out:
            failures.append('IsDepthFetchFormat: expected 22 and 23 only')
        print(f'decision: {len(want) - sum(1 for f in failures if f.startswith("decision"))}/{len(want)} cases as expected')

        # 1b. the registrations the front end passes on
        out = subprocess.run([exe, 'register'], capture_output=True, text=True, timeout=60).stdout
        want_reg = {'alias_of_depth': 1, 'no_source': 0, 'source_is_itself': 0, 'no_resource': 0, 'surface_kind': 0,
                    'default_command': 0}
        got_reg = {m[1]: int(m[2]) for m in re.finditer(r'REGISTER (\S+) (\d)', out)}
        for k, v in want_reg.items():
            if got_reg.get(k) != v:
                failures.append(f'registration {k}: got {got_reg.get(k)}, expected {v}')
        print(f'registrations: {len(want_reg) - sum(1 for f in failures if f.startswith("registration"))}/{len(want_reg)} cases as expected')

        # 2. packing, C++ against Python
        rng = random.Random(0x7E57)
        depths = [0.0, -0.0, -1.0, 1.0, 2.0, 0.5, 0.25, 0.999999, 1e-8, 1e-30, 6.1035156e-05,
                  float_of(0x38800000), float_of(0x387FFFFF), float_of(0x3FFFFFF7), float_of(0x3FFFFFF8)]
        depths += [f32(rng.random()) for _ in range(3000)] + [f32(rng.random() ** 8) for _ in range(1500)]
        depths += [f32(1.0 - rng.random() ** 8) for _ in range(1500)] + [float_of(rng.randrange(0, 0x7F800000)) for _ in range(500)]
        stencils = [0, 1, 0x7F, 0x80, 0xFF]
        lines, expected = [], []
        for i, d in enumerate(depths):
            for flt in (0, 1):
                s = stencils[i % len(stencils)]
                lines.append(f'{bits_of(d)} {s} {flt}')
                expected.append(pack(d, s, flt))
        res = subprocess.run([exe, 'pack'], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=120).stdout
        gots = [int(m[1]) for m in re.finditer(r'PACK \d+ \d+ \d+ (\d+)', res)]
        if len(gots) != len(expected):
            failures.append(f'pack: {len(gots)} results for {len(expected)} inputs')
        else:
            bad = [(lines[i], gots[i], expected[i]) for i in range(len(gots)) if gots[i] != expected[i]]
            print(f'pack: {len(gots)} words, C++ and Python differ in {len(bad)}')
            for line, g, e in bad[:5]:
                failures.append(f'pack {line}: C++ {g:08X}, Python {e:08X}')

    # 3. semantics
    previous = -1
    worst = 0.0
    for k in range(0, 4001):
        d = f32(k / 4000.0)
        v = enc_20e4(d)
        if v < previous:
            failures.append(f'20e4 not monotonic at {d}')
            break
        previous = v
        if d > 2 ** -14:
            worst = max(worst, abs(dec_20e4(v) - d) / d)
    print(f'20e4: monotonic over [0, 1]; worst relative error of the decode {worst:.3g}')
    if worst > 2 ** -17:
        failures.append(f'20e4 decode error {worst} > 2^-17')
    # 1.0 is exponent 15, mantissa 0 (0xF00000); 0xFFFFFF is the format's largest value, about 2.0.
    if enc_20e4(1.0) != 0xF00000 or dec_20e4(0xF00000) != 1.0 or enc_20e4(0.0) != 0 or enc_20e4(2.0) != 0xFFFFFF:
        failures.append('20e4 of 1.0 / 0.0 / 2.0')
    word = pack(0.5, 0xAB, 0)
    raw = word.to_bytes(4, 'little')
    d24 = int(round(f32(0.5 * 16777215.0)))
    if list(raw) != [0xAB, d24 & 255, (d24 >> 8) & 255, (d24 >> 16) & 255]:
        failures.append(f'byte layout {raw.hex()}')
    swz = 0x60A  # the title's swizzle for the alias: R=Z(2), G=Y(1), B=X(0), A=W(3)
    chans = [raw[(swz >> (3 * c)) & 7] for c in range(4)]
    if chans[2] != 0xAB:
        failures.append('stencil not in blue under swizzle 0x60A')
    print('byte layout: R stencil, G/B/A depth bits 7..0 / 15..8 / 23..16; swizzle 0x60A puts the stencil in blue')

    # 4. the same arithmetic as the Vulkan path's shader
    header = open(os.path.join(DK_DIR, 'dk_depth_alias.h'), encoding='utf-8').read()
    ours = re.search(r'kPackedDepthAliasPs\[\] = R"\((.*?)\)";', header, re.S).group(1)
    vs_text = re.search(r'kPackedDepthAliasVs\[\] = R"\((.*?)\)";', header, re.S).group(1)
    if os.path.isfile(LRA_SHADER):
        reference = open(LRA_SHADER, encoding='utf-8').read()
        a, b = glsl_function(ours, 'depth20e4'), glsl_function(reference, 'depth20e4')
        print('depth20e4: ' + ('same text as packed_depth_alias_ps.glsl' if a == b else 'DIFFERENT from packed_depth_alias_ps.glsl'))
        if a != b:
            failures.append('depth20e4 differs from the Vulkan path\'s shader')
        # The rest of main() is covered by the pack comparison above; the two lines the Vulkan shader has too:
        for needle in ('roundEven(clamp(depth, 0.0, 1.0) * 16777215.0)', 'alias_constants.mode != 0u'):
            print(f'  reference has "{needle}": {needle in reference}')
    else:
        print('packed_depth_alias_ps.glsl not found: shader text not compared')

    # 5. uam
    if os.path.isfile(UAM):
        with tempfile.TemporaryDirectory() as tmp:
            for stage, name, text in (('frag', 'alias_ps', ours), ('vert', 'alias_vs', vs_text)):
                src = os.path.join(tmp, name + '.glsl')
                dst = os.path.join(tmp, name + '.dksh')
                with open(src, 'w', newline='\n') as f:
                    f.write(text)
                r = subprocess.run([UAM, '-s', stage, '-o', dst, src], capture_output=True, text=True)
                ok = r.returncode == 0 and os.path.isfile(dst) and os.path.getsize(dst) > 0x30
                print(f'uam {name}: ' + (f'ok, {os.path.getsize(dst)} bytes' if ok else 'FAILED ' + (r.stdout + r.stderr)[:300]))
                if not ok:
                    failures.append(f'uam did not compile {name}')
    else:
        print('uam.exe not found: shaders not compiled')

    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
