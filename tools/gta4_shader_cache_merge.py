#!/usr/bin/env python3
"""Adds the shaders of a second XenosRecomp shader_cache.cpp to a first one (docs/switch-port/16-deko3d-devlog.md, M0).

    D:/Python/python.exe tools/gta4_shader_cache_merge.py --base <v8 shader_cache.cpp>
        --extra <EU shader_cache.cpp> --out <merged shader_cache.cpp>

gta4_native finds each shader by the XXH3 of its guest container. The cache built from v8 (USA, title
update 8) has 1,250 of the 1,274 shaders the European base creates; XenosRecomp run over the European
containers (tools/pc-eu-base, pc_shader_dump_dir) gives the rest. Entries of the base are kept as they
are (with its AIR and DXIL arrays, if any); every entry of --extra whose hash the base lacks is added
with its SPIR-V (and late SPIR-V) appended to the base's SPIR-V blob, no AIR and no DXIL. Both files and
the output are game data: never in the repository. Needs the zstandard module (pip install zstandard).
"""
import argparse
import re

import zstandard

ENTRY = re.compile(r'^\t\{ 0x([0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), '
                   r'"([^"]*)"(?:, nullptr, (\d+))? \},$')


def parse(path):
    text = open(path, encoding='utf-8').read()
    entries = []
    for line in text.split('\n'):
        m = ENTRY.match(line)
        if m:
            g = m.groups()
            entries.append({'hash': int(g[0], 16), 'dxil': (int(g[1]), int(g[2])), 'spirv': (int(g[3]), int(g[4])),
                            'late': (int(g[5]), int(g[6])), 'air': (int(g[7]), int(g[8])), 'spec': int(g[9]),
                            'file': g[10], 'tex': None if g[11] is None else int(g[11])})
    arrays = {}
    for m in re.finditer(r'const uint8_t (g_compressed\w+)\[\] = \{([0-9,]*)\};', text):
        arrays[m.group(1)] = bytes(int(v) for v in m.group(2).split(',') if v)
    sizes = {m.group(1): int(m.group(2)) for m in re.finditer(r'const size_t (\w+) = (\d+);', text)}
    return entries, arrays, sizes, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--extra', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    base, base_arrays, base_sizes, base_text = parse(a.base)
    extra, extra_arrays, extra_sizes, _ = parse(a.extra)
    dctx = zstandard.ZstdDecompressor()
    spirv = bytearray(dctx.decompress(base_arrays['g_compressedSpirvCache'],
                                      max_output_size=base_sizes['g_spirvCacheDecompressedSize']))
    extra_spirv = dctx.decompress(extra_arrays['g_compressedSpirvCache'],
                                  max_output_size=extra_sizes['g_spirvCacheDecompressedSize'])
    have = {e['hash'] for e in base}
    added = []
    for e in extra:
        if e['hash'] in have:
            continue
        off, size = e['spirv']
        new_off = len(spirv)
        spirv += extra_spirv[off:off + size]
        late_off, late_size = e['late']
        new_late = 0
        if late_size:
            new_late = len(spirv)
            spirv += extra_spirv[late_off:late_off + late_size]
        name = 'shader/eu-base/' + e['file'].replace(chr(92), '/').split('/')[-1]  # no local paths
        added.append(dict(e, dxil=(0, 0), spirv=(new_off, size), late=(new_late, late_size), air=(0, 0), file=name))
    entries = base + added
    entries.sort(key=lambda e: e['hash'])

    lines = ['#include "shader_cache.h"', 'ShaderCacheEntry g_shaderCacheEntries[] = {']
    for e in entries:
        tex = '' if e['tex'] is None else f', nullptr, {e["tex"]}'
        lines.append(f'\t{{ 0x{e["hash"]:X}, {e["dxil"][0]}, {e["dxil"][1]}, {e["spirv"][0]}, {e["spirv"][1]}, '
                     f'{e["late"][0]}, {e["late"][1]}, {e["air"][0]}, {e["air"][1]}, {e["spec"]}, "{e["file"]}"{tex} }},')
    lines.append('};')
    out = '\n'.join(lines) + '\n'
    # The base's other arrays (AIR, DXIL) unchanged, the SPIR-V recompressed.
    for name in ('g_compressedDxilCache', 'g_compressedAirCache'):
        if name in base_arrays:
            stem = name.replace('g_compressed', '').replace('Cache', '').lower()
            out += f'const uint8_t {name}[] = {{' + ','.join(map(str, base_arrays[name])) + ',};\n'
            out += f'const size_t g_{stem}CacheCompressedSize = {base_sizes[f"g_{stem}CacheCompressedSize"]};\n'
            out += f'const size_t g_{stem}CacheDecompressedSize = {base_sizes[f"g_{stem}CacheDecompressedSize"]};\n'
    compressed = zstandard.ZstdCompressor(level=19).compress(bytes(spirv))
    out += 'const uint8_t g_compressedSpirvCache[] = {' + ','.join(map(str, compressed)) + ',};\n'
    out += f'const size_t g_spirvCacheCompressedSize = {len(compressed)};\n'
    out += f'const size_t g_spirvCacheDecompressedSize = {len(spirv)};\n'
    out += f'const size_t g_shaderCacheEntryCount = {len(entries)};\n'
    open(a.out, 'w', encoding='utf-8', newline='\n').write(out)
    print(f'base {len(base)} entries, extra {len(extra)} ({len(added)} new): {len(entries)} entries, '
          f'SPIR-V {len(spirv)} bytes')
    for e in added:
        print(f'  added {e["hash"]:016X} {e["file"]} spec {e["spec"]:#x} late {e["late"][1] > 0}')


if __name__ == '__main__':
    main()
