#!/usr/bin/env python3
"""Theft4-NX (docs/switch-port/20, U8): the game captures of a run (nfsmw_captura_cada_s, capturas/captura_NNN_SSSSs.png)
measured, and paired by second with a reference run's (the PC build from the same save, or another console run).

    python tools/ultimate/u8_capture_compare.py <capturas folder> [--reference <capturas folder>] [--window 10]

Per capture: the mean luma (Rec. 601, 0-255) of the whole frame and of its central third (where the player is), and the
share of near-black pixels (luma < 8). With --reference, each capture is paired with the reference capture closest in
seconds (within --window), and the ratios of the means are printed: a lighting fault that blackens the scene shows as a
central-third ratio far below 1 and a near-black share far above the reference's. Reads only. Needs Pillow.
"""
import argparse
import os
import re
import sys

from PIL import Image, ImageStat

NAME_RE = re.compile(r'captura_(\d+)_(\d+)s\.png$')


def measure(path):
    img = Image.open(path).convert('L')
    w, h = img.size
    whole = ImageStat.Stat(img).mean[0]
    centre = ImageStat.Stat(img.crop((w // 3, h // 3, 2 * w // 3, 2 * h // 3))).mean[0]
    hist = img.histogram()
    black = sum(hist[:8]) / float(w * h)
    return whole, centre, black


def load(folder):
    out = []
    for name in sorted(os.listdir(folder)):
        m = NAME_RE.search(name)
        if m:
            out.append((int(m.group(2)), name, measure(os.path.join(folder, name))))
    if not out:
        sys.exit(f'u8_capture_compare: no captura_NNN_SSSSs.png in {folder}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--reference', default=None)
    ap.add_argument('--window', type=int, default=10)
    a = ap.parse_args()
    run = load(a.folder)
    ref = load(a.reference) if a.reference else []
    print(f"{'capture':24} {'luma':>6} {'centre':>7} {'black%':>7}" +
          (f"   {'reference':24} {'luma':>6} {'centre':>7} {'black%':>7} {'luma x':>7} {'centre x':>8}" if ref else ''))
    for s, name, (whole, centre, black) in run:
        line = f'{name:24} {whole:6.1f} {centre:7.1f} {100 * black:7.1f}'
        if ref:
            near = min(ref, key=lambda r: abs(r[0] - s))
            if abs(near[0] - s) <= a.window:
                rw, rc, rb = near[2]
                line += (f'   {near[1]:24} {rw:6.1f} {rc:7.1f} {100 * rb:7.1f} {whole / max(rw, 1e-6):7.2f} '
                         f'{centre / max(rc, 1e-6):8.2f}')
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
