#!/usr/bin/env python3
"""Two runtime packages (gta4_deko3d_runtime.bin) compared shader by shader (16-deko3d-devlog.md, run 61).

    python tools/deko3d/gta4_deko3d_package_diff.py <old.bin> <new.bin>

For every (hash, late) of the old package: the new one must have it with the same stage, masks, outputs, flags, io
mask, inputs, texture types and, per specialization value, a byte-identical DKSH. Prints the shaders only the new one
has. Exit 1 on any difference. (The glsl index and the offsets are positions in the file, not compared.)
"""
import struct
import sys


def load(path):
    data = open(path, 'rb').read()
    if data[:4] != b'GDKR':
        raise SystemExit(f'{path}: not a runtime package')
    _v, count, blob_count, tag_len, record = struct.unpack_from('<5I', data, 4)
    table_at = 32 + tag_len + ((-tag_len) % 8)
    extra_end = table_at + count * record
    shaders = {}
    recs = []
    for i in range(count):
        o = table_at + i * record
        h, stage, late, n_in, n_var, tex_mask, spec_mask, _glsl, in_off, var_off, tex_off, outputs, flags, io = \
            struct.unpack_from('<QBBBBIIIIIIIII', data, o)
        recs.append((h, stage, late, n_in, n_var, tex_mask, spec_mask, in_off, var_off, tex_off, outputs, flags, io))
        extra_end = max(extra_end, in_off + 4 * n_in, var_off + 8 * n_var, tex_off + 32)
    blob_table = (extra_end + 255) // 256 * 256
    blobs = [struct.unpack_from('<II', data, blob_table + 8 * i) for i in range(blob_count)]
    for h, stage, late, n_in, n_var, tex_mask, spec_mask, in_off, var_off, tex_off, outputs, flags, io in recs:
        variants = {}
        for j in range(n_var):
            value, idx = struct.unpack_from('<II', data, var_off + 8 * j)
            variants[value] = None if idx == 0xFFFFFFFF else data[blobs[idx][0]:blobs[idx][0] + blobs[idx][1]]
        shaders[(h, late)] = dict(stage=stage, tex_mask=tex_mask, spec_mask=spec_mask, outputs=outputs, flags=flags,
                                  io=io, inputs=data[in_off:in_off + 4 * n_in], textures=data[tex_off:tex_off + 32],
                                  variants=variants)
    return shaders


def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    differ = []
    for key, s in old.items():
        n = new.get(key)
        if n is None:
            differ.append(f'{key[0]:016X}{"_late" if key[1] else ""}: missing from the new package')
            continue
        for f in ('stage', 'tex_mask', 'spec_mask', 'outputs', 'flags', 'io', 'inputs', 'textures'):
            if s[f] != n[f]:
                differ.append(f'{key[0]:016X}{"_late" if key[1] else ""}: {f} differs')
        if s['variants'] != n['variants']:
            changed = [f'{v:x}' for v in set(s['variants']) | set(n['variants'])
                       if s['variants'].get(v) != n['variants'].get(v)]
            differ.append(f'{key[0]:016X}{"_late" if key[1] else ""}: variants differ ({", ".join(changed[:6])})')
    added = sorted(k for k in new if k not in old)
    print(f'old {len(old)} shaders, new {len(new)}; of the old: {len(old) - len(differ)} identical, '
          f'{len(differ)} different; added {len(added)} '
          f'({sum(1 for k in added if not k[1])} shaders + {sum(1 for k in added if k[1])} late sources)')
    for d in differ[:40]:
        print('  ' + d)
    return 1 if differ else 0


if __name__ == '__main__':
    sys.exit(main())
