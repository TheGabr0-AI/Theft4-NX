#!/usr/bin/env python3
"""GTA IV shaders for deko3d: SPIR-V -> GLSL -> DKSH, every variant (docs/switch-port/16-deko3d-devlog.md, M1).

    python tools/deko3d/gta4_deko3d_shaders.py --spirv <dir> --cache-cpp <shader_cache.cpp> --out <dir>
        [--tools out/deko3d_tools] [--jobs 3] [--no-passes]

Input: the SPIR-V XenosRecomp writes with XENOS_RECOMP_DEKO3D_SPIRV_DIR (GTA4_DEKO3D: constants in three
uniform buffers, each fetch slot one texture at binding = slot, no push constants, no 64-bit integers,
Direct3D clip space), one <vs|ps>_<HASH>[_late].spv per shader, and the shader_cache.cpp XenosRecomp wrote
in the same run (the specialization mask of each hash).

For each shader:
  1. SPIRV-Cross: GLSL 4.60 without Vulkan semantics (uam's front end is GL's), combined samplers taking
     the texture's binding (--combined-samplers-inherit-bindings), NaN checks relaxed;
  2. checks on the text: no extension uam lacks (buffer references, int64, nonuniform), every UBO binding
     below 16 and every sampler binding below 32, no dynamic component index into a uniform vec4 (uam
     reads .x for it: nfsmw-nx HowToDeko3d 6.12);
  3. banjo-nx's GLSL passes (tools/deko3d/deko3d_glsl_opt.py, --check: a shader whose passes fail in uam
     keeps its input text);
  4. uam for every variant: the specialization value is SPIRV_CROSS_CONSTANT_ID_0, defined before the
     text. GTA IV's shaders use bit 0 (R11G11B10 normals) and the alpha test (0x702: bit 1 on, the
     function in bits 8-10); a shader whose mask is 0 has one variant, one with the alpha test nine (off
     and the eight functions), times two when bit 0 is in the mask;
  5. the DKSH cache file <out>/gta4_deko3d_shaders.bin (format below), byte-identical between runs.

Output: <out>/glsl, <out>/glsl_opt, <out>/dksh/<stem>_<value>.dksh, <out>/report.txt, the cache file.
Everything here is game data (translated from the game's shaders): it stays in the build tree.

Cache file (little endian): "GDK1", u32 version (1), u32 count, u32 uam tag length, the tag (uam
--version + the patch set + this tool's revision), then count entries of {u64 key, u32 offset, u32
size} sorted by key, then the DKSH blobs. key = XXH64-like FNV-1a 64 of the exact GLSL text uam compiled
(variant header included), so a changed converter or pass can never load a stale binary.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import struct
import subprocess
import sys

TOOL_REVISION = 'gta4-deko3d-1'
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FORBIDDEN = ('GL_EXT_buffer_reference', 'GL_ARB_gpu_shader_int64', 'GL_EXT_shader_explicit_arithmetic_types',
             'GL_EXT_nonuniform_qualifier', 'GL_EXT_samplerless_texture_functions', 'uint64_t', 'u64vec')
ALPHA_TEST_CAPABILITY = 0x702
R11G11B10 = 0x1


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def spec_masks(cache_cpp):
    """{hash: specConstantsMask} from XenosRecomp's shader_cache.cpp entries."""
    masks = {}
    entry = re.compile(r'^\t\{ 0x([0-9A-Fa-f]+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+), "')
    with open(cache_cpp, encoding='utf-8', errors='ignore') as f:
        for line in f:
            m = entry.match(line)
            if m:
                masks[int(m.group(1), 16)] = int(m.group(10))
    return masks


def variants(mask):
    values = [0]
    if (mask & ALPHA_TEST_CAPABILITY) == ALPHA_TEST_CAPABILITY:
        values += [0x2 | (f << 8) for f in range(8)]
    if mask & R11G11B10:
        values += [v | R11G11B10 for v in values]
    return values


def variant_text(glsl, value):
    lines = glsl.split('\n')
    k = 1 if lines and lines[0].startswith('#version') else 0
    lines.insert(k, f'#define SPIRV_CROSS_CONSTANT_ID_0 {value}u')
    return '\n'.join(lines)


UBO_RE = re.compile(r'layout\(binding = (\d+), std140\) uniform')
SAMPLER_RE = re.compile(r'layout\(binding = (\d+)\) uniform (?:sampler|isampler|usampler)\w+ (\w+)')
DYNAMIC_COMPONENT_RE = re.compile(r'(g_Dk(?:VertexConstants|PixelConstants|Shared)\[[^\]]+\])\[(?!\d+\])')


def check_text(glsl):
    """'' when uam can take it, else the first reason."""
    for f in FORBIDDEN:
        if f in glsl:
            return f'uses {f}'
    for m in UBO_RE.finditer(glsl):
        if int(m.group(1)) >= 16:
            return f'uniform buffer at binding {m.group(1)} (deko3d has 16)'
    for m in SAMPLER_RE.finditer(glsl):
        if int(m.group(1)) >= 32:
            return f'texture {m.group(2)} at binding {m.group(1)} (deko3d has 32)'
    m = DYNAMIC_COMPONENT_RE.search(glsl)
    if m:
        return f'dynamic component index into a uniform vec4: {m.group(0)[:60]}'
    if re.search(r'uniform\s+\w+\s+\w+\s*\[\s*\]', glsl):
        return 'runtime-sized resource array'
    return ''


VS_INPUT_RE = re.compile(r'^layout\(location = (\d+)\) in (\w+) (\w+);$', re.M)


def gta4_fixups(glsl, stage):
    """Two GTA IV forms uam refuses (M1's first run, 632 of 12,120 variants):

    - XenosRecomp gives each vertex input semantic a fixed location (COLOR0 17, BLENDWEIGHT and BLENDINDICES
      above 16), and uam (GL's limit) takes locations 0-15: the inputs are renumbered in the order of their
      locations, and the map is written on the second line ("// gta4-dk vertex-inputs old>new ...") for the
      renderer, which binds each vertex element at its new location;
    - some pixel shaders read constants c224-c255 (the Vulkan build reads them by address): every pixel
      shader declares 256 pixel constants (the renderer binds 4,096 bytes for every pixel shader)."""
    notes = []
    if stage == 'vert':
        inputs = sorted({int(m.group(1)) for m in VS_INPUT_RE.finditer(glsl)})
        remap = {old: new for new, old in enumerate(inputs)}
        glsl = VS_INPUT_RE.sub(lambda m: f'layout(location = {remap[int(m.group(1))]}) in {m.group(2)} {m.group(3)};',
                               glsl)
        notes.append('// gta4-dk vertex-inputs ' + ' '.join(f'{o}>{n}' for o, n in remap.items()))
    else:
        glsl = glsl.replace('vec4 g_DkPixelConstants[224];', 'vec4 g_DkPixelConstants[256];', 1)
        notes.append('// gta4-dk pixel-constants 256')
    if notes:
        nl = glsl.find('\n') + 1
        glsl = glsl[:nl] + '\n'.join(notes) + '\n' + glsl[nl:]
    return glsl


def to_glsl(tools, spv, out_glsl):
    r = subprocess.run([os.path.join(tools, 'spirv-cross-build', 'spirv-cross.exe'), spv, '--version', '460',
                        '--no-es', '--combined-samplers-inherit-bindings', '--relax-nan-checks',
                        '--output', out_glsl], capture_output=True, text=True)
    if r.returncode != 0:
        return (r.stderr or r.stdout).strip().split('\n')[0][:300]
    stage = 'vert' if os.path.basename(spv).startswith('vs_') else 'frag'
    text = gta4_fixups(open(out_glsl, encoding='utf-8').read(), stage)
    open(out_glsl, 'w', encoding='utf-8', newline='\n').write(text)
    return ''


UAM_TIMEOUT = 300  # seconds: uam can loop for ever on a few shaders (M1's second run: three vertex shaders)


def uam(tools, stage, glsl_path, dksh_path):
    try:
        r = subprocess.run([os.path.join(tools, 'uam-build', 'uam.exe'), '-s', stage, '-o', dksh_path, glsl_path],
                           capture_output=True, text=True, timeout=UAM_TIMEOUT)
    except subprocess.TimeoutExpired:
        if os.path.isfile(dksh_path):
            os.remove(dksh_path)
        return f'uam did not finish in {UAM_TIMEOUT} s', []
    log = (r.stdout + r.stderr).strip()
    warnings = [l.strip() for l in log.split('\n') if 'warning' in l.lower()]
    if r.returncode != 0 or not os.path.isfile(dksh_path):
        first = next((l for l in log.split('\n') if 'error' in l.lower()), log[:300])
        return first.strip()[:300], warnings
    return '', warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spirv', required=True)
    ap.add_argument('--cache-cpp', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--tools', default=os.path.join(ROOT, 'out', 'deko3d_tools'))
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--no-passes', action='store_true')
    ap.add_argument('--pass-timeout', type=int, default=60, help='seconds of banjo-nx passes per shader')
    ap.add_argument('--skip-glsl', action='store_true', help='reuse <out>/glsl from an earlier run')
    ap.add_argument('--reuse', action='store_true',
                    help='keep pass results and DKSH whose input text has not changed since an earlier run')
    a = ap.parse_args()

    for d in ('glsl', 'glsl_opt', 'dksh', 'variants'):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)
    masks = spec_masks(a.cache_cpp)
    spvs = sorted(f for f in os.listdir(a.spirv) if f.endswith('.spv'))
    failures = collections.defaultdict(list)  # reason -> [(name, detail)]
    dxc_failed = []
    failed_list = os.path.join(a.spirv, 'failed.txt')
    if os.path.isfile(failed_list):
        dxc_failed = [l.split('\t')[0] for l in open(failed_list, encoding='utf-8') if l.strip()]

    # 1-2: SPIR-V -> GLSL and the text checks
    def convert(name):
        stem = name[:-4]
        glsl = os.path.join(a.out, 'glsl', stem + '.glsl')
        err = to_glsl(a.tools, os.path.join(a.spirv, name), glsl)
        if err:
            return stem, 'spirv-cross', err
        return stem, None, check_text(open(glsl, encoding='utf-8').read())

    converted = []

    def convert_or_reuse(name):
        stem = name[:-4]
        glsl = os.path.join(a.out, 'glsl', stem + '.glsl')
        if a.skip_glsl and os.path.isfile(glsl):
            return stem, None, check_text(open(glsl, encoding='utf-8').read())
        return convert(name)

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for stem, stage_fail, detail in ex.map(convert_or_reuse, spvs):
            if stage_fail:
                failures[stage_fail].append((stem, detail))
            elif detail:
                failures['check'].append((stem, detail))
            else:
                converted.append(stem)

    # 3: banjo-nx's passes (tools/deko3d/deko3d_glsl_opt.py), one shader at a time with a time limit (some
    # passes' regular expressions backtrack for a very long time on a few large shaders), then banjo-nx's --check:
    # a shader whose optimised text uam refuses (default variant and the alpha-test ones) keeps its input text.
    glsl_dir = os.path.join(a.out, 'glsl')
    opt_dir = os.path.join(a.out, 'glsl_opt')
    passes = collections.Counter()
    passes_detail = collections.defaultdict(list)
    runner = ('import sys; sys.path.insert(0, sys.argv[1]); import deko3d_glsl_opt as o; '
              't = open(sys.argv[2], encoding="utf-8").read(); s = "frag" if "ps_" in sys.argv[2] else "vert"; '
              'n = sys.argv[2].replace(chr(92), "/").split("/")[-1]; '
              'open(sys.argv[3], "w", encoding="utf-8", newline="\\n").write(o.optimise(t, o.DEFAULT, s, n[3:19]))')

    def optimise(stem):
        src = os.path.join(glsl_dir, stem + '.glsl')
        dst = os.path.join(opt_dir, stem + '.glsl')
        original = open(src, encoding='utf-8').read()
        seen = os.path.join(opt_dir, stem + '.input')
        if a.reuse and os.path.isfile(dst) and os.path.isfile(seen) and open(seen, encoding='utf-8').read() == original:
            return stem, 'reused', ''
        open(seen, 'w', encoding='utf-8', newline='\n').write(original)
        if a.no_passes:
            open(dst, 'w', encoding='utf-8', newline='\n').write(original)
            return stem, 'off', ''
        try:
            r = subprocess.run([sys.executable, '-c', runner, os.path.join(ROOT, 'tools', 'deko3d'), src, dst],
                               capture_output=True, text=True, timeout=a.pass_timeout)
            if r.returncode != 0 or not os.path.isfile(dst):
                open(dst, 'w', encoding='utf-8', newline='\n').write(original)
                return stem, 'error', (r.stderr or r.stdout).strip().split('\n')[-1][:200]
        except subprocess.TimeoutExpired:
            open(dst, 'w', encoding='utf-8', newline='\n').write(original)
            return stem, 'timeout', f'{a.pass_timeout} s'
        text = open(dst, encoding='utf-8').read()
        if text == original:
            return stem, 'unchanged', ''
        stage = 'frag' if stem.startswith('ps_') else 'vert'
        for value in ([0, 0x702, 0x703] if 'SPIRV_CROSS_CONSTANT_ID_0' in text else [0]):
            probe = os.path.join(a.out, 'variants', f'{stem}_check{value:x}.glsl')
            open(probe, 'w', encoding='utf-8', newline='\n').write(variant_text(text, value))
            err, _ = uam(a.tools, stage, probe, probe[:-5] + '.dksh')
            if err:
                open(dst, 'w', encoding='utf-8', newline='\n').write(original)
                return stem, 'reverted (uam refused the optimised text)', err[:200]
        return stem, 'optimised', ''

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for stem, outcome, detail in ex.map(optimise, converted):
            passes[outcome] += 1
            if detail:
                passes_detail[outcome].append((stem, detail))
    passes_log = ', '.join(f'{k} {v}' for k, v in passes.most_common())
    for outcome, items in passes_detail.items():
        passes_log += f'\n  {outcome}: ' + '; '.join(f'{s} ({d})' for s, d in items[:10])

    # 4: every variant through uam
    jobs = []
    for stem in converted:
        m = re.match(r'(vs|ps)_([0-9A-F]{16})(_late)?$', stem)
        if not m:
            failures['name'].append((stem, 'not <vs|ps>_<HASH>[_late]'))
            continue
        mask = masks.get(int(m.group(2), 16))
        if mask is None:
            failures['no cache entry'].append((stem, 'hash not in shader_cache.cpp'))
            continue
        for value in variants(mask):
            jobs.append((stem, 'frag' if m.group(1) == 'ps' else 'vert', value))

    warnings = collections.Counter()
    warned = collections.defaultdict(list)
    blobs = {}

    def compile_variant(job):
        stem, stage, value = job
        text = variant_text(open(os.path.join(opt_dir, stem + '.glsl'), encoding='utf-8').read(), value)
        path = os.path.join(a.out, 'variants', f'{stem}_{value:x}.glsl')
        dksh = os.path.join(a.out, 'dksh', f'{stem}_{value:x}.dksh')
        if a.reuse and os.path.isfile(path) and os.path.isfile(dksh) and open(path, encoding='utf-8').read() == text:
            return job, text, dksh, '', []
        open(path, 'w', encoding='utf-8', newline='\n').write(text)
        if os.path.isfile(dksh):
            os.remove(dksh)
        err, warns = uam(a.tools, stage, path, dksh)
        return job, text, dksh, err, warns

    compiled = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for (stem, stage, value), text, dksh, err, warns in ex.map(compile_variant, jobs):
            for w in set(warns):
                key = re.sub(r'\d+', 'N', w)[:160]
                warnings[key] += 1
                warned[key].append(f'{stem}_{value:x}')
            if not err and not os.path.isfile(dksh):
                err = 'uam wrote no DKSH'
            if err:
                failures['uam'].append((f'{stem}_{value:x}', err))
                continue
            compiled += 1
            blobs[fnv1a64(text.encode('utf-8'))] = open(dksh, 'rb').read()

    # 5: the cache file
    r = subprocess.run([os.path.join(a.tools, 'uam-build', 'uam.exe'), '-v'], capture_output=True, text=True)
    tag = ((r.stdout + r.stderr).strip().split('\n')[0] + ' patches 1-8 ' + TOOL_REVISION).encode('utf-8')
    keys = sorted(blobs)
    header = b'GDK1' + struct.pack('<III', 1, len(keys), len(tag)) + tag
    table_size = len(keys) * 16
    offset = len(header) + table_size
    table, data = b'', b''
    for k in keys:
        table += struct.pack('<QII', k, offset + len(data), len(blobs[k]))
        data += blobs[k]
        data += b'\0' * ((-len(data)) % 256)
    with open(os.path.join(a.out, 'gta4_deko3d_shaders.bin'), 'wb') as f:
        f.write(header + table + data)

    total_shaders = len(spvs) + len(dxc_failed)
    lines = [f'{total_shaders} shaders (SPIR-V {len(spvs)}, DXC failed {len(dxc_failed)}), '
             f'{len(converted)} converted to GLSL',
             f'variants: {len(jobs)} requested, {compiled} compiled, {len(failures.get("uam", []))} failed in uam',
             f'cache: {len(keys)} distinct DKSH, {offset + len(data)} bytes, tag {tag.decode()}']
    if dxc_failed:
        lines.append(f'\n[DXC] {len(dxc_failed)}')
        lines += [f'  {n}' for n in dxc_failed[:40]]
    for reason, items in sorted(failures.items(), key=lambda kv: -len(kv[1])):
        lines.append(f'\n[{reason}] {len(items)}')
        by = collections.Counter(re.sub(r'_[0-9A-Fa-f]{6,}|\d+', 'N', d)[:200] for _, d in items)
        for d, n in by.most_common(20):
            ex_name = next(s for s, dd in items if re.sub(r'_[0-9A-Fa-f]{6,}|\d+', 'N', dd)[:200] == d)
            lines.append(f'  {n:5}  {d}   (e.g. {ex_name})')
    if warnings:
        lines.append('\n[uam warnings]')
        for w, n in warnings.most_common(20):
            lines.append(f'  {n:5}  {w}   (e.g. {warned[w][0]})')
    if passes_log:
        lines.append('\n[banjo-nx passes]\n' + passes_log[-3000:])
    report = '\n'.join(lines)
    open(os.path.join(a.out, 'report.txt'), 'w', encoding='utf-8').write(report + '\n')
    print(report)


if __name__ == '__main__':
    main()
