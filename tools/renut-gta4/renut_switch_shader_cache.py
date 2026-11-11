#!/usr/bin/env python3
"""ReNut's shader table for the Switch build, from the PC build's shader cache (docs/switch-port/18-renut-migration.md, R5).

The Switch build is a cross build: Renut's XenosRecomp (a host tool) runs on the PC, whose build writes
out/build/pc-eu-base/renut_gpu/shader_cache.cpp (the table of GTA IV's 1,370 shaders, then DXIL and SPIR-V). The console
build draws with plume's deko3d back end and links no Vulkan driver (REXGLUE_SWITCH_NO_NVK): its game shaders are the
GLSL package (tools/deko3d/deko3d_shader_package.py), and the renderer reads neither the DXIL nor the SPIR-V there
(render/shader_library.cpp: deko3d returns before either is unpacked). This copy keeps the table and leaves both arrays
empty, so the NRO carries neither (sa2-nx's tools/renut_switch_shader_cache.py keeps the SPIR-V for its NVK builds).

    python tools/renut-gta4/renut_switch_shader_cache.py [--pc out/build/pc-eu-base] [--out out/renut-switch]

Output: <out>/shader_cache_switch.cpp (THEFT4_RENUT_SHADER_CACHE_CPP of tools/switch-game). Game data: never committed.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def blank(text, array, sizes):
    m = re.search(r'(const uint8_t ' + array + r'\[\]\s*=\s*\{)(.*?)(\};)', text, re.S)
    if not m:
        sys.exit(f'renut_switch_shader_cache: no {array} in the PC shader cache')
    text = text[:m.start(2)] + '0' + text[m.end(2):]
    for s in sizes:
        text, n = re.subn(r'(const size_t ' + s + r'\s*=\s*)\d+', r'\g<1>0', text)
        if n != 1:
            sys.exit(f'renut_switch_shader_cache: {s} found {n} times')
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', default=os.path.join(ROOT, 'out', 'build', 'pc-eu-base'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut-switch'))
    a = ap.parse_args()
    src = os.path.join(a.pc, 'renut_gpu', 'shader_cache.cpp')
    if not os.path.isfile(src):
        sys.exit(f'renut_switch_shader_cache: no {src}: build the PC tree (tools/pc-eu-base, THEFT4_RENUT_ENGINE) first')
    text = open(src, encoding='utf-8', errors='strict').read()
    count = re.search(r'const size_t g_shaderCacheEntryCount\s*=\s*(\d+)', text)
    if not count:
        sys.exit('renut_switch_shader_cache: no g_shaderCacheEntryCount')
    text = blank(text, 'g_compressedDxilCache', ['g_dxilCacheCompressedSize', 'g_dxilCacheDecompressedSize'])
    text = blank(text, 'g_compressedSpirvCache', ['g_spirvCacheCompressedSize', 'g_spirvCacheDecompressedSize'])
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, 'shader_cache_switch.cpp')
    open(out + '.tmp', 'w', encoding='utf-8', newline='\n').write(
        '// tools/renut-gta4/renut_switch_shader_cache.py: the PC shader table, no DXIL, no SPIR-V (deko3d reads the GLSL\n'
        '// package). Game data: build tree only.\n' + text)
    os.replace(out + '.tmp', out)
    print(f'renut_switch_shader_cache: {count.group(1)} shaders; {os.path.getsize(src) >> 10} KB -> '
          f'{os.path.getsize(out) >> 10} KB ({out})')


if __name__ == '__main__':
    main()
