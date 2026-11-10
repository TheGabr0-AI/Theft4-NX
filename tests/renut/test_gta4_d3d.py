#!/usr/bin/env python3
"""ReNut on GTA IV, step R1: the Direct3D layout and hook set of tools/renut-gta4 against the game's own code.

  python tests/renut/test_gta4_d3d.py

Parts:
  HEADER   gta4_d3d.h holds the values the checks below name (so a changed header fails here);
  LAYOUT   every offset is read again from the PowerPC listing the generated code keeps in comments
           (glue/rexglue-sdk-main/gta4-recomp/generated/eu-base, local: skipped when it is not there);
  HOOKS    every hooked address is a function start; gta4_renut_device.cpp defines exactly the table's hooks;
  CALLS    the generated code calls none of the hooked functions directly (tools/renut-gta4/weak_calls.py);
  XDK      with THEFT4_GAME_ROOT set: default.xex links D3D9 2.0.6274.0 (skipped otherwise);
  WIRING   the PC target's option and selection.
The generated code and the game are local data: nothing of them is in this file but offsets and addresses.
"""
import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RENUT = os.path.join(ROOT, 'tools', 'renut-gta4')
GEN = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'gta4-recomp', 'generated', 'eu-base')

# function -> instructions its listing must hold (objdump spelling, as the generated comments have it)
LAYOUT = {
    '829D0218': ['li r3,22400', 'li r4,128', 'cmpwi cr6,r29,2', 'stb r11,10940(r31)', 'stw r30,22280(r31)'],
    '829DF380': ['stw r11,60(r31)', 'std r11,10880(r31)', 'stw r11,10888(r31)', 'stw r10,10892(r31)',
                 'li r3,4800', 'stw r3,16712(r31)'],
    '829D74C0': ['lbz r10,10941(r3)', 'ori r10,r10,32', 'lwz r11,16712(r3)', 'stw r11,48(r3)', 'stw r10,52(r3)',
                 'stw r11,56(r3)'],
    '829DF600': ['stw r11,21540(r31)', 'stw r11,21544(r31)', 'stw r11,14920(r31)'],
    '829DF3F0': ['addi r3,r31,14928', 'addi r3,r31,14956', 'lwz r11,1896(r11)', 'stb r11,10942(r31)',
                 'stfs f0,21568(r31)', 'stfs f0,21572(r31)'],
    '829E1BB8': ['addi r11,r3,21680', 'li r10,41'],
    '829D8518': ['lwz r11,10896(r31)', 'lwz r11,13216(r31)', 'lbz r11,10940(r31)', 'lwz r11,10908(r31)',
                 'lwz r3,48(r31)'],
    '829D7D40': ['lwz r10,10896(r31)', 'lwz r11,10908(r31)', 'lwz r11,13216(r31)'],
    '829D8730': ['lwz r11,48(r31)', 'lwz r10,52(r31)'],
    '829D76F0': ['lwz r3,13500(r31)', 'lbz r11,10941(r31)'],
    '829D8138': ['lwz r3,13500(r31)', 'lwz r4,14924(r31)', 'lwz r11,22280(r31)'],
    '829D8618': ['lwz r31,10908(r3)', 'stw r31,10928(r3)'],
    '829D5338': ['lwz r11,16544(r31)', 'stw r11,16544(r31)', 'lwz r11,16552(r31)', 'lwz r11,14896(r31)',
                 'stw r30,14924(r31)', 'andi. r11,r11,239'],
    '829D1490': ['stw r4,11812(r3)'],
    '829CA310': ['stw r29,12428(r31)'],
    '829CA1F0': ['addi r11,r29,3113', 'stb r11,12520(r10)', 'stw r9,1780(r11)'],  # streams at (3113 + n) * 4 = 12452
    '829D1008': ['stw r29,12684(r30)'],
    '829D12C0': ['stw r29,12688(r30)'],
    '829CAAB0': ['lwz r11,12432(r31)', 'lwz r11,12444(r31)', 'lwz r11,12448(r31)', 'lwz r10,12720(r31)'],
    '829CAE18': ['lwz r10,12448(r31)'],
    # Predicated tiling: BeginTiling stores its count and rects, EndTiling's tail and the pending block count.
    '829CD470': ['stw r27,12740(r31)', 'addi r10,r31,12748'],
    '829CD908': ['lwz r11,12740(r31)', 'addi r5,r31,12744', 'addi r5,r11,-8912'],  # the callback sub_829DDD30
    '829DDF28': ['lbz r11,10941(r31)', 'stw r26,13216(r31)', 'stw r26,13220(r31)', 'addi r3,r31,13352',
                 'addi r3,r31,13232'],
    '829D7C68': ['lwz r11,11000(r30)', 'stw r11,11000(r30)'],
    '829D8798': ['lwz r10,56(r31)', 'lwz r11,11000(r31)'],
}
# gta4_d3d.h constant -> the value the checks above read
HEADER = {
    'kDeviceAllocSize': 22400, 'kDeviceAlignment': 128, 'kRing': 0x30, 'kRingLimit': 0x34, 'kRingGuarantee': 0x38,
    'kReferenceCount': 60, 'kGlobalQword': 10880, 'kCreationThreadId': 10888, 'kIsTitleProcess': 10892,
    'kFenceCompletion': 10896, 'kFenceTag': 10908, 'kInsertedFence': 10928, 'kFlags': 10940, 'kFlags1': 10941,
    'kFlags2': 10942, 'kVertexDeclaration': 11812, 'kIndexBuffer': 12428, 'kRenderTargets': 12432,
    'kDepthStencil': 12448, 'kStreamSources': 3113 * 4, 'kStreamStrides': 12520, 'kTextures': 3134 * 4,
    'kPixelShader': 12684, 'kVertexShader': 12688, 'kTilingSurfaces': 12720, 'kInKickOff': 13216,
    'kCommandBuffer': 13500, 'kSegmentBytes': 14896, 'kMinusOneA': 14920, 'kPresentFenceTag': 14924,
    'kCriticalSection0': 14928, 'kCriticalSection1': 14956, 'kSwapCount': 16544, 'kFlipCount': 16552,
    'kSideBuffer': 16712, 'kSideBufferSize': 4800, 'kMinusOneB': 21540, 'kMinusOneC': 21544,
    'kPerfFrequency': 21568, 'kTitleMinusOne': 21680, 'kTitleMinusOneWords': 41, 'kBehaviorFlags': 22280,
    'kVdGlobalDeviceImportSlot': 0x82000000 + 1896, 'kVertexFetch': 1780 - 4,
    'kPendingBlocks': 11000, 'kTileCount': 12740, 'kTileRects': 12744, 'kTileSegmentWords': 13220,
    'kSegmentLists': 13232, 'kSegmentListCount': 7, 'kSegmentListSize': 20,
}
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
    return ok


def read(path):
    return open(path, encoding='utf-8', errors='replace').read()


def part_header():
    text = read(os.path.join(RENUT, 'gta4_d3d.h'))
    values = {m.group(1): int(m.group(2), 0) for m in
              re.finditer(r'constexpr uint(?:32|8)_t (k\w+) = (0x[0-9A-Fa-f]+|\d+);', text)}
    for name, value in HEADER.items():
        check(values.get(name) == value, 'HEADER: %s is %s, the code says %d' % (name, values.get(name), value))
    table = re.findall(r'^\s*X\((\w+), ([0-9A-F]{8}),', text, re.M)
    check(len(table) == 17 and len(set(a for _, a in table)) == 17, 'HEADER: 17 distinct hooked functions')
    print('HEADER   %d constants checked, %d hooked functions' % (len(HEADER), len(table)))
    return table


def listings():
    index = {}
    for path in glob.glob(os.path.join(GEN, 'gta4_recomp.*.cpp')):
        data = read(path)
        starts = [(m.start(), m.group(1)) for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', data, re.M)]
        for i, (at, name) in enumerate(starts):
            index[name] = (data, at, starts[i + 1][0] if i + 1 < len(starts) else len(data))
    return index


def part_layout(index):
    read_n = 0
    for function, wanted in LAYOUT.items():
        if not check(function in index, 'LAYOUT: sub_%s is not a function start' % function):
            continue
        data, a, b = index[function]
        lines = set(l.strip()[3:] for l in data[a:b].split('\n') if l.strip().startswith('// '))
        for instruction in wanted:
            read_n += 1
            check(instruction in lines, 'LAYOUT: sub_%s has no "%s"' % (function, instruction))
    data = ''.join(index[f][0][index[f][1]:index[f][2]] for f in index if '829C7000' <= f < '829E4600')
    check(len(re.findall(r'// addi r\d+,r\d+,3134\n', data)) >= 1, 'LAYOUT: no (slot + 3134) * 4 texture word')
    print('LAYOUT   %d instructions read in %d functions' % (read_n + 1, len(LAYOUT)))


def part_hooks(table, index):
    source = read(os.path.join(RENUT, 'gta4_renut_device.cpp'))
    defined = set(re.findall(r'^THEFT4_RENUT_HOOK\(sub_([0-9A-F]{8})\)', source, re.M))
    check(defined == set(a for _, a in table), 'HOOKS: the hooks defined are not the table of gta4_d3d.h')
    if index is not None:
        for name, address in table:
            check(address in index, 'HOOKS: %s sub_%s is not a function start' % (name, address))
    # A hook that is not chosen must run the original: every hook names its __imp__.
    for address in defined:
        check('__imp__sub_%s' % address in source or 'THEFT4_RENUT_YIELD(sub_%s)' % address in source,
              'HOOKS: sub_%s never runs its original' % address)
    print('HOOKS    %d hooks, each a function start with its original reachable' % len(defined))


def part_calls():
    r = subprocess.run([sys.executable, os.path.join(RENUT, 'weak_calls.py'), '--check'], capture_output=True,
                       text=True)
    check(r.returncode == 0, 'CALLS: ' + (r.stdout + r.stderr).strip())
    print('CALLS    ' + r.stdout.strip())


def part_xdk():
    root = os.environ.get('THEFT4_GAME_ROOT')
    path = os.path.join(root, 'default.xex') if root else None
    if not path or not os.path.exists(path):
        print('XDK      skipped (THEFT4_GAME_ROOT not set)')
        return
    d = open(path, 'rb').read(0x10000)
    found = None
    if d[:4] == b'XEX2':
        for i in range(struct.unpack('>I', d[20:24])[0]):
            key, at = struct.unpack('>II', d[24 + i * 8:32 + i * 8])
            if key == 0x000200FF:
                size = struct.unpack('>I', d[at:at + 4])[0]
                for o in range(at + 4, at + size, 16):
                    if d[o:o + 8].rstrip(b'\0') == b'D3D9':
                        found = struct.unpack('>HHHH', d[o + 8:o + 16])
    check(found is not None and found[:3] == (2, 0, 6274), 'XDK: D3D9 is %s, not 2.0.6274' % (found,))
    print('XDK      D3D9 %d.%d.%d (QFE %d)' % (found[0], found[1], found[2], found[3] & 0xFF) if found else 'XDK      ?')


def part_wiring():
    cmake = read(os.path.join(ROOT, 'tools', 'pc-eu-base', 'CMakeLists.txt'))
    main = read(os.path.join(ROOT, 'tools', 'pc-eu-base', 'main.cpp'))
    source = read(os.path.join(RENUT, 'gta4_renut_device.cpp'))
    check('option(THEFT4_RENUT' in cmake and 'weak_calls.py' in cmake and 'gta4_renut_device.cpp' in cmake,
          'WIRING: the PC target has no THEFT4_RENUT option')
    check('theft4::renut::Selected()' in main and 'CreateNullGraphicsSystem' in main,
          'WIRING: main.cpp does not choose the renderer')
    check('REXCVAR_DEFINE_STRING(gta4_renderer, "xenos"' in source, 'WIRING: gta4_renderer must default to xenos')
    print('WIRING   THEFT4_RENUT option, gta4_renderer (default xenos), no emulated GPU when chosen')


def main():
    table = part_header()
    index = listings() if os.path.isdir(GEN) and glob.glob(os.path.join(GEN, 'gta4_recomp.*.cpp')) else None
    if index is None:
        print('LAYOUT   skipped (no generated code)')
    else:
        part_layout(index)
    part_hooks(table, index)
    if index is None:
        print('CALLS    skipped (no generated code)')
    else:
        part_calls()
    part_xdk()
    part_wiring()
    if failures:
        print('\n'.join('FAIL ' + f for f in failures))
        sys.exit(1)
    print('OK')


if __name__ == '__main__':
    main()
