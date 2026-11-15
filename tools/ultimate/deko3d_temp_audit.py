#!/usr/bin/env python3
"""Theft4-NX (2026-11-15): which game shaders the mutable-temporary fix of ultimate/tools/deko3d_glsl.py changes.

    python tools/ultimate/deko3d_temp_audit.py [<folder of *.vk.glsl>] [--show]

Default folder: ultimate/out/deko3d_shaders/vk (SPIRV-Cross's output, kept by ultimate/tools/deko3d_shaders.py; game
data, never committed). Each text is converted twice: as before the fix (a `uint _N = expr;` temporary stands for its
declared expression wherever it is read) and as now (not when it is written again: immutable_uint_temps). A shader
whose bound textures (the banjo-dk line's texb) differ had a fetch bound to the texture of the word a temporary was
DECLARED with, while the shader reads another word there on some path: on the console that fetch sampled the wrong
texture. Prints each such shader with the bindings that go and the temporaries behind them (--show: the lines that
write them). No such shader: the packages made before the fix are the same as now, and the fix changes nothing in the
picture. Exit status: 0 none, 1 some (the shader package and the DKSH cache have to be made again), 2 no input.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ULT = os.path.normpath(os.path.join(HERE, '..', '..', 'ultimate'))
sys.path.insert(0, os.path.join(ULT, 'tools'))
import deko3d_glsl  # noqa: E402

_LEGACY_RE = re.compile(r'^\s*uint (_\d+) = ([^;]+);', re.M)


def legacy_temps(src):
    """The temporaries as the converter took them before the fix: every declaration with an initialiser."""
    return {m.group(1): m.group(2).strip() for m in _LEGACY_RE.finditer(src)}


def texb(text):
    m = re.search(r'^// banjo-dk [^\n]* texb=(\S+)', text, re.M)
    return m.group(1).split(',') if m else []


def audit(src, stage):
    """(bindings before, bindings now, mutable temporaries {name: declared expression}), or None if not convertible."""
    fixed = deko3d_glsl.immutable_uint_temps
    try:
        now, _ = deko3d_glsl.convert(src, stage, deko3d_glsl.DEFAULT_UBO_MAP, 'handle')
        deko3d_glsl.immutable_uint_temps = legacy_temps
        before, _ = deko3d_glsl.convert(src, stage, deko3d_glsl.DEFAULT_UBO_MAP, 'handle')
    except deko3d_glsl.Unsupported:
        return None
    finally:
        deko3d_glsl.immutable_uint_temps = fixed
    kept = fixed(src)
    mutable = {n: e for n, e in legacy_temps(src).items() if n not in kept}
    return texb(before), texb(now), mutable


def words(entries):
    """texb entries without their binding numbers (those shift when one goes): texture:sampler[:variant]."""
    return sorted(e.split(':', 1)[1] for e in entries)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder', nargs='?', default=os.path.join(ULT, 'out', 'deko3d_shaders', 'vk'))
    ap.add_argument('--show', action='store_true', help='the lines that write each temporary')
    a = ap.parse_args()
    if not os.path.isdir(a.folder):
        print(f'deko3d_temp_audit: {a.folder} missing: run ultimate/tools/switch_shaders.ps1 first', file=sys.stderr)
        return 2
    names = sorted(f for f in os.listdir(a.folder) if f.endswith('.glsl'))
    if not names:
        print(f'deko3d_temp_audit: no *.glsl under {a.folder}', file=sys.stderr)
        return 2
    changed = skipped = with_mutable = 0
    for name in names:
        src = open(os.path.join(a.folder, name), encoding='utf-8').read().replace('\r\n', '\n')
        r = audit(src, 'frag' if name.startswith('ps_') else 'vert')
        if r is None:
            skipped += 1
            continue
        before, now, mutable = r
        with_mutable += bool(mutable)
        if words(before) == words(now):
            continue
        changed += 1
        gone = [w for w in words(before) if w not in words(now)]
        print(f'{name}: {len(before)} bound textures before, {len(now)} now; no longer bound (texture word:sampler '
              f'word[:variant], byte offsets in SharedConstants): {", ".join(gone)}')
        for n, e in sorted(mutable.items()):
            print(f'    {n} declared as {e}, written again')
            if a.show:
                for line in src.split('\n'):
                    if re.search(r'(?<![\w.])' + n + r'\s*(?:(?:<<|>>|[-+*/%&|^])?=(?!=)|\+\+|--)', line) \
                            and not re.match(r'\s*uint ' + n + r' = ', line):
                        print(f'        {line.strip()}')
    print(f'{len(names)} shaders: {changed} with a fetch that was bound to a fixed texture through a temporary written '
          f'again ({with_mutable} have such a temporary at all), {skipped} not convertible')
    return 1 if changed else 0


if __name__ == '__main__':
    sys.exit(main())
