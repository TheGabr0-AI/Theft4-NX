#!/usr/bin/env python3
"""The game's deko3d shaders for the SD card (docs/switch-port/16-deko3d-devlog.md, M2).

    python tools/deko3d/gta4_deko3d_package.py --out-dir <M1 output> --cache-cpp <shader_cache.cpp>

Reads <out-dir>/glsl_opt/<vs|ps>_<HASH>[_late].glsl (tools/deko3d/gta4_deko3d_shaders.py) and the
specialization masks, and writes <out-dir>/gta4_deko3d_glsl.bin, which switch-game's nx_gpu = deko3d
compiles on the console with its uam and compares with <out-dir>/gta4_deko3d_shaders.bin (the PC's DKSH):

  "GDKG", u32 version 1, u32 count, count x {u32 stage (DkStage: 0 vertex, 4 fragment), u32 text
  offset, u32 text size, u32 values offset, u32 value count}, the values (u32), the texts.

Copy both files to sdmc:/switch/theft4/deko3d/. They are translated from the game's own shaders: game
data, kept on the PC and the SD card, never in the repository or the NRO.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gta4_deko3d_shaders as chain  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--cache-cpp', required=True)
    a = ap.parse_args()
    masks = chain.spec_masks(a.cache_cpp)
    opt = os.path.join(a.out_dir, 'glsl_opt')
    entries = []
    for name in sorted(os.listdir(opt)):
        m = re.fullmatch(r'(vs|ps)_([0-9A-F]{16})(_late)?\.glsl', name)
        if not m or int(m.group(2), 16) not in masks:
            continue
        text = open(os.path.join(opt, name), encoding='utf-8').read().encode('utf-8')
        entries.append((0 if m.group(1) == 'vs' else 4, text, chain.variants(masks[int(m.group(2), 16)])))
    header = b'GDKG' + struct.pack('<II', 1, len(entries))
    table_size = len(entries) * 20
    values = b''.join(struct.pack(f'<{len(v)}I', *v) for _, _, v in entries)
    pos_values = len(header) + table_size
    pos_text = pos_values + len(values)
    table, texts, vo = b'', b'', pos_values
    for stage, text, vals in entries:
        table += struct.pack('<IIIII', stage, pos_text + len(texts), len(text), vo, len(vals))
        texts += text
        vo += 4 * len(vals)
    path = os.path.join(a.out_dir, 'gta4_deko3d_glsl.bin')
    open(path, 'wb').write(header + table + values + texts)
    print(f'{path}: {len(entries)} shaders, {sum(len(v) for _, _, v in entries)} variants, '
          f'{len(header) + table_size + len(values) + len(texts)} bytes')


if __name__ == '__main__':
    main()
