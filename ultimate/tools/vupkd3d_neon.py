#!/usr/bin/env python3
"""renut-ultimate port 73 (2026-11-11): sasrt-nx's tool as it was that day (the base runs it with
--gen app/generated). sasrt-nx (2026-11-11, machine-copy idea 43, B3): vupkd3d128 type 0 (D3DCOLOR to four floats 1.0 + byte/2^23) as one
NEON table lookup (post-codegen step, before tools/localize_native.py so the copies get it too).

The generator writes it lane by lane (sdk/src/codegen/builders/vector.cpp, "TODO: Vectorize"):
    vTemp.u32[0] = ctx.v62.u8[3] | 0x3F800000;   ... [1] = .u8[0], [2] = .u8[1], [3] = .u8[2]
    ctx.v8 = vTemp;
which GCC builds in two general registers (bfxil / bfi) and moves to the vector unit: seven instructions and two
GPR-to-SIMD transfers, in WaterThread's inner loop (sub_8286C230) on every iteration. The four lane lines become
REX_VUPKD3D_COLOR(vTemp, src): on AArch64 `tbx` over a vector of 1.0f (bytes 00 00 80 3F per lane) with the indices
{3,-,-,-, 0,-,-,-, 1,-,-,-, 2,-,-,-}: each lane's low byte is the source byte, the other three keep 00 80 3F, which is
`byte | 0x3F800000` exactly. Elsewhere, or with REX_VUPKD3D_LANES, the macro is the four lane lines. The line
`dst = vTemp;` stays.

  python tools/vupkd3d_neon.py --gen app/generated/default [--check] [--restore]
Idempotent: the rewritten lines no longer match; --restore writes the four lane lines back.
"""
import argparse
import glob
import os
import re
import sys

LANES = re.compile(r'^(\t+)vTemp\.u32\[0\] = ([\w.]+)\.u8\[3\] \| 0x3F800000;\n'
                   r'\1vTemp\.u32\[1\] = \2\.u8\[0\] \| 0x3F800000;\n'
                   r'\1vTemp\.u32\[2\] = \2\.u8\[1\] \| 0x3F800000;\n'
                   r'\1vTemp\.u32\[3\] = \2\.u8\[2\] \| 0x3F800000;\n', re.M)
MACRO_USE = re.compile(r'^(\t+)REX_VUPKD3D_COLOR\(vTemp, ([\w.]+)\);\n', re.M)
MARKER = '// rexglue ports: vupkd3d128 type 0 as one table lookup (tools/vupkd3d_neon.py)'
END = '// (end of tools/vupkd3d_neon.py)\n'
MACROS = MARKER + '''
#ifndef REX_VUPKD3D_COLOR
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_VUPKD3D_LANES)
#include <arm_neon.h>
#define REX_VUPKD3D_COLOR(dst, src)                                                                             \\
  do {                                                                                                          \\
    const uint8x16_t rex_vupkd3d_idx_ = {3, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0xFF, 0xFF,                             \\
                                         1, 0xFF, 0xFF, 0xFF, 2, 0xFF, 0xFF, 0xFF};                            \\
    vst1q_u8((dst).u8, vqtbx1q_u8(vreinterpretq_u8_f32(vdupq_n_f32(1.0f)), vld1q_u8((src).u8), rex_vupkd3d_idx_)); \\
  } while (0)
#else
#define REX_VUPKD3D_COLOR(dst, src)                \\
  do {                                             \\
    (dst).u32[0] = (src).u8[3] | 0x3F800000;       \\
    (dst).u32[1] = (src).u8[0] | 0x3F800000;       \\
    (dst).u32[2] = (src).u8[1] | 0x3F800000;       \\
    (dst).u32[3] = (src).u8[2] | 0x3F800000;       \\
  } while (0)
#endif
#endif
''' + END
ANCHOR = '#define REX_LOAD_U8(x)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--restore', action='store_true')
    a = ap.parse_args()
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vupkd3d_neon: no single *_pch.h in %s' % a.gen)
    files = sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp')))
    found = changed = 0
    for path in files:
        text = open(path, encoding='utf-8').read()
        if a.restore:
            new = MACRO_USE.sub(lambda m: ''.join('%svTemp.u32[%d] = %s.u8[%d] | 0x3F800000;\n' % (m.group(1), i, m.group(2), k)
                                                  for i, k in ((0, 3), (1, 0), (2, 1), (3, 2))), text)
        else:
            new, n = LANES.subn(lambda m: '%sREX_VUPKD3D_COLOR(vTemp, %s);\n' % (m.group(1), m.group(2)), text)
            found += n
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pch = pchs[0]
    text = open(pch, encoding='utf-8').read()
    if MARKER in text:
        start = text.index(MARKER)
        text = text[:start] + text[text.index(END, start) + len(END):]
    if not a.restore:
        if ANCHOR not in text:
            sys.exit('ERROR (post-codegen patch): vupkd3d_neon: no %s in %s' % (ANCHOR, pch))
        text = text.replace(ANCHOR, MACROS + ANCHOR, 1)
    if not a.check and text != open(pch, encoding='utf-8').read():
        open(pch, 'w', encoding='utf-8', newline='\n').write(text)
    print('vupkd3d_neon: %s %d vupkd3d128 type-0 sites in %d files' % (
        'restored' if a.restore else 'one table lookup at', found, changed))


if __name__ == '__main__':
    main()
