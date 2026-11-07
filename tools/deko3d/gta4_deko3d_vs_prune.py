#!/usr/bin/env python3
"""Run 56: vertex shader variants that write fewer outputs (docs/switch-port/16-deko3d-devlog.md, section AD).

    python tools/deko3d/gta4_deko3d_vs_prune.py --out-dir <package dir> --cache-cpp <shader_cache.cpp>
        --tools out/deko3d_tools --dest <dir> [--jobs 6] [--only HASH,HASH]

Every vertex shader of the package declares and writes 18 vec4 outputs (TEXCOORD0-15, COLOR0-1: XenosRecomp's
interface), most of them the constant vec4(0.0) (banjo-nx's pass marks them in "// banjo-vszero=0x..."; 9 to 18 of the 18
in every one of the 636). Each written output is stored per vertex and set up per primitive whether a pixel shader reads
it or not. banjo-nx's GLSL gates every output with the constant SPIRV_CROSS_CONSTANT_ID_905 (default: all), which the
package left at all ones. masseffect-nx's optimisation S6 (vertex outputs the pixel shader does not read, zero for depth
only) measured its scene at 66 -> 36.6 ms.

Two variants per vertex shader variant, compiled here with the PC's uam (the console never compiles them):
  position only (905 = 0): no generic output; for the draws without a pixel shader (GTA IV's shadow maps bind none) and
    the depth-only rule's draws (run 55);
  live outputs (905 = the outputs that are not the constant zero): for a pixel shader that reads none of the outputs
    this vertex shader writes as zero (the recorder checks it with the pixel shader's declared inputs).
Each DKSH is checked through its NVIDIA shader program header (thirdparty/uam/source/nv_shader_header.h, the VTG output
map omap_generic_vector at byte 54): the position-only one must write no generic attribute, the live one none outside
its mask, and the full variant (compiled once per shader for the check, not kept) must write all 18: else the variant is
not used. Writes <dest>/<stem>_<value>_p.dksh and _l.dksh, and <dest>/prune.json (masks and the check per variant),
which gta4_deko3d_runtime.py --pruned-dksh reads.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gta4_deko3d_shaders as chain  # noqa: E402

ZERO_RE = re.compile(r'^// banjo-vszero=0x([0-9a-fA-F]+)\s*$', re.M)
GATE = '#ifndef SPIRV_CROSS_CONSTANT_ID_905'
ALL_OUTPUTS = (1 << 18) - 1
POSITION_ONLY_BIT = 0x80000000  # dk_shader_package.h kVariantPositionOnly
LIVE_OUTPUTS_BIT = 0x40000000   # dk_shader_package.h kVariantLiveOutputs


def gated_text(glsl, value, outputs):
    """The variant's text (chain.variant_text) with the output gate set to `outputs` before its default."""
    text = chain.variant_text(glsl, value)
    if GATE not in text:
        return None
    return text.replace(GATE, f'#define SPIRV_CROSS_CONSTANT_ID_905 {outputs}u\n' + GATE, 1)


def written_outputs(dksh):
    """Generic attributes the vertex program writes (bit per attribute), from its SPH; None if not a VTG program."""
    if len(dksh) < 24:
        return None
    magic, _, control, _, programs_off, programs = __import__('struct').unpack_from('<6I', dksh, 0)
    if magic != 0x48534B44 or programs != 1:
        return None
    ptype, entry = __import__('struct').unpack_from('<II', dksh, programs_off)
    sph = control + entry
    if ptype != 0 or sph + 80 > len(dksh) or dksh[sph] & 0x1F != 1:  # DkStage_Vertex, NvSphType_VTG
        return None
    omap = dksh[sph + 54:sph + 70]
    mask = 0
    for a in range(32):
        if (omap[a // 2] >> (4 * (a % 2))) & 0xF:
            mask |= 1 << a
    return mask


def compile_one(tools, glsl_path, dksh_path):
    if os.path.isfile(dksh_path) and os.path.getsize(dksh_path) > 0:
        return ''
    err, _ = chain.uam(tools, 'vert', glsl_path, dksh_path)
    if not err and not os.path.isfile(dksh_path):
        err = 'uam wrote no DKSH'
    return err


def job(tools, dest, stem, glsl, value, zero):
    out = {'stem': stem, 'value': value, 'zero': zero}
    live = ALL_OUTPUTS & ~zero
    for kind, mask in (('f', ALL_OUTPUTS), ('p', 0), ('l', live)):
        text = gated_text(glsl, value, mask)
        if text is None:
            out['error'] = 'no output gate'
            return out
        g = os.path.join(dest, f'{stem}_{value:x}_{kind}.glsl')
        d = g[:-5] + '.dksh'
        with open(g, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        err = compile_one(tools, g, d)
        if err:
            out[kind] = {'error': err[:300]}
            continue
        w = written_outputs(open(d, 'rb').read())
        out[kind] = {'written': w, 'bytes': os.path.getsize(d)}
        if kind == 'f':
            os.remove(d)  # the full variant is the package's own; compiled only for the check
    f = out.get('f', {})
    ok_full = f.get('written') is not None and (f['written'] & ALL_OUTPUTS) == ALL_OUTPUTS
    for kind, mask in (('p', 0), ('l', live)):
        r = out.get(kind, {})
        r['ok'] = bool(ok_full and r.get('written') is not None and (r['written'] & ~mask) == 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--cache-cpp', required=True)
    ap.add_argument('--tools', required=True)
    ap.add_argument('--dest', required=True)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    masks = chain.spec_masks(a.cache_cpp)
    opt = os.path.join(a.out_dir, 'glsl_opt')
    os.makedirs(a.dest, exist_ok=True)
    only = {int(h, 16) for h in a.only.split(',') if h}
    jobs = []
    for name in sorted(os.listdir(opt)):
        m = re.fullmatch(r'vs_([0-9A-F]{16})\.glsl', name)
        if not m:
            continue
        h = int(m.group(1), 16)
        if h not in masks or (only and h not in only):
            continue
        glsl = open(os.path.join(opt, name), encoding='utf-8').read()
        z = ZERO_RE.search(glsl)
        zero = int(z.group(1), 16) & ALL_OUTPUTS if z else 0
        for value in chain.variants(masks[h]):
            jobs.append((name[:-5], glsl, value, zero))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = [pool.submit(job, a.tools, a.dest, *j) for j in jobs]
        for i, fut in enumerate(concurrent.futures.as_completed(futures)):
            results.append(fut.result())
            if (i + 1) % 100 == 0:
                print(f'{i + 1} of {len(jobs)}', flush=True)
    results.sort(key=lambda r: (r['stem'], r['value']))
    with open(os.path.join(a.dest, 'prune.json'), 'w') as f:
        json.dump(results, f, indent=0)
    ok_p = sum(r.get('p', {}).get('ok', False) for r in results)
    ok_l = sum(r.get('l', {}).get('ok', False) for r in results)
    print(f'{len(results)} vertex shader variants: position only {ok_p} checked, live outputs {ok_l} checked; '
          f'{os.path.join(a.dest, "prune.json")}')
    return 0 if results else 1


if __name__ == '__main__':
    sys.exit(main())
