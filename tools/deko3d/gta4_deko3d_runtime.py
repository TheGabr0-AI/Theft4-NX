#!/usr/bin/env python3
"""The deko3d renderer's shader package for the SD card (docs/switch-port/16-deko3d-devlog.md, M3 B3).

    python tools/deko3d/gta4_deko3d_runtime.py --out-dir <M1 output> --cache-cpp <shader_cache.cpp>

Reads what tools/deko3d/gta4_deko3d_shaders.py made (<out-dir>/glsl_opt and the PC's DKSH cache
<out-dir>/gta4_deko3d_shaders.bin) and writes <out-dir>/gta4_deko3d_runtime.bin, which the console's
renderer (glue/rexglue-sdk-main/src/graphics/gta4_native/deko3d/dk_shader_package.h) reads: every shader by its
hash, what the renderer needs to bind it, and the DKSH of each variant, so the console compiles nothing
the PC compiled. The GLSL package (gta4_deko3d_glsl.bin, gta4_deko3d_package.py) stays for the variants
uam could not compile on the PC (glsl_index points into it).

Format (little endian):
  "GDKR", u32 version (1), u32 shader count, u32 blob count, u32 tag length, the tag (the DKSH cache's), zero
  padding to 8 bytes;
  shader count x 48 bytes, sorted by (hash, late):
    u64 hash; u8 stage (0 vertex, 1 pixel); u8 late; u8 input count; u8 variant count;
    u32 used texture mask (shader_cache.cpp); u32 specialization mask; u32 glsl index (0xFFFFFFFF: none);
    u32 inputs offset; u32 variants offset; u32 texture types offset; u32 outputs (pixel: the colour
    output locations written, bit per location; vertex: 0); u32 reserved;
  inputs, 4 bytes each: u8 XenosRecomp location (0-39, core::ConvertVertexUsageToLocation), u8 uam location
    (0-15, the "// gta4-dk vertex-inputs" map), u8 numeric type (0 float, 1 int, 2 uint), u8 components;
  variants, 8 bytes each: u32 specialization value, u32 blob index (0xFFFFFFFF: uam failed on the PC);
  texture types, 32 bytes per shader: per fetch slot 0 none, 1 2D, 2 2D array, 3 3D, 4 cube;
  blob count x {u32 offset, u32 size}; the DKSH blobs, each at a 256-byte boundary.

Everything in it is translated from the game's shaders: game data, kept on the PC and the SD card.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gta4_deko3d_shaders as chain  # noqa: E402

ENTRY_RE = re.compile(r'^\t\{ 0x([0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), '
                      r'"[^"]*", nullptr, (\d+) \}')
INPUT_RE = re.compile(r'^layout\(location = (\d+)\) in (vec|ivec|uvec|float|int|uint)(\d?) \w+;$', re.M)
OUTPUT_RE = re.compile(r'^layout\(location = (\d+)\) out \w+ \w+;$', re.M)
MAP_RE = re.compile(r'^// gta4-dk vertex-inputs(.*)$', re.M)
TEXTURE_RE = re.compile(r'^layout\(binding = (\d+)\) uniform (sampler2D|sampler2DArray|sampler3D|samplerCube) \w+;$',
                        re.M)
TEXTURE_TYPES = {'sampler2D': 1, 'sampler2DArray': 2, 'sampler3D': 3, 'samplerCube': 4}
NUMERIC = {'vec': 0, 'float': 0, 'ivec': 1, 'int': 1, 'uvec': 2, 'uint': 2}


def cache_entries(cache_cpp):
    """{hash: (specConstantsMask, usedTextureMask)} from XenosRecomp's shader_cache.cpp."""
    out = {}
    with open(cache_cpp, encoding='utf-8', errors='ignore') as f:
        for line in f:
            m = ENTRY_RE.match(line)
            if m:
                out[int(m.group(1), 16)] = (int(m.group(10)), int(m.group(11)))
    return out


def read_dksh_cache(path):
    data = open(path, 'rb').read()
    magic, version, count, tag_len = struct.unpack_from('<4sIII', data, 0)
    if magic != b'GDK1':
        raise SystemExit(f'{path}: not a GDK1 cache')
    tag = data[16:16 + tag_len]
    table = 16 + tag_len
    blobs = {}
    for i in range(count):
        key, off, size = struct.unpack_from('<QII', data, table + i * 16)
        blobs[key] = data[off:off + size]
    return tag, blobs


def glsl_order(opt, masks):
    """The entries of gta4_deko3d_glsl.bin, in its order (gta4_deko3d_package.py)."""
    names = []
    for name in sorted(os.listdir(opt)):
        m = re.fullmatch(r'(vs|ps)_([0-9A-F]{16})(_late)?\.glsl', name)
        if m and int(m.group(2), 16) in masks:
            names.append(name)
    return {n: i for i, n in enumerate(names)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--cache-cpp', required=True)
    ap.add_argument('--extra-dksh', default='',
                    help='a folder of <stem>_<value>.dksh for variants uam could not compile as they are (E: the same '
                         'shader with its for(;;) bounded); used only where the PC cache has none')
    a = ap.parse_args()
    entries = cache_entries(a.cache_cpp)
    masks = {h: e[0] for h, e in entries.items()}
    opt = os.path.join(a.out_dir, 'glsl_opt')
    order = glsl_order(opt, masks)
    tag, pc_blobs = read_dksh_cache(os.path.join(a.out_dir, 'gta4_deko3d_shaders.bin'))

    shaders = []  # (hash, late, stage, inputs, variants, used, mask, glsl_index, types, outputs)
    blob_index = {}
    blob_list = []
    missing = 0
    for name, glsl_index in order.items():
        m = re.fullmatch(r'(vs|ps)_([0-9A-F]{16})(_late)?\.glsl', name)
        h = int(m.group(2), 16)
        stage = 0 if m.group(1) == 'vs' else 1
        late = 1 if m.group(3) else 0
        text = open(os.path.join(opt, name), encoding='utf-8').read()
        mask, used = entries[h]
        inputs = []
        if stage == 0:
            mp = MAP_RE.search(text)
            remap = {}
            if mp:
                for pair in mp.group(1).split():
                    old, new = pair.split('>')
                    remap[int(new)] = int(old)
            for im in INPUT_RE.finditer(text):
                new = int(im.group(1))
                comps = int(im.group(3)) if im.group(3) else 1
                inputs.append((remap.get(new, new), new, NUMERIC[im.group(2)], comps))
        outputs = 0
        if stage == 1:
            for om in OUTPUT_RE.finditer(text):
                outputs |= 1 << int(om.group(1))
        types = [0] * 32
        for tm in TEXTURE_RE.finditer(text):
            slot = int(tm.group(1))
            if slot < 32:
                types[slot] = TEXTURE_TYPES[tm.group(2)]
        variants = []
        for value in chain.variants(mask):
            key = chain.fnv1a64(chain.variant_text(text, value).encode('utf-8'))
            blob = pc_blobs.get(key)
            extra = os.path.join(a.extra_dksh, f'{os.path.splitext(name)[0]}_{value:x}.dksh') if a.extra_dksh else ''
            if blob is None and extra and os.path.isfile(extra):
                blob = open(extra, 'rb').read()
                key = chain.fnv1a64(blob)  # its own key: the text that made it is not the package's
            if blob is None:
                variants.append((value, 0xFFFFFFFF))
                missing += 1
                continue
            if key not in blob_index:
                blob_index[key] = len(blob_list)
                blob_list.append(blob)
            variants.append((value, blob_index[key]))
        shaders.append((h, late, stage, inputs, variants, used, mask, glsl_index, types, outputs))
    shaders.sort(key=lambda s: (s[0], s[1]))

    header = b'GDKR' + struct.pack('<IIII', 1, len(shaders), len(blob_list), len(tag)) + tag
    header += b'\0' * ((-len(header)) % 8)
    table_pos = len(header)
    extra_pos = table_pos + 48 * len(shaders)
    extra = b''
    table = b''
    for h, late, stage, inputs, variants, used, mask, glsl_index, types, outputs in shaders:
        inputs_off = extra_pos + len(extra)
        extra += b''.join(struct.pack('<BBBB', *i) for i in inputs)
        variants_off = extra_pos + len(extra)
        extra += b''.join(struct.pack('<II', *v) for v in variants)
        types_off = extra_pos + len(extra)
        extra += bytes(types)
        table += struct.pack('<QBBBBIIIIIIII', h, stage, late, len(inputs), len(variants), used, mask, glsl_index,
                             inputs_off, variants_off, types_off, outputs, 0)
    blobs_table_pos = extra_pos + len(extra)
    blobs_table_pos += (-blobs_table_pos) % 256
    data_pos = blobs_table_pos + 8 * len(blob_list)
    data_pos += (-data_pos) % 256
    blob_table = b''
    blob_data = b''
    for blob in blob_list:
        blob_table += struct.pack('<II', data_pos + len(blob_data), len(blob))
        blob_data += blob + b'\0' * ((-len(blob)) % 256)
    out = header + table + extra
    out += b'\0' * (blobs_table_pos - len(out))
    out += blob_table
    out += b'\0' * (data_pos - len(out))
    out += blob_data
    path = os.path.join(a.out_dir, 'gta4_deko3d_runtime.bin')
    open(path, 'wb').write(out)
    variants = sum(len(s[4]) for s in shaders)
    print(f'{path}: {len(shaders)} shaders, {variants} variants ({missing} without DKSH), {len(blob_list)} DKSH, '
          f'{len(out)} bytes')


if __name__ == '__main__':
    main()
