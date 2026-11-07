#!/usr/bin/env python3
"""The deko3d renderer's shader package for the SD card (docs/switch-port/16-deko3d-devlog.md, M3 B3).

    python tools/deko3d/gta4_deko3d_runtime.py --out-dir <M1 output> --cache-cpp <shader_cache.cpp>

Reads what tools/deko3d/gta4_deko3d_shaders.py made (<out-dir>/glsl_opt and the PC's DKSH cache
<out-dir>/gta4_deko3d_shaders.bin) and writes <out-dir>/gta4_deko3d_runtime.bin, which the console's
renderer (glue/rexglue-sdk-main/src/graphics/gta4_native/deko3d/dk_shader_package.h) reads: every shader by its
hash, what the renderer needs to bind it, and the DKSH of each variant, so the console compiles nothing
the PC compiled. The GLSL package (gta4_deko3d_glsl.bin, gta4_deko3d_package.py) stays for the variants
uam could not compile on the PC (glsl_index points into it).

Format (little endian, version 2; the full layout and the rules the console's reader enforces are in
tools/deko3d/gta4_deko3d_runtime_check.py, which also validates the file written here before it replaces the
old one):
  "GDKR", u32 version (2), u32 shader count, u32 blob count, u32 tag length, u32 record size (48), u64 file size,
  the tag (the DKSH cache's), zero padding to 8 bytes;
  shader count x 48 bytes, sorted by (hash, late):
    u64 hash; u8 stage (0 vertex, 1 pixel); u8 late; u8 input count; u8 variant count;
    u32 used texture mask (shader_cache.cpp); u32 specialization mask; u32 glsl index (0xFFFFFFFF: none);
    u32 inputs offset; u32 variants offset; u32 texture types offset; u32 outputs (pixel: the colour
    output locations written, bit per location; vertex: 0); u32 reserved; u32 padding (8-byte records);
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
import gta4_deko3d_runtime_check as fmt  # noqa: E402
import gta4_deko3d_shaders as chain  # noqa: E402

ENTRY_RE = re.compile(r'^\t\{ 0x([0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), '
                      r'"[^"]*", nullptr, (\d+) \}')
INPUT_RE = re.compile(r'^layout\(location = (\d+)\) in (vec|ivec|uvec|float|int|uint)(\d?) \w+;$', re.M)
OUTPUT_RE = re.compile(r'^layout\(location = (\d+)\) out \w+ \w+;$', re.M)
MAP_RE = re.compile(r'^// gta4-dk vertex-inputs(.*)$', re.M)
TEXTURE_RE = re.compile(r'^layout\(binding = (\d+)\) uniform (sampler2D|sampler2DArray|sampler3D|samplerCube) \w+;$',
                        re.M)
TEXTURE_TYPES = {'sampler2D': 1, 'sampler2DArray': 2, 'sampler3D': 3, 'samplerCube': 4}
PRUNE_ZERO_RE = re.compile(r'^// banjo-vszero=0x([0-9a-fA-F]+)\s*$', re.M)
PS_INPUT_RE = re.compile(r'^layout\(location = (\d+)\)\s+(?:(?:flat|noperspective|centroid|smooth|sample)\s+)*in\s', re.M)
VS_OUTPUT_RE = re.compile(r'^layout\(location = (\d+)\) out vec4 (\w+);$', re.M)
GATE_RE = re.compile(r'^\s*if \(\(g_banjoVsOutputs & (\d+)u\) != 0u\) \{ (\w+) =', re.M)


def gate_bits_are_locations(text):
    """Run 56: True when every output gate `if ((g_banjoVsOutputs & 2^k) ...) { out = ...` names the output declared at
    location k (so the zero mask and a pixel shader's input locations count the same things), and all 18 are there."""
    locations = {name: int(loc) for loc, name in VS_OUTPUT_RE.findall(text)}
    gates = GATE_RE.findall(text)
    if len(gates) != 18:
        return False
    for bit, name in gates:
        b = int(bit)
        if b & (b - 1) or locations.get(name) != b.bit_length() - 1:
            return False
    return True
NUMERIC = {'vec': 0, 'float': 0, 'ivec': 1, 'int': 1, 'uvec': 2, 'uint': 2}


def dksh_pixel_flags(blob):
    """Run 55: KillsPixels and the depth output of a compiled pixel shader, from its NVIDIA shader program header (SPH),
    which uam writes at the program's entry point in the code section (thirdparty/uam/source/nv_shader_header.h:
    CommonWord0 bit 15 kills_pixels; the PS part's word at byte 76, bit 1 omap_depth). The compiled program, not the
    text: the text's alpha-test discard is under a specialization constant and goes in the variants without it.
    None when the blob is not a one-program fragment DKSH."""
    if len(blob) < 24:
        return None
    magic, _, control, _, programs_off, programs = struct.unpack_from('<6I', blob, 0)
    if magic != 0x48534B44 or programs != 1 or programs_off + 64 > len(blob):
        return None
    ptype, entry = struct.unpack_from('<II', blob, programs_off)
    sph = control + entry
    if ptype != 1 or sph + 80 > len(blob):
        return None
    word0 = struct.unpack_from('<I', blob, sph)[0]
    if word0 & 0x1F != 2:  # NvSphType_PS
        return None
    flags = 0
    if word0 & (1 << 15):
        flags |= fmt.FLAG_KILLS
    if struct.unpack_from('<I', blob, sph + 76)[0] & 2:
        flags |= fmt.FLAG_WRITES_DEPTH
    return flags


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
    ap.add_argument('--pruned-dksh', default='',
                    help='run 56: gta4_deko3d_vs_prune.py\'s folder (prune.json and <stem>_<value>_p/_l.dksh): the '
                         'vertex shader variants that write fewer outputs, added under value | bit 31 / bit 30, only '
                         'those whose program header passed the check')
    a = ap.parse_args()
    pruned = {}
    if a.pruned_dksh:
        import json
        for r in json.load(open(os.path.join(a.pruned_dksh, 'prune.json'))):
            pruned[(r['stem'], r['value'])] = r
    entries = cache_entries(a.cache_cpp)
    masks = {h: e[0] for h, e in entries.items()}
    opt = os.path.join(a.out_dir, 'glsl_opt')
    order = glsl_order(opt, masks)
    tag, pc_blobs = read_dksh_cache(os.path.join(a.out_dir, 'gta4_deko3d_shaders.bin'))

    shaders = []  # (hash, late, stage, inputs, variants, used, mask, glsl_index, types, outputs)
    blob_index = {}
    blob_list = []
    missing = 0
    pruned_added = 0
    gate_mismatch = 0
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
        flags = fmt.FLAGS_VALID
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
                if stage == 1 and not value & 0x2:
                    flags &= ~fmt.FLAGS_VALID  # a variant the depth-only rule could meet, not known
                continue
            if stage == 1 and not value & 0x2:
                # Run 55: the variants without the alpha test (the only ones a depth-only draw can have) decide.
                f = dksh_pixel_flags(blob)
                if f is None:
                    flags &= ~fmt.FLAGS_VALID
                else:
                    flags |= f
            if key not in blob_index:
                blob_index[key] = len(blob_list)
                blob_list.append(blob)
            variants.append((value, blob_index[key]))
        # Run 56: the io mask (record word 44, flag bit 3): a vertex shader's outputs written as the constant zero, a
        # pixel shader's declared input locations; and the pruned vertex shader variants.
        io_mask = 0
        if stage == 0:
            z = PRUNE_ZERO_RE.search(text)
            if z and not gate_bits_are_locations(text):
                z = None  # the mask's bits would not be the pixel shader's input locations: no mask, no pruning
                gate_mismatch += 1
            if z:
                io_mask = int(z.group(1), 16) & ((1 << 18) - 1)
                flags |= fmt.FLAG_IO_MASK
            stem = os.path.splitext(name)[0]
            for value in list(chain.variants(mask)) if not late else []:
                r = pruned.get((stem, value))
                if not r or r.get('zero') != io_mask:
                    continue
                for kind, bit in (('p', fmt.VARIANT_POSITION_ONLY), ('l', fmt.VARIANT_LIVE_OUTPUTS)):
                    path = os.path.join(a.pruned_dksh, f'{stem}_{value:x}_{kind}.dksh')
                    if not r.get(kind, {}).get('ok') or not os.path.isfile(path):
                        continue
                    blob = open(path, 'rb').read()
                    key = chain.fnv1a64(blob)
                    if key not in blob_index:
                        blob_index[key] = len(blob_list)
                        blob_list.append(blob)
                    variants.append((value | bit, blob_index[key]))
                    pruned_added += 1
        else:
            for im in PS_INPUT_RE.finditer(text):
                io_mask |= 1 << int(im.group(1))
            flags |= fmt.FLAG_IO_MASK
        shaders.append((h, late, stage, inputs, variants, used, mask, glsl_index, types, outputs, flags, io_mask))
    shaders.sort(key=lambda s: (s[0], s[1]))

    header_len = fmt.align(fmt.HEADER_BYTES + len(tag), 8)
    table_pos = header_len
    extra_pos = table_pos + fmt.RECORD_BYTES * len(shaders)
    extra = b''
    table = b''
    for h, late, stage, inputs, variants, used, mask, glsl_index, types, outputs, flags, io_mask in shaders:
        inputs_off = extra_pos + len(extra)
        extra += b''.join(struct.pack('<BBBB', *i) for i in inputs)
        variants_off = extra_pos + len(extra)
        extra += b''.join(struct.pack('<II', *v) for v in variants)
        types_off = extra_pos + len(extra)
        extra += bytes(types)
        # flags and the io mask are the last two u32s: the record is RECORD_BYTES long, not one byte short of it
        table += fmt.RECORD.pack(h, stage, late, len(inputs), len(variants), used, mask, glsl_index,
                                 inputs_off, variants_off, types_off, outputs, flags, io_mask)
    assert len(table) == fmt.RECORD_BYTES * len(shaders)
    blobs_table_pos = fmt.align(extra_pos + len(extra), 256)
    data_pos = fmt.align(blobs_table_pos + fmt.BLOB_ENTRY_BYTES * len(blob_list), 256)
    blob_table = b''
    blob_data = b''
    for blob in blob_list:
        blob_table += struct.pack('<II', data_pos + len(blob_data), len(blob))
        blob_data += blob + b'\0' * ((-len(blob)) % 256)
    total = data_pos + len(blob_data)
    header = fmt.HEADER.pack(fmt.MAGIC, fmt.VERSION, len(shaders), len(blob_list), len(tag), fmt.RECORD_BYTES, total)
    header += tag
    header += b'\0' * (header_len - len(header))
    out = header + table + extra
    out += b'\0' * (blobs_table_pos - len(out))
    out += blob_table
    out += b'\0' * (data_pos - len(out))
    out += blob_data
    assert len(out) == total, (len(out), total)
    path = os.path.join(a.out_dir, 'gta4_deko3d_runtime.bin')
    # Written beside the final name and put in place only if the validator (the console reader's rules) accepts it:
    # a broken file never replaces a good one.
    tmp = path + '.new'
    with open(tmp, 'wb') as f:
        f.write(out)
    try:
        info = fmt.check_file(tmp, expect_shaders=len(shaders), expect_blobs=len(blob_list))
    except fmt.FormatError as e:
        rejected = path + '.rejected'
        os.replace(tmp, rejected)
        raise SystemExit(f'{rejected}: NOT installed, the validator rejects it: {e}')
    try:
        os.replace(tmp, path)
    except PermissionError as e:
        # Windows: the old file is open in another process (a probe, an indexer, a copy to the card)
        raise SystemExit(f'{path} is in use ({e}): the validated package waits as {tmp}; close what holds the old '
                         f'file and rename it')
    variants = sum(len(s[4]) for s in shaders)
    print(f'{path}: {len(shaders)} shaders, {variants} variants ({missing} without DKSH, {pruned_added} pruned vertex '
          f'shader variants, {gate_mismatch} vertex shaders whose gates are not their locations), {len(blob_list)} DKSH, {len(out)} bytes, version {info["version"]}, validated; sha256 '
          f'{fmt.sha256(path)}')


if __name__ == '__main__':
    main()
