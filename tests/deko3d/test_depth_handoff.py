#!/usr/bin/env python3
"""Offline test of run 44 (devlog section O): dk_depth_handoff.h, the scene depth hand-off.

    python tests/deko3d/test_depth_handoff.py

1. The decision (PlanDepthHandoff): only a well-formed hand-off is done; each way it can be wrong gives its own verdict, in
   the order the Vulkan path rejects them (policy, pass, surfaces, same image, unwritten source, extent, encoding); the
   Vulkan path's reject reasons exist in graphics_system.cpp for each of these.
2. The pixel the pass leaves (SceneHandoffPixel), C++ against an independent Python version written from the format (the
   20e4 float of guest format 23, the 24-bit fixed depth of 22): the destination holds depth 0 and stencil 0x80 where the
   packed scene depth is zero and the source depth and stencil 0xFF where it is not (5,000 depths, edge values included).
3. What the hand-off is FOR, on a model of the title's z-fail light volumes (not the title): a volume lights the pixels whose
   scene surface lies inside it; the z-fail count (back faces +1 and front faces -1 where the depth test fails) gives that
   set exactly against the buffer the hand-off makes, and gives no lit pixel at all against a freshly created depth
   surface (the recorder's: depth 1, stencil 0) or a cleared one (0), and the wrong set against last frame's.
4. The pixel shader: its depth20e4 is the same text as Liberty Recompiled's scene_depth_handoff_ps.glsl, both discard on a zero
   packed depth and write gl_FragDepth = depth; it compiles with the PC uam when that is there.
5. The pass state in dk_recorder.cpp is the Vulkan path's (depth_handoff_util.h and the load ops of
   RecordDepthSurfaceHandoff): clear depth 0 and stencil 0x80, depth test always with write, stencil always with REPLACE of
   0xFF, the front end passes the command on.
What no offline test shows: that the picture changes. That is run 44's measurement.
"""
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_depth_alias as alias  # noqa: E402  (the independent Python 20e4 of run 38's test)

ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
GTA = os.path.join(SDK, 'src', 'graphics', 'gta4_native')
DK_DIR = os.path.join(GTA, 'deko3d')
LRA_SHADER = os.path.join(GTA, 'scene_depth_handoff_ps.glsl')
UAM = os.path.join(ROOT, 'out', 'deko3d_tools', 'uam-build', 'uam.exe')

VERDICT = {'done': 0, 'policy': 1, 'pass': 2, 'surface': 3, 'same': 4, 'unwritten': 5, 'extent': 6, 'encoding': 7}


def compile_probe(workdir):
    vs = os.environ.get('THEFT4_VS_ROOT', r'D:\Microsoft Visual Studio')
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    clang = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang++.exe')
    if not (os.path.isfile(vcvars) and os.path.isfile(clang)):
        return None
    exe = os.path.join(workdir, 'dk_run44_probe.exe')
    bat = os.path.join(workdir, 'build_probe.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n')
        f.write(f'call "{vcvars}" >nul || exit /b 1\r\n')
        f.write(f'"{clang}" -std=c++23 -O1 -g -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra -I "{DK_DIR}" -I "{GTA}" '
                f'-I "{os.path.join(SDK, "include")}" "{os.path.join(HERE, "dk_run44_probe.cpp")}" -o "{exe}"\r\n')
    p = subprocess.run(['cmd', '/c', bat], capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise SystemExit('the probe did not compile (see above)')
    return exe


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def squash(text):
    return re.sub(r'\s+', ' ', text).strip()


def packed_scene_depth(depth, float24):
    """The packed 24 bits the title's A8R8G8B8 alias would hold for this scene depth (alias.pack with stencil 0)."""
    return alias.pack(depth, 0, float24) >> 8


def expected_pixel(depth, float24):
    if packed_scene_depth(depth, float24) == 0:
        return 0.0, 0x80
    return depth, 0xFF


def zfail_lit(buffer, zf, zb):
    """Reverse-Z title model: a fragment passes the test when its depth is greater than the buffer's; where it does not,
    the z-fail operation runs: back faces +1, front faces -1. Returns the pixels whose count is not zero."""
    lit = []
    for i, zs in enumerate(buffer):
        count = (1 if zb[i] <= zs else 0) - (1 if zf[i] <= zs else 0)
        if count != 0:
            lit.append(i)
    return lit


def main():
    failures = []
    with tempfile.TemporaryDirectory() as work:
        exe = compile_probe(work)
        if exe is None:
            print('THEFT4_VS_ROOT: no Visual Studio with clang found; nothing ran')
            return 2

        # 1. the decision
        out = subprocess.run([exe, 'plan'], capture_output=True, text=True, timeout=60).stdout
        got = {m[1]: int(m[2]) for m in re.finditer(r'PLAN (\S+) (\d+)', out)}
        want = {'good': 'done', 'good_format_22': 'done', 'preserve': 'policy', 'no_pass': 'pass',
                'no_source_surface': 'surface', 'no_destination_surface': 'surface', 'same_image': 'same',
                'source_unwritten': 'unwritten', 'width_differs': 'extent', 'height_differs': 'extent',
                'colour_format': 'encoding', 'unwritten_and_extent_and_format': 'unwritten',
                'extent_and_format': 'extent', 'same_and_unwritten': 'same'}
        bad = [k for k, v in want.items() if got.get(k) != VERDICT[v]]
        print(f'decision: {len(want) - len(bad)}/{len(want)} cases give the verdict the Vulkan path\'s order says')
        for k in bad:
            failures.append(f'decision {k}: got {got.get(k)}, expected {want[k]}')
        if 'CONSTANTS empty 128 covered 255' not in out:
            failures.append('the stencil constants are not 0x80 and 0xFF')
        vk = read(os.path.join(GTA, 'graphics_system.cpp'))
        for reason in ('surface-create', 'same-object', 'source-unwritten', 'extent', 'scene-depth-encoding'):
            if f'reject("{reason}"' not in vk:
                failures.append(f'the Vulkan path has no reject("{reason}")')

        # 2. the pixel, C++ against Python
        rng = random.Random(0x44)
        depths = [0.0, -0.0, -1.0, 1.0, 2.0, 0.5, 1e-8, 1e-30, 5.9604645e-08, 2 ** -24, 3 * 2 ** -25, 6.1035156e-05,
                  alias.float_of(0x38800000), alias.float_of(0x387FFFFF), alias.float_of(0x3FFFFFF7),
                  alias.float_of(0x3FFFFFF8), float('inf')]
        depths += [alias.f32(rng.random()) for _ in range(1500)] + [alias.f32(rng.random() ** 8) for _ in range(1500)]
        depths += [alias.f32(1.0 - rng.random() ** 8) for _ in range(1000)]
        depths += [alias.float_of(rng.randrange(0, 0x7F800000)) for _ in range(500)]
        lines, expected = [], []
        for d in depths:
            for flt in (0, 1):
                lines.append(f'{alias.bits_of(d)} {flt}')
                expected.append(expected_pixel(d, flt))
        res = subprocess.run([exe, 'pixel'], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=120).stdout
        rows = [(int(m[1]), int(m[2]), int(m[3]), int(m[4])) for m in re.finditer(r'PIXEL (\d+) (\d+) (\d+) (\d+)', res)]
        if len(rows) != len(expected):
            failures.append(f'pixel: {len(rows)} results for {len(expected)} inputs')
        else:
            wrong = []
            for (b, flt, depth_bits, stencil), (depth, st) in zip(rows, expected):
                want_bits = alias.bits_of(depth) if st == 0xFF else 0
                if stencil != st or depth_bits != want_bits:
                    wrong.append((b, flt, depth_bits, stencil, want_bits, st))
            covered = sum(1 for r in rows if r[3] == 0xFF)
            print(f'pixel: {len(rows)} cases ({covered} covered, {len(rows) - covered} empty), C++ and Python differ in {len(wrong)}')
            for w in wrong[:5]:
                failures.append(f'pixel {w}')
        # edge facts, from the formats themselves
        if expected_pixel(0.0, 1) != (0.0, 0x80) or expected_pixel(1.0, 1) != (1.0, 0xFF) or expected_pixel(1.0, 0) != (1.0, 0xFF):
            failures.append('edge depths: 0 must be empty, 1 covered')
        if expected_pixel(2 ** -26, 0) != (0.0, 0x80) or expected_pixel(3 * 2 ** -25, 0) != (3 * 2 ** -25, 0xFF):
            failures.append('fixed 24: below half a step is empty, above covered')

    # 3. what it is for: the z-fail count against four buffers
    rng = random.Random(0x2F)
    n = 4000
    scene = [alias.f32(0.05 + 0.9 * rng.random()) if rng.random() < 0.85 else 0.0 for _ in range(n)]  # 0: sky (no scene depth)
    zf, zb = [], []
    for _ in range(n):  # a volume whose near cap is in front of (above, reverse-Z) its far cap
        z0 = alias.f32(0.05 + 0.9 * rng.random())
        z1 = alias.f32(0.05 + 0.9 * rng.random())
        zf.append(max(z0, z1))
        zb.append(min(z0, z1))
    truth = [i for i in range(n) if scene[i] > 0.0 and zb[i] <= scene[i] < zf[i]]
    handed = []
    for d in scene:
        handed.append(expected_pixel(d, 1)[0])
    lit_handoff = zfail_lit(handed, zf, zb)
    lit_fresh = zfail_lit([1.0] * n, zf, zb)       # a depth surface the recorder just created: depth 1
    lit_cleared = zfail_lit([0.0] * n, zf, zb)     # cleared to 0
    stale = scene[1:] + scene[:1]                  # last frame's scene (the camera moved by a pixel)
    lit_stale = zfail_lit(stale, zf, zb)
    print(f'z-fail model: {len(truth)} pixels have their scene surface inside the volume; lit with the hand-off '
          f'{len(lit_handoff)}, against a fresh buffer {len(lit_fresh)}, a cleared one {len(lit_cleared)}, a stale one '
          f'{len(lit_stale)} ({len(set(lit_stale) & set(truth))} right)')
    if lit_handoff != truth:
        failures.append('the hand-off buffer does not give the lit set')
    if lit_fresh or lit_cleared:
        failures.append('a constant depth buffer lit pixels')
    if lit_stale == truth or not truth:
        failures.append('a stale buffer was as good as the scene (the model proves nothing)')

    # 4. the shader
    header = read(os.path.join(DK_DIR, 'dk_depth_handoff.h'))
    ours = re.search(r'kSceneDepthHandoffPs\[\] = R"\((.*?)\)";', header, re.S).group(1)
    alias_header = read(os.path.join(DK_DIR, 'dk_depth_alias.h'))
    vs_text = re.search(r'kPackedDepthAliasVs\[\] = R"\((.*?)\)";', alias_header, re.S).group(1)
    if os.path.isfile(LRA_SHADER):
        reference = read(LRA_SHADER)
        a, b = alias.glsl_function(ours, 'depth20e4'), alias.glsl_function(reference, 'depth20e4')
        print('depth20e4: ' + ('same text as scene_depth_handoff_ps.glsl' if a == b else 'DIFFERENT from scene_depth_handoff_ps.glsl'))
        if a != b:
            failures.append('depth20e4 differs from the Vulkan path\'s hand-off shader')
        ref_body = squash(reference)
        for needle in ('roundEven(clamp(depth, 0.0, 1.0) * 16777215.0)', 'discard;', 'gl_FragDepth = depth;',
                       'texelFetch(source_image, ivec2(gl_FragCoord.xy), 0).x'):
            if needle not in ref_body:
                failures.append(f'the reference shader no longer has "{needle}": re-read it')
        our_body = squash(ours)
        for needle in ('roundEven(clamp(depth, 0.0, 1.0) * 16777215.0)', 'if (packed_depth == 0u) discard;',
                       'gl_FragDepth = depth;', 'texelFetch(source_depth, ivec2(gl_FragCoord.xy), 0).r'):
            if needle not in our_body:
                failures.append(f'our shader lacks "{needle}"')
    else:
        failures.append('scene_depth_handoff_ps.glsl not found')
    if os.path.isfile(UAM):
        with tempfile.TemporaryDirectory() as tmp:
            for stage, name, text in (('frag', 'handoff_ps', ours), ('vert', 'alias_vs', vs_text)):
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

    # 5. the state, in the text
    rec = squash(read(os.path.join(DK_DIR, 'dk_recorder.cpp')))
    util = squash(read(os.path.join(GTA, 'depth_handoff_util.h')))
    for needle in ('dkCmdBufClearDepthStencil(cmd, true, 0.0f, 0xFF, kForwardEmptySceneStencil);',
                   'ds.depthCompareOp = DkCompareOp_Always;', 'ds.depthTestEnable = true;', 'ds.depthWriteEnable = true;',
                   'ds.stencilTestEnable = true;', 'ds.stencilFrontPassOp = ds.stencilBackPassOp = DkStencilOp_Replace;',
                   'ds.stencilFrontFailOp = ds.stencilBackFailOp = DkStencilOp_Keep;',
                   'ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = DkStencilOp_Keep;',
                   'ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;',
                   'dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, 0xFF, uint8_t(kForwardCoveredSceneStencil), 0xFF);'):
        if needle not in rec:
            failures.append(f'dk_recorder.cpp lacks: {needle}')
    for needle in ('state.failOp = VK_STENCIL_OP_KEEP;', 'state.passOp = VK_STENCIL_OP_REPLACE;',
                   'state.depthFailOp = VK_STENCIL_OP_KEEP;', 'state.compareOp = VK_COMPARE_OP_ALWAYS;',
                   'state.compareMask = 0xFF;', 'state.writeMask = 0xFF;', 'state.reference = kForwardCoveredSceneStencil;'):
        if needle not in util:
            failures.append(f'the Vulkan state changed: {needle}')
    gs = squash(vk)
    if 'depth_attachment.clearValue.depthStencil = {0.0f, 0};' not in gs or \
            'rebuild_scene_stencil ? kForwardEmptySceneStencil : 0u' not in gs:
        failures.append('the Vulkan load ops changed (depth 0, stencil 0x80): re-read RecordDepthSurfaceHandoff')
    fe = squash(read(os.path.join(GTA, 'native_front_end.cpp')))
    if 'out.emplace_back(std::move(h)); ++stats_.depth_handoffs;' not in fe:
        failures.append('the front end does not pass the hand-off on')
    if not failures:
        print('state: the pass state, the clear values and the front end are as the Vulkan path has them')

    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + f)
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
