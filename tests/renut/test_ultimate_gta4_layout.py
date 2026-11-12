#!/usr/bin/env python3
"""Ultimate Base on GTA IV, phase U4 (docs/switch-port/20): the device layout of the base's renderer against GTA IV.

  python tests/renut/test_ultimate_gta4_layout.py [--report out.txt]

The base's d3d/d3d.h and d3d/d3d_native.h hold Beautiful Katamari's values (D3D 2.0.5632). For every constant they
define, this compares the value with GTA IV's (2.0.6274) as the working renderer has it (thirdparty/renut/renut_engine,
the values checked since R1 by tests/renut/test_gta4_d3d.py), and for each constant with evidence below reads the
instruction again from the PowerPC listing the 0.10 generated code keeps in its comments (ultimate/app/generated).

Fails when: a constant the base defines differs from GTA IV's value; an evidence instruction is not in its function;
a constant with a GTA IV value has no evidence and is not in ACCEPTED_WITHOUT_EVIDENCE.
The generated code is local data: nothing of it is in this file but offsets and addresses.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE = os.path.join(ROOT, 'ultimate', 'app', 'src', 'renut_engine', 'gpu', 'd3d')
OURS = os.path.join(ROOT, 'thirdparty', 'renut', 'renut_engine', 'gpu', 'd3d')
GEN = os.path.join(ROOT, 'ultimate', 'app', 'generated')

# constant (as the headers name it) -> [(function, instruction)] in GTA IV's code (objdump spelling)
EVIDENCE = {
    # d3d.h
    'kDeviceAllocSize': [('829D0218', 'li r3,22400')],
    'kDeviceAlignment': [('829D0218', 'li r4,128')],
    'kDeviceOffsetCreationThreadId': [('829DF380', 'stw r11,10888(r31)')],
    'kDeviceOffsetIsTitleProcess': [('829DF380', 'stw r10,10892(r31)')],
    'kDeviceOffsetBehaviorFlags': [('829D0218', 'stw r30,22280(r31)'), ('829D8138', 'lwz r11,22280(r31)')],
    'kDeviceOffsetVertexDeclaration': [('829D1490', 'stw r4,11812(r3)')],
    'kDeviceOffsetIndexBuffer': [('829CA310', 'stw r29,12428(r31)')],
    'kDeviceOffsetRenderTargets': [('829CAAB0', 'lwz r11,12432(r31)')],
    'kDeviceOffsetDepthStencil': [('829CAAB0', 'lwz r11,12448(r31)'), ('829CAE18', 'lwz r10,12448(r31)')],
    'kDeviceOffsetStreamSources': [('829CA1F0', 'addi r11,r29,3113')],  # (3113 + n) * 4 = 12452 + 4 n
    'kDeviceOffsetStreamStrides': [('829CA1F0', 'stb r11,12520(r10)')],
    'kDeviceOffsetPixelShader': [('829D1008', 'stw r29,12684(r30)')],
    'kDeviceOffsetVertexShader': [('829D12C0', 'stw r29,12688(r30)')],
    'kDeviceOffsetPredication': [('829CD908', 'stw r9,12700(r31)')],
    'kDeviceOffsetCommandBuffer': [('829D76F0', 'lwz r3,13500(r31)'), ('829D8138', 'lwz r3,13500(r31)')],
    'kDeviceOffsetVerticesRingEnd': [('829CCAB0', 'lwz r11,13428(r3)')],
    'kDeviceOffsetSwapCount': [('829D5338', 'lwz r11,16544(r31)'), ('829D5338', 'stw r11,16544(r31)')],
    'kDeviceOffsetFenceTag': [('829D8618', 'lwz r31,10908(r3)'), ('829D8518', 'lwz r11,10908(r31)')],
    'kDeviceOffsetSwapFenceTag': [('829D8618', 'stw r31,10928(r3)')],
    'kDeviceOffsetPresentFenceTag': [('829D5338', 'stw r30,14924(r31)'), ('829D8138', 'lwz r4,14924(r31)')],
    'kDeviceOffsetSwapFlags': [('829D74C0', 'lbz r10,10941(r3)'), ('829DDF28', 'lbz r11,10941(r31)')],
    # d3d_native.h
    'FenceCompletion': [('829D8518', 'lwz r11,10896(r31)'), ('829D7D40', 'lwz r10,10896(r31)')],
    'FenceTag': [('829D8618', 'lwz r31,10908(r3)')],
    'TilePredication': [('829CD908', 'stw r22,10932(r31)')],
    'RunPredication': [('829CD908', 'stw r22,10936(r31)')],
    'Flags': [('829D0218', 'stb r11,10940(r31)'), ('829D8518', 'lbz r11,10940(r31)')],
    'Flags1': [('829D74C0', 'lbz r10,10941(r3)')],
    'Flags2': [('829DF3F0', 'stb r11,10942(r31)')],
    'IndexBuffer': [('829CA310', 'stw r29,12428(r31)')],
    'RenderTargets': [('829CAAB0', 'lwz r11,12432(r31)')],
    'DepthStencil': [('829CAAB0', 'lwz r11,12448(r31)')],
    'StreamSources': [('829CA1F0', 'addi r11,r29,3113')],
    'StreamStrides': [('829CA1F0', 'stb r11,12520(r10)')],
    'Predication': [('829CD908', 'stw r9,12700(r31)')],
    'RunPredicationState': [('829CD908', 'stw r22,12704(r31)')],
    'PredicationSet': [('829CD908', 'stw r22,12708(r31)')],
    'TilingSurfaces': [('829CAAB0', 'lwz r10,12720(r31)')],
    'TileCount': [('829CD470', 'stw r27,12740(r31)'), ('829CD908', 'lwz r11,12740(r31)')],
    'TileRects': [('829CD908', 'addi r5,r31,12744')],
    'CommandBuffer': [('829D76F0', 'lwz r3,13500(r31)')],
    'SideBuffer': [('829DF380', 'stw r3,16712(r31)'), ('829D74C0', 'lwz r11,16712(r3)')],
}
# GTA IV values placed by the shift of the fields around them, not read from GTA IV's code (the working renderer's own
# notes, d3d_native.h: "only N&B's natives, which GTA IV does not use, read them"), or not read by anything GTA IV runs.
ACCEPTED_WITHOUT_EVIDENCE = {
    'CommandBufferTagMask', 'Flags3', 'ScissorRect', 'TileOrigins', 'TilingExtent', 'AutoDepthStencil',
    'AutoFrontBuffer', 'AutoBackBuffer', 'kDeviceSnapshotSize', 'kDeviceOffsetTextures', 'Textures',
}

CONST = re.compile(r'constexpr\s+u(?:32|16|8)\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\s*;')


def constants(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    return {m.group(1): int(m.group(2), 0) for m in CONST.finditer(text)}


def listings():
    funcs, cur = {}, None
    for n in sorted(os.listdir(GEN)):
        if not (n.startswith('renut_recomp') and n.endswith('.cpp')):
            continue
        for line in open(os.path.join(GEN, n), encoding='utf-8', errors='replace'):
            m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', line)
            if m:
                cur = m.group(1).upper()
                funcs[cur] = []
            elif cur and line.lstrip().startswith('// '):
                funcs[cur].append(line.strip()[3:].strip())
    return funcs


def main():
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else None
    out, fails = [], []
    funcs = listings()
    out.append(f'functions with listings in the 0.10 generated code: {len(funcs)}')
    for hdr in ('d3d.h', 'd3d_native.h'):
        base, ours = constants(os.path.join(BASE, hdr)), constants(os.path.join(OURS, hdr))
        out.append(f'\n== {hdr}: {len(base)} constants in the base, {len(ours)} in the working GTA IV renderer')
        for name in sorted(base):
            b, g = base[name], ours.get(name)
            if g is None:
                out.append(f'  {name:<34} base {b:<8} GTA IV -        (not in the working renderer: not a device offset it uses)')
                continue
            ev = EVIDENCE.get(name, [])
            found = []
            for fn, ins in ev:
                ok = ins in funcs.get(fn, [])
                found.append(f'{fn}:{"ok" if ok else "MISSING"} "{ins}"')
                if not ok:
                    fails.append(f'{hdr} {name}: "{ins}" not in sub_{fn}')
            state = 'same' if b == g else 'DIFFERS'
            if b != g:
                fails.append(f'{hdr} {name}: base {b} (Katamari), GTA IV {g}')
            if not ev and name not in ACCEPTED_WITHOUT_EVIDENCE and b != g:
                fails.append(f'{hdr} {name}: no evidence for GTA IV {g}')
            out.append(f'  {name:<34} base {b:<8} GTA IV {g:<8} {state:<7} ' + ('; '.join(found) if found else
                       ('(accepted without evidence)' if name in ACCEPTED_WITHOUT_EVIDENCE else '(no evidence)')))
    out.append(f'\nfailures: {len(fails)}')
    out += ['  ' + f for f in fails]
    text = '\n'.join(out) + '\n'
    if report:
        open(report, 'w', encoding='utf-8', newline='\n').write(text)
    print(text if not report else f'test_ultimate_gta4_layout: failures {len(fails)} -> {report}')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
