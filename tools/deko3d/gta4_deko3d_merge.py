#!/usr/bin/env python3
"""Run 61: a package work directory with shaders added, without recompiling the ones it has (16-deko3d-devlog.md AI).

    python tools/deko3d/gta4_deko3d_merge.py --base <package dir> --base-cache-cpp <cpp> --base-prune <prune dir>
        --add <package dir> --add-cache-cpp <cpp> --add-prune <prune dir> --out <new package dir> --out-prune <dir>

--base is a finished package work directory (run 56's deko3d-out-s56: glsl_opt and the GDK1 DKSH cache
gta4_deko3d_shaders.bin); --add is one made by the same chain for new shaders only (gta4_fxc_shaders.py coverage
--missing-out, XenosRecomp, gta4_deko3d_structure.py, gta4_deko3d_shaders.py, gta4_deko3d_zero_init.py build
--stages vs, gta4_deko3d_vs_prune.py). The output holds both: glsl_opt (a name in both must have the same text),
the DKSH cache (both tags must be equal: the same uam and patches compiled them), the cache entries (<out>/
shader_cache.cpp: the entry lines only, which is all the deko3d tools read: the specialization and texture masks) and
the pruned vertex shader folder (prune.json concatenated, the DKSH copied). Then run gta4_deko3d_runtime.py
--out-dir <out> --cache-cpp <out>/shader_cache.cpp --pruned-dksh <out-prune> and gta4_deko3d_package.py.
Everything here is game data: it stays out of the repository.
"""
import argparse
import json
import os
import re
import shutil
import struct
import sys

ENTRY = re.compile(r'^\t\{ 0x([0-9A-Fa-f]+), ')


def read_gdk1(path):
    data = open(path, 'rb').read()
    if data[:4] != b'GDK1':
        raise ValueError(f'{path}: not a GDK1 cache')
    _version, count, tag_len = struct.unpack_from('<III', data, 4)
    tag = data[16:16 + tag_len]
    blobs = {}
    for i in range(count):
        key, off, size = struct.unpack_from('<QII', data, 16 + tag_len + 16 * i)
        blobs[key] = data[off:off + size]
    return tag, blobs


def write_gdk1(path, tag, blobs):
    keys = sorted(blobs)
    header = b'GDK1' + struct.pack('<III', 1, len(keys), len(tag)) + tag
    offset = len(header) + len(keys) * 16
    table, data = [], bytearray()
    for k in keys:
        table.append(struct.pack('<QII', k, offset + len(data), len(blobs[k])))
        data += blobs[k]
        data += b'\0' * ((-len(data)) % 256)
    with open(path, 'wb') as f:
        f.write(header + b''.join(table) + data)


def entries(cpp):
    out = {}
    with open(cpp, encoding='utf-8', errors='ignore') as f:
        for line in f:
            m = ENTRY.match(line)
            if m:
                out[int(m.group(1), 16)] = line.rstrip('\n')
    return out


def main():
    ap = argparse.ArgumentParser()
    for side in ('base', 'add'):
        ap.add_argument(f'--{side}', required=True)
        ap.add_argument(f'--{side}-cache-cpp', required=True)
        ap.add_argument(f'--{side}-prune', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--out-prune', required=True)
    a = ap.parse_args()
    for d in (a.out, a.out_prune):
        if os.path.exists(d) and os.listdir(d):
            sys.exit(f'{d} exists and is not empty')

    # glsl_opt
    opt = os.path.join(a.out, 'glsl_opt')
    os.makedirs(opt, exist_ok=True)
    texts = 0
    for side in (a.base, a.add):
        src = os.path.join(side, 'glsl_opt')
        for n in sorted(os.listdir(src)):
            if not n.endswith('.glsl'):
                continue
            dst = os.path.join(opt, n)
            if os.path.exists(dst):
                if open(dst, 'rb').read() != open(os.path.join(src, n), 'rb').read():
                    sys.exit(f'{n}: different texts in --base and --add')
                continue
            shutil.copyfile(os.path.join(src, n), dst)
            texts += 1

    # DKSH cache
    tag_b, blobs = read_gdk1(os.path.join(a.base, 'gta4_deko3d_shaders.bin'))
    tag_a, more = read_gdk1(os.path.join(a.add, 'gta4_deko3d_shaders.bin'))
    if tag_a != tag_b:
        sys.exit(f'uam tags differ: {tag_b!r} / {tag_a!r}')
    base_count = len(blobs)
    for k, v in more.items():
        if k in blobs and blobs[k] != v:
            sys.exit(f'DKSH key {k:016X}: different binaries for the same text')
        blobs[k] = v
    write_gdk1(os.path.join(a.out, 'gta4_deko3d_shaders.bin'), tag_b, blobs)

    # cache entries
    e = entries(a.base_cache_cpp)
    base_entries = len(e)
    for h, line in entries(a.add_cache_cpp).items():
        e.setdefault(h, line)
    with open(os.path.join(a.out, 'shader_cache.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// gta4_deko3d_merge.py: entry lines only (specialization and texture masks); no compressed caches\n')
        f.write('#include "shader_cache.h"\nShaderCacheEntry g_shaderCacheEntries[] = {\n')
        for h in sorted(e):
            f.write(e[h] + '\n')
        f.write('};\n')

    # pruned vertex shaders
    os.makedirs(a.out_prune, exist_ok=True)
    results = []
    for side in (a.base_prune, a.add_prune):
        results += json.load(open(os.path.join(side, 'prune.json')))
        for n in os.listdir(side):
            if n.endswith('.dksh') and not os.path.exists(os.path.join(a.out_prune, n)):
                shutil.copyfile(os.path.join(side, n), os.path.join(a.out_prune, n))
    with open(os.path.join(a.out_prune, 'prune.json'), 'w') as f:
        json.dump(results, f, indent=0)

    print(f'glsl_opt {texts} texts; DKSH {base_count} + {len(blobs) - base_count} = {len(blobs)}; '
          f'cache entries {base_entries} + {len(e) - base_entries} = {len(e)}; pruned variants {len(results)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
