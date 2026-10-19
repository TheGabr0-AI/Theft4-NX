#!/usr/bin/env python3
"""Native renderer, step N1: joins the PM4 reader's native-observe.csv (reader
against the emulated renderer, written by src/graphics/native/pm4_observer.cpp)
with native-hooks.csv (the game's own Direct3D calls, tools/pc-eu-base/
native_hooks.cpp), frame by frame, into one CSV, and prints one summary line
plus the first frames that differ.

Usage: native_observe_report.py <native-observe.csv> [<native-hooks.csv>] [--out joined.csv]

Frames are numbered from the first swap each source saw; the two files start
at the same Swap only if both were on from the start (they are, with the cvars
on the command line).
"""
import csv
import sys


def load(path):
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = None
    if '--out' in sys.argv:
        out = sys.argv[sys.argv.index('--out') + 1]
        args = [a for a in args if a != out]
    observe = load(args[0])
    hooks = load(args[1]) if len(args) > 1 else []
    rows = []
    for i, o in enumerate(observe):
        row = dict(o)
        if i < len(hooks):
            for k, v in hooks[i].items():
                if k != 'frame':
                    row[k] = v
        rows.append(row)
    if out and rows:
        with open(out, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) +
                               [k for k in (rows[-1].keys()) if k not in rows[0]])
            w.writeheader()
            w.writerows(rows)

    def total(key):
        return sum(int(r[key]) for r in rows if key in r and r[key] != '')

    n = len(rows)
    equal = sum(int(r['equal']) for r in rows)
    line = (f'N1 summary: {n} frames; draws reader {total("reader_draws")} / emulated '
            f'{total("emulated_draws")}; resolves {total("reader_resolves")} / '
            f'{total("emulated_resolves")}; render target changes {total("reader_rt_changes")} / '
            f'{total("emulated_rt_changes")}; predicated {total("reader_predicated")}; frames equal '
            f'(draws and resolves) {equal} of {n}')
    if hooks:
        line += (f'; game calls over {min(n, len(hooks))} frames: draws {total("hook_draws")}, '
                 f'resolves {total("hook_resolves")}, SetRenderTarget '
                 f'{total("hook_set_render_target")}, SetDepthStencil '
                 f'{total("hook_set_depth_stencil")}, shaders {total("hook_pixel_shaders")} pixel / '
                 f'{total("hook_vertex_shaders")} vertex')
    print(line)
    shown = 0
    for r in rows:
        if r['equal'] == '0' and shown < 10:
            shown += 1
            print('  differs: ' + ', '.join(f'{k}={v}' for k, v in r.items()))


if __name__ == '__main__':
    main()
