#!/usr/bin/env python3
"""Theft4-NX: device offsets written inline in the Ultimate Base's renderer, mapped to GTA IV (docs/switch-port/20, U4).

The base's d3d.h / d3d_native.h are not the only place its renderer names device offsets: hook bodies write them
inline (d.W(0x5404), d.SetW(0x2F98, 5), word(16552), WriteU32(bytes, 10900, ...), local constexpr kDeviceOffset*),
most with a "katamari-nx: <N&B> -> <Katamari>" note. For each, this tool:
  - takes N&B's value from that note (or the value itself when there is no note: Katamari kept N&B's there),
  - maps it to GTA IV with the zones verified in docs/switch-port/18 section 2 (N&B's up to the flags at 10943; -8 from
    the vertex declaration 11820 to the tiling extent 13180; -16 from the KickOff word 13232 to the side buffer 16728;
    -20 in the tail from 21532; anything else: zone unknown),
  - and looks for the GTA IV value as a displacement in the listing of GTA IV's Direct3D library (829C7000-829E4600) in
    the 0.10 generated code, naming the functions that use it.

    python tools/ultimate/device_offset_census.py [--ultimate ultimate] [--report out.txt] files...

Default files: the base's d3d/hooks/*.cpp and d3d/d3d_native_state.cpp. Reports; changes nothing.
"""
import argparse
import os
import re

NB_ZONES = [  # (first N&B offset, last N&B offset, delta to GTA IV)
    (0, 10943, 0),
    (11820, 13180, -8),
    (13232, 16728, -16),
    (21532, 22000, -20),  # SetFence's N&B 21532 is GTA IV's 21512 (the working renderer, R1)
]
OFFSET_RE = re.compile(
    r'(?:\bd\.(?:W|B|H|SetW|SetB|SetH|Q|SetQ)\(|\bword\(|WriteU32\(bytes,\s*|ReadU32\(bytes,\s*|bytes\s*\+\s*|bytes\[)'
    r'\s*(0x[0-9A-Fa-f]+|\d+)')
CONST_RE = re.compile(r'constexpr\s+u(?:32|16|8)\s+(k\w*(?:Offset|Device)\w*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)')
NOTE_RE = re.compile(r'katamari-nx:\s*(0x[0-9A-Fa-f]+|\d+)\s*->\s*(0x[0-9A-Fa-f]+|\d+)')
FUNC_RE = re.compile(r'^(?:static\s+|inline\s+)*[\w:<>,\s\*&]+?\b(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{')


def to_int(s):
    return int(s, 0)


def gta4(nb):
    for lo, hi, delta in NB_ZONES:
        if lo <= nb <= hi:
            return nb + delta
    return None


def d3d_listing(gen):
    """function address -> its listing lines, for GTA IV's Direct3D library only."""
    funcs, cur = {}, None
    for n in sorted(os.listdir(gen)):
        if not (n.startswith('renut_recomp') and n.endswith('.cpp')):
            continue
        for line in open(os.path.join(gen, n), encoding='utf-8', errors='replace'):
            m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', line)
            if m:
                a = int(m.group(1), 16)
                cur = m.group(1).upper() if 0x829C7000 <= a < 0x829E4600 else None
                if cur:
                    funcs[cur] = []
            elif cur and line.lstrip().startswith('// '):
                funcs[cur].append(line.strip()[3:].strip())
    return funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ultimate', default='ultimate')
    ap.add_argument('--report')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    d3d = os.path.join(a.ultimate, 'app', 'src', 'renut_engine', 'gpu', 'd3d')
    files = a.files or sorted([os.path.join(d3d, 'hooks', f) for f in os.listdir(os.path.join(d3d, 'hooks'))
                               if f.endswith('.cpp')] + [os.path.join(d3d, 'd3d_native_state.cpp')])
    listing = d3d_listing(os.path.join(a.ultimate, 'app', 'generated'))
    by_disp = {}
    for fn, lines in listing.items():
        for l in lines:
            for m in re.finditer(r'[ ,](-?\d+)\(r\d+\)', l):
                by_disp.setdefault(int(m.group(1)), set()).add(fn)

    rows = []
    for path in files:
        func = '?'
        for n, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
            code = line.split('//')[0]
            fm = FUNC_RE.match(line)
            if fm and not line.startswith((' ', '\t')):
                func = fm.group(1)
            hits = [(m.group(1), None) for m in OFFSET_RE.finditer(code)]
            hits += [(m.group(2), m.group(1)) for m in CONST_RE.finditer(code)]
            if not hits:
                continue
            notes = NOTE_RE.findall(line)
            for raw, name in hits:
                val = to_int(raw)
                if val < 0x100 and name is None:
                    continue  # small fields of other structures (command buffers, 0x08, 0x6C ...)
                nb = None
                for nbs, kat in notes:
                    if to_int(kat) == val:
                        nb = to_int(nbs)
                if nb is None:
                    nb = val
                g = gta4(nb)
                ev = sorted(by_disp.get(g, ())) if g is not None else []
                rows.append((os.path.relpath(path, a.ultimate).replace('\\', '/'), n, func, name or '', val, nb, g, ev))

    out = ['file:line function [constant] katamari n&b -> gta4 (evidence: GTA IV D3D functions with that displacement)']
    changes = 0
    for f, n, func, name, val, nb, g, ev in rows:
        state = 'zone?' if g is None else ('same' if g == val else 'CHANGE')
        changes += state == 'CHANGE'
        evs = ('%d fn: %s' % (len(ev), ' '.join(ev[:6]) + (' ...' if len(ev) > 6 else ''))) if ev else 'no evidence'
        out.append(f'{f}:{n} {func} {name} kat {val} nb {nb} -> gta4 {g if g is not None else "?"} {state} ({evs})')
    out.append(f'\n{len(rows)} offsets; {changes} differ for GTA IV; '
               f'{sum(1 for r in rows if r[6] is None)} in a zone not mapped; '
               f'{sum(1 for r in rows if r[6] is not None and not r[7])} without evidence in GTA IV\'s D3D library')
    text = '\n'.join(out) + '\n'
    if a.report:
        open(a.report, 'w', encoding='utf-8', newline='\n').write(text)
        print(out[-1].strip(), '->', a.report)
    else:
        print(text)


if __name__ == '__main__':
    main()
