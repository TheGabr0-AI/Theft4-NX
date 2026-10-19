#!/usr/bin/env python3
"""Native renderer, step N2: the side-by-side images of the captures written
by src/graphics/native/native_render_targets.cpp (cvar native_capture_every_s):
for each pair, the frame drawn emulated, the next frame drawn native, and
their difference (x4), at half size, one PNG per pair; plus one line per pair
with the share of pixels that differ by more than 24 (sum of R, G, B).

Usage: native_compare_frames.py <capture dir> [<out dir>]
Pure Python (no Pillow): PPM in, PNG out.
"""
import glob
import os
import re
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(maxsplit=4)  # P6, width, height, maxval, pixels
    w, h = int(parts[1]), int(parts[2])
    header_end = len(data) - w * h * 3
    return w, h, data[header_end:]


def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src
    os.makedirs(out, exist_ok=True)
    pairs = {}
    for p in glob.glob(os.path.join(src, 'n2-*pair*-frame*-*.ppm')):
        m = re.search(r'(n2-[\w-]*?pair)(\d+)-frame(\d+)-(emulated|native|test)\.ppm$', p)
        if m:
            mode = 'native' if m.group(4) == 'test' else m.group(4)
            pairs.setdefault((m.group(1), int(m.group(2))), {})[mode] = (int(m.group(3)), p)
    for pair in sorted(pairs):
        if 'emulated' not in pairs[pair] or 'native' not in pairs[pair]:
            continue
        fe, pe = pairs[pair]['emulated']
        fn, pn = pairs[pair]['native']
        we, he, de = read_ppm(pe)
        wn, hn, dn = read_ppm(pn)
        if (we, he) != (wn, hn):
            print(f'pair {pair}: sizes differ ({we}x{he} / {wn}x{hn})')
            continue
        hw, hh = we // 2, he // 2
        rows = []
        differ = 0
        total = 0
        for y in range(hh):
            row = bytearray(hw * 3 * 3)
            sy = y * 2
            for x in range(hw):
                o = (sy * we + x * 2) * 3
                e = de[o:o + 3]
                n = dn[o:o + 3]
                d = [min(255, abs(e[i] - n[i]) * 4) for i in range(3)]
                total += 1
                if abs(e[0] - n[0]) + abs(e[1] - n[1]) + abs(e[2] - n[2]) > 24:
                    differ += 1
                row[x * 3:x * 3 + 3] = e
                row[(hw + x) * 3:(hw + x) * 3 + 3] = n
                row[(2 * hw + x) * 3:(2 * hw + x) * 3 + 3] = bytes(d)
            rows.append(row)
        name = os.path.join(out, f'{pair[0]}{pair[1]:02d}-frames{fe:05d}-{fn:05d}.png')
        write_png(name, hw * 3, hh, rows)
        mean_e = sum(de[::3 * 7]) / max(1, len(de[::3 * 7]))
        print(f'{pair[0]}{pair[1]:2d}: frames {fe} (emulated) / {fn} (native): {100.0 * differ / total:5.1f}% '
              f'of pixels differ by more than 24; emulated mean red {mean_e:.0f} -> {name}')


if __name__ == '__main__':
    main()
