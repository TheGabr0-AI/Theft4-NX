#!/usr/bin/env python3
"""Turns gta4_renut_diag_dump_dir's .raw files (ReNut's resolve readbacks, docs/switch-port/18) into PNG files.

Each .raw: a 20-byte header (width, height, plume RenderFormat, bytes a texel, texels a row; little-endian u32), then
the rows. RGBA8 is written as it is (alpha dropped); 16-bit float and 16-bit unorm as their RGB scaled to the file's
own largest value (printed); one-channel 32-bit float as grey, scaled from its smallest to largest value.

    python tools/renut-gta4/raw2png.py <dump dir> [<out dir>]
"""
import glob
import os
import struct
import sys
import zlib

# plume::RenderFormat values (plume_render_interface_types.h)
R16G16B16A16_FLOAT, R16G16B16A16_UNORM, R8G8B8A8_UNORM, D32_FLOAT, R32_FLOAT, R16G16_FLOAT = 10, 11, 20, 32, 34, 26


def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def convert(path, out_dir):
    data = open(path, 'rb').read()
    w, h, fmt, texel, row = struct.unpack('<5I', data[:20])
    body = data[20:]
    rows = [body[y * row * texel:(y * row + w) * texel] for y in range(h)]
    flat = b''.join(rows)
    note = ''
    if fmt == R8G8B8A8_UNORM:
        rgb = bytearray(w * h * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = flat[0::4], flat[1::4], flat[2::4]
    elif fmt in (R16G16B16A16_FLOAT, R16G16B16A16_UNORM, R16G16_FLOAT):
        n = 4 if fmt != R16G16_FLOAT else 2
        code = 'e' if fmt != R16G16B16A16_UNORM else 'H'
        values = struct.unpack('<%d%s' % (w * h * n, code), flat)
        if code == 'H':
            values = [v / 65535.0 for v in values]
        chans = [values[c::n] for c in range(min(n, 3))]
        while len(chans) < 3:
            chans.append([0.0] * (w * h))
        finite = [v for c in chans for v in c if v == v and abs(v) != float('inf')]
        top = max(finite) if finite else 1.0
        top = top if top > 0 else 1.0
        rgb = bytearray(w * h * 3)
        for c in range(3):
            rgb[c::3] = bytes(max(0, min(255, int(v / top * 255.0))) if v == v else 255 for v in chans[c])
        note = 'largest %.4g, alpha mean %.4g' % (top, (sum(values[3::4]) / (w * h)) if n == 4 else 0.0)
    elif fmt in (R32_FLOAT, D32_FLOAT):
        values = struct.unpack('<%df' % (w * h), flat)
        lo, hi = min(values), max(values)
        span = (hi - lo) or 1.0
        g = bytes(int((v - lo) / span * 255.0) for v in values)
        rgb = bytearray(w * h * 3)
        rgb[0::3] = rgb[1::3] = rgb[2::3] = g
        note = 'range %.6g .. %.6g' % (lo, hi)
    else:
        print('%s: format %d not converted' % (os.path.basename(path), fmt))
        return
    out = os.path.join(out_dir, os.path.basename(path)[:-4] + '.png')
    png(out, w, h, bytes(rgb))
    print('%s %dx%d fmt %d %s' % (os.path.basename(out), w, h, fmt, note))


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src
    os.makedirs(out, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(src, '*.raw'))):
        convert(path, out)


if __name__ == '__main__':
    main()
