#!/usr/bin/env python3
"""Every GTA IV shader container from the game's own files, offline (docs/switch-port/16-deko3d-devlog.md, run 61).

    python tools/deko3d/gta4_fxc_shaders.py extract --game <game folder> --key <aes_key.bin> --out <dir>
    python tools/deko3d/gta4_fxc_shaders.py coverage --manifest <dir>/manifest.tsv --package <gta4_deko3d_runtime.bin>
        [--runtime-dump <dir of <HASH>.<vs|ps>.bin>] [--missing-out <dir>]

Until run 60 the package was made from the 1,274 containers one PC session of 480 s created (pc_shader_dump_dir):
only what that session drew. The game keeps its shaders in two places, both read here:

  - common.rpf: shaders/fxl_final/*.fxc (RAGE effects, deflate-compressed entries of an RPF2 archive whose table of
    contents is AES-256 encrypted; the key is the user's own file, LibertyRecompLib/aes_key.bin, never in the repo);
  - default.xex: RAGE's own effects compiled into the executable (im, postfx, particles, bink, sky), in the image
    after the XEX2 decryption (AES-128 CBC, basic compression).

A container is found by its magic (0x102A1100 pixel, 0x102A1101 vertex) at any byte offset (RAGE does not align
them), then hashed as the runtime does: XXH3-64 of virtual + physical size bytes (tools/pc-eu-base/native_hooks.cpp
LogShaderHash, gta4_native RegisterNativeShader). Every candidate is listed with its verdict under XenosRecomp's own
rule (XenosRecomp/main.cpp: magic & 0xFFFFFF00 == 0x102A1100, sizes inside the file, field1C == 0, field20 == 0), so a
container the translator would skip silently is named here instead.

extract writes <out>/containers/<HASH>.<vs|ps>.bin (the input XenosRecomp takes) and <out>/manifest.tsv:
  hash, stage, archive, file, offset, size, fieldC, constant table, definition table, shader offset, field1C,
  field20, verdict, the source path the compiler recorded (.updb).
coverage counts it against the package (and a runtime dump) and can copy the containers the package lacks into
--missing-out, the input of the next package build.

Everything written is game data: keep --out outside the repository.
"""
import argparse
import collections
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gta4_xbox_data as xd  # noqa: E402

MANIFEST_FIELDS = ('hash', 'stage', 'archive', 'file', 'offset', 'size', 'field_c', 'constant_table',
                   'definition_table', 'shader_offset', 'field_1c', 'field_20', 'verdict', 'source')


def xenosrecomp_verdict(blob, reason):
    """XenosRecomp/main.cpp's acceptance rule for one candidate (None = accepted)."""
    if reason:
        return reason
    words = struct.unpack_from('>9I', blob, 0)
    if (words[0] & 0xFFFFFF00) != 0x102A1100:
        return f'magic {words[0]:08X}'
    if words[7] != 0 or words[8] != 0:
        return f'field1C {words[7]:08X} field20 {words[8]:08X} (XenosRecomp skips it)'
    return None


def scan(data, archive, file, rows):
    for offset, stage, blob, reason in xd.find_containers(data):
        verdict = xenosrecomp_verdict(blob, reason)
        row = {'hash': '', 'stage': stage, 'archive': archive, 'file': file, 'offset': offset,
               'size': len(blob), 'verdict': verdict or 'ok', 'source': ''}
        words = struct.unpack_from('>9I', blob, 0) if len(blob) >= 36 else (0,) * 9
        for k, w in zip(('field_c', 'constant_table', 'definition_table', 'shader_offset', 'field_1c', 'field_20'),
                        words[3:9]):
            row[k] = w
        if blob:
            row['hash'] = f'{xd.xxh3_64(blob):016X}'
            row['source'] = xd.container_source(blob)
            row['blob'] = blob
        rows.append(row)


def extract(a):
    key = open(a.key, 'rb').read()
    if len(key) != 32:
        sys.exit(f'{a.key}: 32 bytes expected')
    rows = []
    archives = collections.Counter()
    common = os.path.join(a.game, 'common.rpf')
    rpf = xd.RpfArchive(open(common, 'rb').read(), key, 'common.rpf')
    for e in rpf.files():
        if e.name.lower().endswith('.fxc'):
            scan(rpf.read(e), 'common.rpf', e.path, rows)
            archives['common.rpf .fxc files'] += 1
    xex = os.path.join(a.game, 'default.xex')
    scan(xd.xex2_image(open(xex, 'rb').read()), 'default.xex', 'image', rows)
    archives['default.xex images'] += 1

    out_dir = os.path.join(a.out, 'containers')
    os.makedirs(out_dir, exist_ok=True)
    written = set()
    with open(os.path.join(a.out, 'manifest.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\t'.join(MANIFEST_FIELDS) + '\n')
        for r in rows:
            f.write('\t'.join(str(r.get(k, '')) for k in MANIFEST_FIELDS) + '\n')
            if r['verdict'] == 'ok' and r['hash'] not in written:
                written.add(r['hash'])
                with open(os.path.join(out_dir, f"{r['hash']}.{r['stage']}.bin"), 'wb') as c:
                    c.write(r['blob'])
    report(rows, archives)


def report(rows, archives):
    ok = [r for r in rows if r['verdict'] == 'ok']
    unique = {}
    for r in ok:
        unique.setdefault(r['hash'], r)
    print('sources:', ', '.join(f'{k} {v}' for k, v in archives.items()))
    print(f'candidates {len(rows)}, valid {len(ok)}, rejected {len(rows) - len(ok)}')
    for r in rows:
        if r['verdict'] != 'ok':
            print(f"  rejected: {r['archive']}/{r['file']} +{r['offset']}: {r['verdict']}")
    by = collections.Counter((r['archive'], r['stage']) for r in unique.values())
    print(f'unique containers {len(unique)} (duplicates {len(ok) - len(unique)}): ' +
          ', '.join(f'{arc} {st} {n}' for (arc, st), n in sorted(by.items())))


def read_manifest(path):
    with open(path, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        return [dict(zip(head, line.rstrip('\n').split('\t'))) for line in f if line.strip()]


def package_hashes(path):
    """{(hash, late)} of a gta4_deko3d_runtime.bin (gta4_deko3d_runtime_check.py's layout)."""
    with open(path, 'rb') as f:
        head = f.read(32)
        if head[:4] != b'GDKR':
            raise ValueError(f'{path}: not a runtime package')
        _v, count, _blobs, tag_len, record = struct.unpack_from('<5I', head, 4)
        table_at = 32 + tag_len + ((-tag_len) % 8)
        f.seek(table_at)
        table = f.read(count * record)
    return {(struct.unpack_from('<Q', table, i * record)[0], table[i * record + 9]) for i in range(count)}


def coverage(a):
    rows = [r for r in read_manifest(a.manifest) if r['verdict'] == 'ok']
    offline = {}
    for r in rows:
        offline.setdefault(int(r['hash'], 16), r)
    pkg = package_hashes(a.package)
    in_pkg = {h for h, late in pkg if not late}
    late = {h for h, lt in pkg if lt}
    dumped = {}
    if a.runtime_dump:
        for n in os.listdir(a.runtime_dump):
            p = n.split('.')
            if len(p) == 3 and p[2] == 'bin':
                dumped[int(p[0], 16)] = p[1]
    stage = collections.Counter(r['stage'] for r in offline.values())
    print(f'offline unique containers {len(offline)}: vertex {stage["vs"]}, pixel {stage["ps"]}')
    print(f'package: {len(in_pkg)} shaders (+{len(late)} late alpha-test sources)')
    missing = sorted(h for h in offline if h not in in_pkg)
    print(f'offline containers in the package {len(offline) - len(missing)}, missing {len(missing)}')
    by = collections.Counter((offline[h]['archive'], offline[h]['stage']) for h in missing)
    for (arc, st), n in sorted(by.items()):
        print(f'  missing from {arc}: {st} {n}')
    extra = sorted(h for h in in_pkg if h not in offline)
    print(f'package shaders not found offline {len(extra)}' + (': ' + ' '.join(f'{h:016X}' for h in extra[:20])
                                                                if extra else ''))
    if dumped:
        print(f'runtime dump {len(dumped)}: found offline {sum(1 for h in dumped if h in offline)}, '
              f'in the package {sum(1 for h in dumped if h in in_pkg)}')
    for h in missing:
        r = offline[h]
        print(f"  {h:016X} {r['stage']} {r['archive']}/{r['file']} {r['source'].split(chr(92))[-1]}")
    if a.missing_out:
        src_dir = os.path.join(os.path.dirname(a.manifest), 'containers')
        os.makedirs(a.missing_out, exist_ok=True)
        for h in missing:
            n = f"{h:016X}.{offline[h]['stage']}.bin"
            shutil.copyfile(os.path.join(src_dir, n), os.path.join(a.missing_out, n))
        print(f'{len(missing)} containers copied to {a.missing_out}')
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract')
    e.add_argument('--game', required=True)
    e.add_argument('--key', required=True)
    e.add_argument('--out', required=True)
    c = sub.add_parser('coverage')
    c.add_argument('--manifest', required=True)
    c.add_argument('--package', required=True)
    c.add_argument('--runtime-dump', default='')
    c.add_argument('--missing-out', default='')
    a = ap.parse_args()
    if a.cmd == 'extract':
        extract(a)
        return 0
    return coverage(a)


if __name__ == '__main__':
    sys.exit(main())
