#!/usr/bin/env python3
"""Theft4-NX (docs/switch-port/20, U6): GTA IV's offline DKSH cache for the Ultimate Base's deko3d renderer, and the
checks of what it is made from.

    python tools/ultimate/gta4_dksh_cache.py --list <pipeline_list.bin> [--list ...] [--runtime-log <game.log> ...]
        [--merge <deko3d_shaders.bin>] [--out <deko3d_shaders.bin>] [--jobs N] [--check-only]
        [--console-cache <the SD's deko3d_shaders.bin>] [--manifest <json>]

Inputs (game data, made by tools/ultimate/build_deko3d_tools.bat and the base's tools/switch_shaders.ps1; never
committed): ultimate/out/deko3d_shaders/glsl_opt (the GLSL package's texts), ultimate/app/out/renut_switch
(deko3d_glsl.bin and shader_cache_switch.cpp, the NRO's), ultimate/out/deko3d_tools (uam.exe, uam_dksh_test.exe).

1. The package against the NRO's shader table: one GLSL text per table entry and none without one; deko3d_glsl.bin's
   table (sorted, unique) names the same shaders with each text's size and stage, and a sample of its entries unpacks
   (zstd with the package's dictionary) to the text byte for byte.
2. uam: the PC build of the base's uam as a library (uamlib, what the NRO links) against the uam tool (what the cache
   compiles with) on every 10th text and the 10 largest: the same DKSH bytes; a shader that does not compile gives an
   error in the log and the compiler goes on (uam_dksh_test).
3. The cache: the base's tools/deko3d_variant_cache.py --generic --plain-guess <table> with the lists (and the run-time
   logs, the merged cache), --export-variants beside the output (every variant's key checked against its text).
4. The file: "BDKC", version 1, banjo_renut_deko3d.cpp's compiler tag; whole records sorted by key, unique, each a DKSH;
   every exported variant's key in it; the lists' shaders that the package lacks, by name.
5. --console-cache: a cache the console wrote (it appends each variant it compiles, its NRO's uam built by devkitA64
   GCC): its records whose key this cache has, byte for byte; the others whose text is among the exported variants,
   compiled by the PC tool and compared; the rest counted (variants the lists did not give: make the cache again with
   that run's list and --runtime-log). This is the check that the PC's uam (the MSVC ABI, not the base's MinGW) gives the
   console's bytes; first done on run 72's card (docs/switch-port/20, U6). With --runtime-log, a record that differs
   where a draw waited for the variant ("BLOCKING (a draw waits)") is the console's one-pass compile
   (banjo_deko3d_fast_first_compile), counted apart and not a failure (the first U8 run: 8 of 88, 80 the same bytes).
--console-merge (U8): the card's records are merged too, less the variants a draw waited for, which the PC compiles in
   full: the variants a run needed but no list or log rule gives (docs/switch-port/20, U8).
Exit status 0 only if every check passes and no variant failed to compile.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ULT = os.path.normpath(os.path.join(HERE, '..', '..', 'ultimate'))
GLSL = os.path.join(ULT, 'out', 'deko3d_shaders', 'glsl_opt')
PKG = os.path.join(ULT, 'app', 'out', 'renut_switch')
TOOLS = os.path.join(ULT, 'out', 'deko3d_tools', 'uam-build')
OUT = os.path.join(ULT, 'out', 'deko3d_shaders', 'deko3d_shaders.bin')
sys.path.insert(0, os.path.join(ULT, 'tools'))
from deko3d_shader_package import ZSTD  # noqa: E402  (the zstd the package is made with)
TAG = b'uam110-sw8-bnx4\0'  # banjo_renut_deko3d.cpp kCacheTag
NAME_RE = re.compile(r'(vs|ps)_([0-9A-F]{16})\.glsl')

failures = []


def fail(msg):
    failures.append(msg)
    print('  FAIL ' + msg)


def table_hashes(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    block = re.search(r'ShaderCacheEntry g_shaderCacheEntries\[\]\s*=\s*\{\n(.*?)\n\};', text, re.S)
    if not block:
        sys.exit(f'gta4_dksh_cache: no g_shaderCacheEntries in {path}')
    return [int(m.group(1), 16) for m in re.finditer(r'^\t\{ 0x([0-9A-Fa-f]+),', block.group(1), re.M)]


def check_package(table_path):
    print('== 1. package against the shader table')
    names = sorted(n for n in os.listdir(GLSL) if NAME_RE.fullmatch(n))
    glsl = {int(n[3:19], 16): n for n in names}
    table = table_hashes(table_path)
    if len(set(table)) != len(table):
        fail(f'the shader table has {len(table) - len(set(table))} duplicated hashes')
    for h in sorted(set(table) - set(glsl))[:10]:
        fail(f'table shader {h:016X} has no GLSL text')
    for h in sorted(set(glsl) - set(table))[:10]:
        fail(f'GLSL text {glsl[h]} has no table entry')
    data = open(os.path.join(PKG, 'deko3d_glsl.bin'), 'rb').read()
    magic, version, count, dict_size, table_off, data_off = struct.unpack_from('<4sIIIII', data, 0)
    if magic != b'BDKG' or version != 1:
        fail(f'deko3d_glsl.bin: {magic} version {version}')
        return names
    entries = [struct.unpack_from('<QIIII', data, table_off + 56 * i) for i in range(count)]
    hashes = [e[0] for e in entries]
    if hashes != sorted(set(hashes)):
        fail('deko3d_glsl.bin: its table is not sorted and unique')
    if set(hashes) != set(glsl):
        fail(f'deko3d_glsl.bin: {len(set(hashes) ^ set(glsl))} shaders differ from glsl_opt')
    for h, off, csize, size, stage in entries:
        n = glsl.get(h)
        if n is None:
            continue
        if size != os.path.getsize(os.path.join(GLSL, n)):
            fail(f'deko3d_glsl.bin: {n} size {size}, the text {os.path.getsize(os.path.join(GLSL, n))}')
        if stage != (4 if n.startswith('ps_') else 0):
            fail(f'deko3d_glsl.bin: {n} stage {stage}')
        if data_off + off + csize > len(data):
            fail(f'deko3d_glsl.bin: {n} runs past the end')
    # a sample unpacked with the package's own dictionary
    sample = entries[::max(1, len(entries) // 24)]
    with tempfile.TemporaryDirectory() as tmp:
        dict_path = os.path.join(tmp, 'dict')
        open(dict_path, 'wb').write(data[32:32 + dict_size])
        for h, off, csize, size, stage in sample:
            frame = os.path.join(tmp, 'f.zst')
            open(frame, 'wb').write(data[data_off + off:data_off + off + csize])
            r = subprocess.run([ZSTD, '-d', '-q', '-c', '-D', dict_path, frame], capture_output=True)
            if r.returncode != 0 or r.stdout != open(os.path.join(GLSL, glsl[h]), 'rb').read():
                fail(f'deko3d_glsl.bin: {glsl[h]} does not unpack to its text')
    vs = sum(1 for n in names if n.startswith('vs_'))
    print(f'  {len(names)} GLSL texts ({vs} vertex, {len(names) - vs} pixel), table {len(table)}, '
          f'deko3d_glsl.bin {count} entries ({len(data) >> 10} KB), {len(sample)} unpacked and compared')
    return names


def check_uam(names):
    print('== 2. uam: library against tool')
    uam, test = os.path.join(TOOLS, 'uam.exe'), os.path.join(TOOLS, 'uam_dksh_test.exe')
    for exe in (uam, test):
        if not os.path.isfile(exe):
            fail(f'{exe} missing: tools/ultimate/build_deko3d_tools.bat')
            return
    chosen = names[::10]
    chosen += [n for n in sorted(names, key=lambda n: -os.path.getsize(os.path.join(GLSL, n)))[:10] if n not in chosen]
    with tempfile.TemporaryDirectory() as tmp:
        src, ref = os.path.join(tmp, 'glsl'), os.path.join(tmp, 'ref')
        os.makedirs(src)
        os.makedirs(ref)
        for n in chosen:
            stem = n[3:19] + ('_vs' if n.startswith('vs_') else '_ps')  # uam_dksh_test: "_vs" = vertex
            shutil.copyfile(os.path.join(GLSL, n), os.path.join(src, stem + '.glsl'))
            r = subprocess.run([uam, '-s', 'vert' if n.startswith('vs_') else 'frag', '-o',
                                os.path.join(ref, stem + '.dksh'), os.path.join(src, stem + '.glsl')],
                               capture_output=True, text=True)
            if r.returncode != 0:
                fail(f'uam tool: {n} did not compile: {(r.stdout + r.stderr).strip()[:200]}')
        r = subprocess.run([test, src, ref], capture_output=True, text=True)
        lines = [l for l in r.stdout.split('\n') if l.strip()]
        for l in lines:
            if l.startswith(('DIFF', 'FAIL', 'NOREF')):
                print('  ' + l)
        # the error path's line carries uam's own log (several lines) before "compile after the error"
        err = 'ok' if re.search(r'error path: ok', r.stdout) else 'WRONG'
        after = 'ok' if re.search(r'compile after the error: ok', r.stdout) else 'WRONG'
        print(f'  a shader that does not compile: error in the log {err}, the next compile {after}')
        print('  ' + (lines[-1] if lines else '').strip())
        if r.returncode != 0:
            fail('uam_dksh_test')


def read_cache(path, records=None):
    """The keys of a cache file (and into records, key -> DKSH, when given)."""
    data = open(path, 'rb').read()
    if data[:4] != b'BDKC' or struct.unpack_from('<I', data, 4)[0] != 1 or data[8:24] != TAG:
        fail(f'{path}: not a version 1 cache with the tag {TAG[:-1].decode()}')
        return None, data
    keys, at = [], 24
    while at < len(data):
        if at + 16 > len(data):
            fail(f'{path}: {len(data) - at} bytes after the last record')
            break
        key, size = struct.unpack_from('<QI', data, at)
        if size < 64 or size > (16 << 20) or at + 12 + size > len(data) or data[at + 12:at + 16] != b'DKSH':
            fail(f'{path}: record {len(keys)} at {at} is not whole')
            break
        keys.append(key)
        if records is not None:
            records[key] = data[at + 12:at + 12 + size]
        at += 12 + size
    return keys, data


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


RT_RE = re.compile(r'uam: (background|blocking \(prebuild waits\)|BLOCKING \(a draw waits\)) run-time variant '
                   r'\(stage (\d+), \d+ bytes, base (\d+) bytes ([0-9a-f]{16})\): (.*)$')


def waited_keys(logs):
    """U8: the keys of the variants a draw waited for in these console logs ("BLOCKING (a draw waits)"). plume compiles
    those in one back-end pass (banjo_deko3d_fast_first_compile, "about a quarter of a full compile") and appends that
    to the card's cache as it is, so their bytes may differ from the PC tool's full compile."""
    by_size, fnv = {}, {}
    for n in os.listdir(GLSL):
        if NAME_RE.fullmatch(n):
            by_size.setdefault(os.path.getsize(os.path.join(GLSL, n)), []).append(n)
    keys = set()
    for path in logs:
        for line in open(path, encoding='utf-8', errors='replace'):
            m = RT_RE.search(line)
            if not m or not m.group(1).startswith('BLOCKING'):
                continue
            size, want = int(m.group(3)), int(m.group(4), 16)
            name = None
            for n in by_size.get(size, []):
                if n not in fnv:
                    fnv[n] = fnv1a64(open(os.path.join(GLSL, n), 'rb').read())
                if fnv[n] == want:
                    name = n
                    break
            if name is None:
                continue
            defines, last = b'', -1  # as deko3d_variant_cache.py reads the line: the first ascending run
            for item in [x.strip() for x in m.group(5).split(';') if x.strip()]:
                parts = item.split()
                if len(parts) != 2 or not parts[0].isdigit() or int(parts[0]) <= last:
                    break
                last = int(parts[0])
                defines += f'#define SPIRV_CROSS_CONSTANT_ID_{parts[0]} {parts[1]}\n'.encode()
            t = open(os.path.join(GLSL, name), 'rb').read()
            nl = t.find(b'\n')
            keys.add(fnv1a64(t[:nl + 1] + defines + t[nl + 1:] if defines else t))
    return keys


def write_console_merge(console_path, base_path, logs, out_path):
    """U8 (--console-merge): the records of the card's cache, and of --merge's, as one merge input for
    deko3d_variant_cache.py, less the variants a draw waited for (waited_keys: possibly one-pass compiles), which the PC
    compiles again in full. Keys sorted, each once; a key in both takes the card's record (the NRO's own compiler)."""
    records = {}
    if base_path and read_cache(base_path, records)[0] is None:
        return None
    console = {}
    if read_cache(console_path, console)[0] is None:
        return None
    records.update(console)
    waited = waited_keys(logs)
    dropped = [k for k in waited if k in records]
    for k in dropped:
        del records[k]
    with open(out_path, 'wb') as f:
        f.write(b'BDKC' + struct.pack('<I', 1) + TAG)
        for k in sorted(records):
            f.write(struct.pack('<QI', k, len(records[k])) + records[k])
    print(f'  merge input: {len(records)} records ({len(console)} from the card), {len(dropped)} variants a draw waited '
          f'for left to the PC tool')
    return out_path


def check_console(path, ours_path, variants, logs=()):
    print('== 5. a console cache against this one')
    console, ours = {}, {}
    if read_cache(path, console)[0] is None or read_cache(ours_path, ours)[0] is None:
        return
    common = [k for k in console if k in ours]
    differ = [k for k in common if console[k] != ours[k]]
    # U8: a variant a draw waited for is a one-pass compile on the console (waited_keys): its bytes may differ
    one_pass = waited_keys(logs) if differ and logs else set()
    known = []
    if one_pass:
        known = [k for k in differ if k in one_pass]
        differ = [k for k in differ if k not in one_pass]
        print(f'  {len(known)} records differ where a draw waited for the variant (the console\'s one-pass compile, '
              f'banjo_deko3d_fast_first_compile; this cache has the full compile)')
    for k in differ[:10]:
        fail(f'record {k:016X}: the console\'s DKSH ({len(console[k])} bytes) is not this cache\'s ({len(ours[k])})')
    rest = {k for k in console if k not in ours}
    same = other = 0
    if rest and os.path.isfile(variants):
        # the exported variants' texts (deko3d_variant_cache.py variant_text: the defines after the first line)
        texts = {}
        for line in open(variants, encoding='utf-8'):
            parts = line.split()
            if not parts:
                continue
            pairs = [p.split('=') for p in parts[1].split(',')] if len(parts) > 1 else []
            defines = ''.join(f'#define SPIRV_CROSS_CONSTANT_ID_{i} {v}u\n' for i, v in pairs).encode()
            if parts[0] not in texts:
                texts[parts[0]] = open(os.path.join(GLSL, parts[0] + '.glsl'), 'rb').read()
            t = texts[parts[0]]
            nl = t.find(b'\n')
            text = t[:nl + 1] + defines + t[nl + 1:] if defines else t
            k = fnv1a64(text)
            if k not in rest:
                continue
            rest.discard(k)
            with tempfile.TemporaryDirectory() as tmp:
                src, out = os.path.join(tmp, 'v.glsl'), os.path.join(tmp, 'v.dksh')
                open(src, 'wb').write(text)
                subprocess.run([os.path.join(TOOLS, 'uam.exe'), '-s', 'vert' if parts[0].startswith('vs_') else 'frag',
                                '-o', out, src], capture_output=True)
                if os.path.isfile(out) and open(out, 'rb').read() == console[k]:
                    same += 1
                else:
                    other += 1
                    fail(f'record {k:016X} ({parts[0]}): the PC tool does not give the console\'s DKSH')
    print(f'  {len(console)} console records: {len(common) - len(differ) - len(known)} the same as this cache\'s, '
          f'{len(known)} one-pass, {len(differ)} different; of the others {same} compiled the same by the PC tool, '
          f"{other} not, {len(rest)} not among this cache's variants")


def list_hashes(path):
    """The two shader hashes of every record of a pipeline list (render/pipeline.cpp, any version)."""
    data = open(path, 'rb').read()
    if data[:4] != b'RNPL':
        fail(f'{path}: not a pipeline list')
        return set(), 0, 0
    version, key_size = struct.unpack_from('<II', data, 4)
    out, at, n = set(), 16, 0
    while at + 4 <= len(data):
        (size,) = struct.unpack_from('<I', data, at)
        at += 4
        if at + size > len(data) or size < key_size:
            break
        vs, ps = struct.unpack_from('<QQ', data, at)
        out.add(('vs', vs))
        if ps:
            out.add(('ps', ps))
        n += 1
        at += size
    return out, n, version


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def folder_digest(folder, pattern):
    """SHA-256 over the sorted names and SHA-256s of a folder's files matching pattern, and their count."""
    h, n = hashlib.sha256(), 0
    for name in sorted(f for f in os.listdir(folder) if re.fullmatch(pattern, f)):
        h.update(f'{name} {sha256(os.path.join(folder, name))}\n'.encode())
        n += 1
    return h.hexdigest(), n


def write_manifest(path, a, lists, cache):
    """What the package and the cache were made from and what they are (no machine paths: game data stays out)."""
    run = lambda cmd: subprocess.run(cmd, capture_output=True, text=True).stdout.strip()
    spvc = ''
    cache_txt = os.path.join(ULT, 'out', 'deko3d_tools', 'spirv-cross-build', 'CMakeCache.txt')
    if os.path.isfile(cache_txt):
        m = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', open(cache_txt, encoding='utf-8').read(), re.M)
        commit = os.path.join(m.group(1), 'COMMIT') if m else ''
        spvc = open(commit).read().strip() if commit and os.path.isfile(commit) else 'unknown'
    dump = os.path.join(ULT, 'out', 'renut_shader_dump')
    recomp = os.path.join(ULT, 'app', 'out', 'pc', 'xenos_recomp', 'build', 'XenosRecomp', 'XenosRecomp.exe')
    m = {
        'tool': 'tools/ultimate/gta4_dksh_cache.py',
        'repository_commit': run(['git', '-C', HERE, 'rev-parse', 'HEAD']),
        'base_commit': open(os.path.join(HERE, 'BASE_COMMIT')).read().strip(),
        'tools': {
            'spirv_cross_commit': spvc,
            'uam': run([os.path.join(TOOLS, 'uam.exe'), '-v']),
            'uam_sha256': sha256(os.path.join(TOOLS, 'uam.exe')),
            'cache_tag': TAG[:-1].decode(),
            'xenos_recomp_sha256': sha256(recomp) if os.path.isfile(recomp) else None,
        },
        'containers': dict(zip(('combined_sha256', 'count'), folder_digest(dump, r'(vs|ps)_[0-9a-f]{16}\.bin'))),
        'package': {
            'glsl_opt': dict(zip(('combined_sha256', 'count'), folder_digest(GLSL, NAME_RE.pattern))),
            'deko3d_glsl_bin_sha256': sha256(os.path.join(PKG, 'deko3d_glsl.bin')),
            'shader_cache_switch_cpp_sha256': sha256(os.path.join(PKG, 'shader_cache_switch.cpp')),
        },
        'lists': lists,
        'runtime_logs': [os.path.basename(p) for p in a.runtime_log],
        'merged_cache': os.path.basename(a.merge) if a.merge else None,
        'console_merge': os.path.basename(a.console_merge) if a.console_merge else None,
        'options': ['--generic', '--plain-guess shader_cache_switch.cpp'],
        'cache': cache,
    }
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    print(f'  manifest {path}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', default=None, help='write what the cache was made from (JSON)')
    ap.add_argument('--list', action='append', default=[])
    ap.add_argument('--runtime-log', action='append', default=[])
    ap.add_argument('--merge', default=None)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('--check-only', action='store_true', help='steps 1, 2 and 4 (and 5) on an existing --out')
    ap.add_argument('--console-cache', default=None, help="step 5: a deko3d_shaders.bin the console wrote")
    ap.add_argument('--console-merge', default=None,
                    help="U8: a deko3d_shaders.bin the console wrote, merged too (with --merge's records), less the "
                         "variants a draw waited for in --runtime-log (one-pass compiles there), compiled again here")
    a = ap.parse_args()
    table_path = os.path.join(PKG, 'shader_cache_switch.cpp')
    for p in (GLSL, os.path.join(PKG, 'deko3d_glsl.bin'), table_path):
        if not os.path.exists(p):
            sys.exit(f'gta4_dksh_cache: {p} missing: run ultimate/tools/switch_shaders.ps1 first')
    names = check_package(table_path)
    check_uam(names)
    variants = a.out + '.variants.txt'
    if not a.check_only:
        print('== 3. tools/deko3d_variant_cache.py')
        if not a.list and not a.runtime_log:
            sys.exit('gta4_dksh_cache: no --list or --runtime-log')
        cmd = [sys.executable, '-I', os.path.join(ULT, 'tools', 'deko3d_variant_cache.py'), '--generic',
               '--plain-guess', table_path, '--glsl', GLSL, '--out', a.out, '--jobs', str(a.jobs),
               '--export-variants', variants]
        for p in a.list:
            cmd += ['--list', p]
        for p in a.runtime_log:
            cmd += ['--runtime-log', p]
        tmp_merge = None
        if a.console_merge:
            tmp_merge = tempfile.TemporaryDirectory()
            merged = write_console_merge(a.console_merge, a.merge, a.runtime_log,
                                         os.path.join(tmp_merge.name, 'merge.bin'))
            if merged is None:
                sys.exit('gta4_dksh_cache: --console-merge: not a cache')
            cmd += ['--merge', merged]
        elif a.merge:
            cmd += ['--merge', a.merge]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if tmp_merge:
            tmp_merge.cleanup()
        sys.stdout.write(''.join('  ' + l + '\n' for l in (r.stdout + r.stderr).strip().split('\n')))
        if r.returncode != 0:
            fail('deko3d_variant_cache.py')
        m = re.search(r'\((\d+) failed\)', r.stdout)
        if not m or m.group(1) != '0':
            fail('variants that did not compile: ' + (m.group(1) if m else '?'))
    print('== 4. the cache file')
    cache = None
    if not os.path.isfile(a.out):
        fail(f'{a.out} missing')
    else:
        keys, data = read_cache(a.out)
        if keys is not None:
            if keys != sorted(set(keys)):
                fail('keys not sorted and unique')
            want = None
            if os.path.isfile(variants):
                want = len(open(variants, encoding='utf-8').read().split('\n')) - 1
                print(f'  {len(keys)} records, {len(data) >> 10} KB; {want} exported variants '
                      f'({"each" if want <= len(keys) else "NOT each"} with a record: the tool checked their keys)')
                if want > len(keys):
                    fail('fewer records than exported variants')
            else:
                print(f'  {len(keys)} records, {len(data) >> 10} KB')
            cache = {'file': os.path.basename(a.out), 'sha256': sha256(a.out), 'bytes': len(data),
                     'records': len(keys), 'exported_variants': want,
                     'variants_sha256': sha256(variants) if os.path.isfile(variants) else None}
    have = {(n[:2], int(n[3:19], 16)) for n in names}
    lists = []
    for p in a.list:
        hashes, n, version = list_hashes(p)
        lacking = sorted(hashes - have)
        print(f'  list {os.path.basename(p)}: version {version}, {n} pipelines, {len(hashes)} shaders, '
              f'{len(lacking)} not in the package'
              + (': ' + ', '.join(f'{s}_{h:016X}' for s, h in lacking[:8]) if lacking else ''))
        lists.append({'file': os.path.basename(p), 'sha256': sha256(p), 'version': version, 'pipelines': n,
                      'shaders': len(hashes), 'shaders_not_in_package': len(lacking)})
    if a.console_cache and os.path.isfile(a.out):
        check_console(a.console_cache, a.out, variants, a.runtime_log)
    if a.manifest:
        write_manifest(a.manifest, a, lists, cache)
    print('gta4_dksh_cache: ' + ('ok' if not failures else f'{len(failures)} checks failed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
