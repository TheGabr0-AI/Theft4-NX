#!/usr/bin/env python3
"""Theft4-NX (docs/switch-port/20, U6): GTA IV's shader containers in the Ultimate Base's shader dump folder, named as
the base names them (the "file finder" of a game whose shaders are not all in its executable; the base's
tools/renut_shader_dump.py and katamari_files.py for Katamari).

    python tools/ultimate/gta4_shader_dump.py --containers <folder> [--dumps <capture folder> ...]
                                               [--out ultimate/out/renut_shader_dump]

--containers: the 1,370 containers tools/deko3d/gta4_fxc_shaders.py and gta4_xbox_data.py extract from common.rpf's
shaders and default.xex (docs/switch-port, run 61), named <HASH>.<vs|ps>.bin. --dumps: folders of containers a PC or
console run captured (renut_shader_capture_dir: <vs|ps>_<hash>.bin), shaders the game would make at run time.
Each is written as <vs|ps>_<xxh3_64, lowercase>.bin: the base's name, which its translator turns into the dumped HLSL's
name (tools/renut_early_alpha.py reads ps_*.hlsl: with GTA IV's own names it found none, silently) and which its
build's capture step (app/cmake/collect_captured_shaders.cmake) compares against (with other names it copied 1,250
duplicates into the containers' folder, U4b). Checked: the container's header (magic 0x102A11xx, low bit 1 a vertex
shader, the name's stage the same), its size (header + physical block = the file), the name's hash = XXH3 of the file.
A container failing a check is not written; two inputs with one hash must be the same bytes. Files already in --out with
the same bytes are left alone; nothing is deleted. Game data: --out is a build folder, never committed.
"""
import argparse
import os
import re
import struct
import sys

import xxhash

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'ultimate', 'out', 'renut_shader_dump'))
OURS = re.compile(r'([0-9A-Fa-f]{16})\.(vs|ps)\.bin')
BASE = re.compile(r'(vs|ps)_([0-9a-f]{16})\.bin')


def is_link(path):
    """A junction or symbolic link (the folder used to be a junction to the containers: build_pc.bat before U6)."""
    if os.path.islink(path):
        return True
    try:
        return bool(os.lstat(path).st_file_attributes & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT
    except (OSError, AttributeError):
        return False


def check(blob, kind, name_hash):
    if len(blob) < 0x24 or blob[:3] != b'\x10\x2a\x11':
        return 'not a shader container (magic)'
    virt, phys = struct.unpack_from('>II', blob, 4)
    if virt + phys != len(blob):
        return f'size {len(blob)}, header says {virt} + {phys}'
    if ('vs' if blob[3] & 1 else 'ps') != kind:
        return f'the header says {"vs" if blob[3] & 1 else "ps"}, the name {kind}'
    h = xxhash.xxh3_64_hexdigest(blob)
    if h != name_hash.lower():
        return f'XXH3 {h}, the name {name_hash}'
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--containers', required=True)
    ap.add_argument('--dumps', nargs='*', default=[])
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    if os.path.exists(a.out) and is_link(a.out):
        sys.exit(f'gta4_shader_dump: {a.out} is a link (the junction of earlier builds): remove the link alone '
                 f'(cmd /c rmdir "{a.out}", never a recursive delete: it would empty the containers folder) and run again')
    os.makedirs(a.out, exist_ok=True)
    chosen = {}  # out name -> (bytes, source)
    rejected = dup_same = 0
    counts = {'vs': 0, 'ps': 0}
    sources = [(a.containers, OURS, lambda m: (m.group(2), m.group(1)))]
    sources += [(d, BASE, lambda m: (m.group(1), m.group(2))) for d in a.dumps]
    for folder, pattern, parts in sources:
        if not os.path.isdir(folder):
            sys.exit(f'gta4_shader_dump: no folder {folder}')
        for n in sorted(os.listdir(folder)):
            m = pattern.fullmatch(n)
            if not m:
                continue
            kind, h = parts(m)
            blob = open(os.path.join(folder, n), 'rb').read()
            why = check(blob, kind, h)
            if why:
                rejected += 1
                print(f'  rejected {os.path.join(folder, n)}: {why}')
                continue
            out_name = f'{kind}_{h.lower()}.bin'
            if out_name in chosen:
                if chosen[out_name][0] != blob:
                    sys.exit(f'gta4_shader_dump: {n} and {chosen[out_name][1]} have one name and different bytes')
                dup_same += 1
                continue
            chosen[out_name] = (blob, n)
            counts[kind] += 1
    written = same = 0
    for out_name, (blob, _) in sorted(chosen.items()):
        path = os.path.join(a.out, out_name)
        if os.path.isfile(path) and open(path, 'rb').read() == blob:
            same += 1
            continue
        open(path + '.tmp', 'wb').write(blob)
        os.replace(path + '.tmp', path)
        written += 1
    others = sorted(n for n in os.listdir(a.out) if n.endswith('.bin') and n not in chosen)
    print(f'gta4_shader_dump: {counts["vs"]} vertex and {counts["ps"]} pixel shaders ({dup_same} duplicates of the same '
          f'bytes, {rejected} rejected): {written} written, {same} already there; {len(others)} other containers in '
          f'{a.out}' + (f' (e.g. {others[0]})' if others else ''))
    return 1 if rejected else 0


if __name__ == '__main__':
    sys.exit(main())
