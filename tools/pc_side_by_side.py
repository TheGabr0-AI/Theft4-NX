#!/usr/bin/env python3
"""Side-by-side PNGs of PC runs' captures (docs/switch-port/16-deko3d-devlog.md, M0).

    python tools/pc_side_by_side.py --out <dir> --run <dir>:<label> [--run ...] [--height 360]

Each run folder holds caps/cap-tNNNN.ppm (tools/pc-eu-base, pc_capture_every_s: the presenter's guest
output). For every capture time present in the first run, the nearest capture of each run is scaled
(nearest neighbour) to --height and placed left to right; the label goes in the file list
(index.txt), not in the picture. Also prints the mean absolute difference of each run against the
first, per time, on the scaled images. Pure Python (zlib only). The pictures are game output: they
stay out of the repository.
"""
import argparse
import os
import re
import struct
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


def scale(img, height):
    w, h, px = img
    nw = max(1, w * height // h)
    out = bytearray(nw * height * 3)
    for y in range(height):
        sy = y * h // height
        row = px[sy * w * 3:(sy + 1) * w * 3]
        for x in range(nw):
            sx = (x * w // nw) * 3
            out[(y * nw + x) * 3:(y * nw + x) * 3 + 3] = row[sx:sx + 3]
    return nw, height, bytes(out)


def write_png(path, w, h, px):
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def captures(run):
    caps = {}
    for f in os.listdir(os.path.join(run, 'caps')):
        m = re.match(r'cap-t(\d+)\.ppm$', f)
        if m:
            caps[int(m.group(1))] = os.path.join(run, 'caps', f)
    return caps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--run', action='append', required=True)
    ap.add_argument('--height', type=int, default=360)
    a = ap.parse_args()
    runs = [r.rsplit(':', 1) for r in a.run]
    caps = [captures(r) for r, _ in runs]
    os.makedirs(a.out, exist_ok=True)
    index = [' | '.join(label for _, label in runs)]
    for t in sorted(caps[0]):
        imgs = []
        for c in caps:
            near = min(c, key=lambda k: abs(k - t)) if c else None
            imgs.append((near, scale(read_ppm(c[near]), a.height)) if near is not None else (None, None))
        total_w = sum(i[1][0] for i in imgs if i[1])
        out = bytearray(total_w * a.height * 3)
        x0 = 0
        for _, img in imgs:
            if not img:
                continue
            w, h, px = img
            for y in range(h):
                out[(y * total_w + x0) * 3:(y * total_w + x0 + w) * 3] = px[y * w * 3:(y + 1) * w * 3]
            x0 += w
        name = f'side-t{t:04d}.png'
        write_png(os.path.join(a.out, name), total_w, a.height, bytes(out))
        diffs = []
        base = imgs[0][1]
        for near, img in imgs[1:]:
            if img and base and img[0] == base[0]:
                d = sum(abs(p - q) for p, q in zip(base[2][::97], img[2][::97])) / len(base[2][::97])
                diffs.append(f'{d:.1f}')
            else:
                diffs.append('-')
        index.append(f'{name}: t={[n for n, _ in imgs]} mean-abs-diff-vs-first={diffs}')
    open(os.path.join(a.out, 'index.txt'), 'w').write('\n'.join(index) + '\n')
    print('\n'.join(index))


if __name__ == '__main__':
    main()
