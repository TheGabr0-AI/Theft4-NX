#!/usr/bin/env python3
"""Theft4-NX: every Renut hook of the Ultimate Base bound to an address GTA IV's table does not name becomes the base's
own unhooked form (docs/switch-port/20, U4b). The hook's host function stays compiled; nothing is registered at the
address, so no Beautiful Katamari address replaces a GTA IV function.

  BANJO_RENUT_HOOK(sub_X, f)        -> BANJO_RENUT_UNHOOKED_HOOK(sub_X, f)
  BANJO_RENUT_HOOK_SHARED(sub_X, f) -> BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_X, f)
  BANJO_RENUT_HOOK_RAW(sub_X)       -> BANJO_RENUT_UNHOOKED_HOOK_RAW(sub_X)
  BANJO_RENUT_HOOK_RAW_SHARED(...)  -> BANJO_RENUT_UNHOOKED_HOOK_RAW_SHARED(...)

Each changed line gets the note "theft4: not hooked on GTA IV (Katamari's address)". Lines whose address is in
tools/ultimate/gta4_hook_bindings.csv are left alone. Idempotent.

    python tools/ultimate/unhook_katamari.py [--ultimate ultimate] [--dry-run] [files...]

Default files: the base's d3d/hooks/*.cpp.
"""
import argparse
import csv
import os
import re

NOTE = '  // theft4: not hooked on GTA IV (Katamari\'s address)'
LINE = re.compile(r'^(\s*)BANJO_RENUT_HOOK(_SHARED|_RAW|_RAW_SHARED)?\(\s*sub_([0-9A-Fa-f]{8})\b')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ultimate', default='ultimate')
    ap.add_argument('--bindings', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gta4_hook_bindings.csv'))
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    keep = {r['address'].strip().upper() for r in csv.DictReader(l for l in open(a.bindings, encoding='utf-8')
                                                                  if not l.startswith('#')) if r['kind'] == 'binding'}
    hooks = os.path.join(a.ultimate, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')
    files = a.files or sorted(os.path.join(hooks, f) for f in os.listdir(hooks) if f.endswith('.cpp'))
    total = 0
    for path in files:
        raw = open(path, 'rb').read()
        crlf = b'\r\n' in raw
        lines = raw.decode('utf-8').replace('\r\n', '\n').split('\n')
        changed = 0
        for i, line in enumerate(lines):
            m = LINE.match(line)
            if not m or m.group(3).upper() in keep:
                continue
            suffix = m.group(2) or ''
            new = line.replace('BANJO_RENUT_HOOK' + suffix + '(', 'BANJO_RENUT_UNHOOKED_HOOK' + suffix + '(', 1)
            if NOTE.strip() not in new:
                new = new.rstrip() + NOTE
            lines[i] = new
            changed += 1
        if changed and not a.dry_run:
            text = '\n'.join(lines)
            open(path, 'wb').write((text.replace('\n', '\r\n') if crlf else text).encode('utf-8'))
        print(f'{os.path.basename(path)}: {changed} unhooked')
        total += changed
    print(f'unhook_katamari: {total} bindings unhooked' + (' (dry run)' if a.dry_run else ''))


if __name__ == '__main__':
    main()
