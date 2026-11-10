#!/usr/bin/env python3
"""Makes the generated code call ReNut's hooked Direct3D functions through their hookable names.

tools/switch-codegen/direct_calls.py turned the calls of functions that had no hook into __imp__sub_X (the function
itself, inlinable). The functions ReNut's path hooks (gta4_d3d.h, THEFT4_RENUT_D3D_FUNCTIONS) had none then: the
ring and fence functions are called that way 122 times, and a hook on sub_X would never run (sa2-nx met the same:
"the first run dropped every draw"). This turns those calls, and only those, back into sub_X(ctx, base). Without a
hook linked sub_X is an alias of __imp__sub_X, so the build without ReNut runs the same code.

The generated code is local (not in the repository). Run again after every code generation or direct_calls.py run.

  python tools/renut-gta4/weak_calls.py [--gen <generated/eu-base>] [--check] [--undo]
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GEN_DEFAULT = os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'gta4-recomp', 'generated', 'eu-base')


def hooked():
    text = open(os.path.join(HERE, 'gta4_d3d.h'), encoding='utf-8').read()
    return sorted(set(re.findall(r'^\s*X\(\w+, ([0-9A-F]{8}),', text, re.M)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN_DEFAULT)
    ap.add_argument('--check', action='store_true', help='count only; exit 1 if a direct call is left')
    ap.add_argument('--undo', action='store_true', help='direct calls again')
    args = ap.parse_args()
    addresses = hooked()
    if not addresses:
        sys.exit('weak_calls: no function in gta4_d3d.h')
    alt = '|'.join(addresses)
    direct = re.compile(rb'(?<![A-Za-z0-9_])__imp__(sub_(?:%s))\(ctx, base\)' % alt.encode())
    weak = re.compile(rb'(?<![A-Za-z0-9_])(sub_(?:%s))\(ctx, base\)' % alt.encode())
    files = sorted(glob.glob(os.path.join(args.gen, 'gta4_recomp.*.cpp')))
    if not files:
        sys.exit('weak_calls: no generated code in %s' % args.gen)
    total_direct = total_weak = changed = 0
    for path in files:
        data = open(path, 'rb').read()
        n_direct, n_weak = len(direct.findall(data)), len(weak.findall(data))
        total_direct += n_direct
        total_weak += n_weak
        if args.check:
            continue
        if args.undo:
            new = weak.sub(rb'__imp__\1(ctx, base)', data)
        else:
            new = direct.sub(rb'\1(ctx, base)', data)
        if new != data:
            open(path, 'wb').write(new)
            changed += 1
    print('weak_calls: %d hooked functions, %d files; before: %d direct calls, %d hookable calls; %d files rewritten'
          % (len(addresses), len(files), total_direct, total_weak, changed))
    if args.check and total_direct:
        sys.exit(1)


if __name__ == '__main__':
    main()
