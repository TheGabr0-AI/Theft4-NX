#!/usr/bin/env python3
"""Run 56: the package's shaders with every function-scope local given a value (docs/switch-port/16-deko3d-devlog.md,
section AD).

    python tools/deko3d/gta4_deko3d_zero_init.py compare --out-dir <package> --tools out/deko3d_tools [--limit N]
    python tools/deko3d/gta4_deko3d_zero_init.py build --out-dir <package> --dest <new package dir> --cache-cpp <cpp>
        --tools out/deko3d_tools [--jobs 6]

SPIRV-Cross declares a register XenosRecomp only partly writes as `vec4 _465;` and assigns lanes one by one; a lane
never written is undefined. The PC's drivers and probe_compare.py's interpreter read it as 0; run 55's GPU probe read it
on the console as NaN (`_465.w`, `_486.w`, `_672.yzw`: the only differences in four character vertex shaders, 109 to
117 values a vertex). Where such a lane reaches a result the console computes something the PC does not. Which shaders
that is cannot be read off the text reliably (lanes move through swizzles and the legacy multiply's zero guard), so
uam answers it: `compare` compiles each shader as it is and with every such local set to zero (`vec4 _465 = vec4(0.0);`);
equal DKSH = the undefined lanes were dead there, a different one = a value nobody wrote was used.

`build` writes the new package directory: glsl_opt with the zero-initialised texts, and gta4_deko3d_shaders.bin (the PC's
DKSH cache, GDK1, the tag of the old one) holding a DKSH for every variant of the new texts, compiled here; then
gta4_deko3d_runtime.py, gta4_deko3d_vs_prune.py and gta4_deko3d_package.py run on that directory as on any other.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gta4_deko3d_shaders as chain  # noqa: E402

ZERO = {'float': '0.0', 'vec2': 'vec2(0.0)', 'vec3': 'vec3(0.0)', 'vec4': 'vec4(0.0)', 'int': '0',
        'ivec2': 'ivec2(0)', 'ivec3': 'ivec3(0)', 'ivec4': 'ivec4(0)', 'uint': '0u', 'uvec2': 'uvec2(0u)',
        'uvec3': 'uvec3(0u)', 'uvec4': 'uvec4(0u)', 'bool': 'false', 'bvec2': 'bvec2(false)',
        'bvec3': 'bvec3(false)', 'bvec4': 'bvec4(false)'}
DECL = re.compile(r'^(\s*)(' + '|'.join(ZERO) + r')\s+(_\d+);[ \t]*$', re.M)


def zero_init(text):
    """Every `T _n;` (SPIRV-Cross's function-scope temporaries; globals have names, not _n) with T's zero."""
    count = 0

    def repl(m):
        nonlocal count
        count += 1
        return f'{m.group(1)}{m.group(2)} {m.group(3)} = {ZERO[m.group(2)]};'

    out = DECL.sub(repl, text)
    return out, count


def stage_of(name):
    return 'vert' if name.startswith('vs_') else 'frag'


def compile_text(tools, stage, text, work, stem):
    g = os.path.join(work, stem + '.glsl')
    d = os.path.join(work, stem + '.dksh')
    with open(g, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    if os.path.isfile(d):
        os.remove(d)
    err, _ = chain.uam(tools, stage, g, d)
    if err or not os.path.isfile(d):
        return None, err or 'no DKSH'
    return open(d, 'rb').read(), ''


def compare(a):
    opt = os.path.join(a.out_dir, 'glsl_opt')
    names = sorted(n for n in os.listdir(opt) if re.fullmatch(r'(vs|ps)_[0-9A-F]{16}(_late)?\.glsl', n))
    if a.limit:
        step = max(1, len(names) // a.limit)
        names = names[::step][:a.limit]
    work = tempfile.mkdtemp(prefix='zero_init_')
    results = {}

    def one(name):
        text = open(os.path.join(opt, name), encoding='utf-8').read()
        new, count = zero_init(text)
        if not count:
            return name, 'no locals', 0
        stem = name[:-5]
        before, e1 = compile_text(a.tools, stage_of(name), chain.variant_text(text, 0), work, stem + '_a')
        after, e2 = compile_text(a.tools, stage_of(name), chain.variant_text(new, 0), work, stem + '_b')
        if before is None or after is None:
            return name, f'uam failed: {e1 or e2}'[:120], count
        return name, 'same' if before == after else 'DIFFERENT', count

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for name, verdict, count in pool.map(one, names):
            results[name] = (verdict, count)
    shutil.rmtree(work, ignore_errors=True)
    tally = {}
    for name, (verdict, count) in results.items():
        key = verdict.split(':')[0]
        tally[key] = tally.get(key, 0) + 1
    for name, (verdict, count) in sorted(results.items()):
        if verdict == 'DIFFERENT':
            print(f'{name}: DIFFERENT ({count} locals given a value)')
    print(f'{len(results)} shaders compiled as they are and zero-initialised: {tally}')
    return 0


def write_cache(path, tag, blobs):
    """GDK1 (gta4_deko3d_shaders.py's format): magic, version 1, count, tag length, tag, {u64 key, u32 offset, u32 size}
    sorted by key, the blobs."""
    keys = sorted(blobs)
    table_at = 16 + len(tag)
    data_at = table_at + 16 * len(keys)
    table = b''
    data = b''
    for k in keys:
        table += struct.pack('<QII', k, data_at + len(data), len(blobs[k]))
        data += blobs[k]
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sIII', b'GDK1', 1, len(keys), len(tag)) + tag + table + data)


def build(a):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gta4_deko3d_runtime as runtime  # noqa: E402
    masks = chain.spec_masks(a.cache_cpp)
    src_opt = os.path.join(a.out_dir, 'glsl_opt')
    dst_opt = os.path.join(a.dest, 'glsl_opt')
    os.makedirs(dst_opt, exist_ok=True)
    tag, old_blobs = runtime.read_dksh_cache(os.path.join(a.out_dir, 'gta4_deko3d_shaders.bin'))
    jobs = []
    changed = 0
    skipped = 0  # variants the PC's uam could not compile before either (their DKSH come from --extra-dksh, unchanged)
    for name in sorted(os.listdir(src_opt)):
        src = os.path.join(src_opt, name)
        if not name.endswith('.glsl'):
            shutil.copy2(src, os.path.join(dst_opt, name))
            continue
        text = open(src, encoding='utf-8').read()
        new, count = zero_init(text) if name[:2] in a.stages.split(',') else (text, 0)
        changed += count > 0
        with open(os.path.join(dst_opt, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(new)
        m = re.fullmatch(r'(vs|ps)_([0-9A-F]{16})(_late)?\.glsl', name)
        if m and int(m.group(2), 16) in masks:
            for value in chain.variants(masks[int(m.group(2), 16)]):
                if chain.fnv1a64(chain.variant_text(text, value).encode('utf-8')) not in old_blobs:
                    skipped += 1
                    continue
                if not count:
                    continue  # the text is unchanged: its DKSH is the old cache's, kept below
                jobs.append((name, value, chain.variant_text(new, value)))
    work = os.path.join(a.dest, 'zero_init_work')
    os.makedirs(work, exist_ok=True)
    blobs = dict(old_blobs)  # the unchanged texts keep their DKSH (same key)
    failed = []

    def one(j):
        name, value, text = j
        stem = f'{name[:-5]}_{value:x}'
        d = os.path.join(work, stem + '.dksh')
        if os.path.isfile(d) and os.path.isfile(d + '.key'):
            return j, open(d, 'rb').read(), ''
        blob, err = compile_text(a.tools, stage_of(name), text, work, stem)
        if blob is not None:
            open(d + '.key', 'w').write('%016x' % chain.fnv1a64(text.encode('utf-8')))
        return j, blob, err

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for i, (j, blob, err) in enumerate(pool.map(one, jobs)):
            if blob is None:
                failed.append((f'{j[0]} {j[1]:x}', err[:160]))
            else:
                blobs[chain.fnv1a64(j[2].encode('utf-8'))] = blob
            if (i + 1) % 500 == 0:
                print(f'{i + 1} of {len(jobs)}', flush=True)
    write_cache(os.path.join(a.dest, 'gta4_deko3d_shaders.bin'), tag, blobs)
    for name in ('gta4_deko3d_glsl.bin',):
        if os.path.isfile(os.path.join(a.out_dir, name)) and not os.path.isfile(os.path.join(a.dest, name)):
            shutil.copy2(os.path.join(a.out_dir, name), os.path.join(a.dest, name))
    print(f'{changed} shaders zero-initialised; {len(jobs)} variants, {len(blobs)} DKSH, {len(failed)} failed in uam, '
          f'{skipped} not compiled before either (left to --extra-dksh)')
    for f in failed[:20]:
        print('  failed:', *f)
    return 0 if not failed else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['compare', 'build'])
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--dest')
    ap.add_argument('--cache-cpp')
    ap.add_argument('--tools', required=True)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--stages', default='vs,ps',
                    help='build: the stages whose texts are zero-initialised (run 56: vs; compare found no pixel '
                         'shader whose code changes, and their uam compiles take ~10 s each)')
    a = ap.parse_args()
    return compare(a) if a.mode == 'compare' else build(a)


if __name__ == '__main__':
    sys.exit(main())
