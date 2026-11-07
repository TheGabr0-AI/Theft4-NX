#!/usr/bin/env python3
"""The deko3d SPIR-V with XenosRecomp's state machines structured (docs/switch-port/16-deko3d-devlog.md, run 47).

    python tools/deko3d/gta4_deko3d_structure.py --spirv <XenosRecomp's deko3d SPIR-V dir> --out <new dir>

Copies every file of --spirv into --out; for every <vs|ps>_<HASH>[_late].hlsl whose state machine hlsl_structure.py can
structure, writes the structured HLSL and compiles it again with XenosRecomp's DXC and its deko3d-mode arguments
(tools/XenosRecomp/XenosRecomp/dxc_compiler.cpp with -DGTA4_DEKO3D and without -fvk-invert-y), replacing the .spv.
A shader DXC refuses keeps its original SPIR-V (listed). Then run gta4_deko3d_shaders.py with --spirv <new dir>.
Writes <out>/structure.txt. Everything here is game data: it stays out of the repository.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hlsl_structure  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DXC = os.path.join(ROOT, 'tools', 'XenosRecomp', 'thirdparty', 'dxc-bin', 'bin', 'x64', 'dxc.exe')


def dxc(hlsl, spv):
    pixel = os.path.basename(hlsl).startswith('ps_')
    r = subprocess.run([DXC, '-E', 'shaderMain', '-T', 'ps_6_0' if pixel else 'vs_6_0', '-HV', '2021', '-all-resources-bound',
                        '-spirv', '-fvk-use-dx-layout', '-Qstrip_debug', '-DGTA4_DEKO3D', '-Fo', spv, hlsl],
                       capture_output=True, text=True)
    return '' if r.returncode == 0 and os.path.isfile(spv) else (r.stderr or r.stdout).strip().split('\n')[0][:300]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spirv', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args()
    if os.path.abspath(a.spirv) == os.path.abspath(a.out):
        sys.exit('--out must be another directory')
    os.makedirs(a.out, exist_ok=True)
    names = sorted(os.listdir(a.spirv))
    for n in names:
        src = os.path.join(a.spirv, n)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(a.out, n))

    def one(name):
        stem = name[:-5]
        text = open(os.path.join(a.spirv, name), encoding='utf-8').read()
        new, message = hlsl_structure.structure(text)
        if new == text:
            return stem, 'none' if message == 'no state machine' else 'left', message
        hlsl = os.path.join(a.out, name)
        open(hlsl, 'w', encoding='utf-8', newline='\n').write(new)
        tmp = os.path.join(a.out, stem + '.spv.new')
        err = dxc(hlsl, tmp)
        if err:
            shutil.copy2(os.path.join(a.spirv, name), hlsl)  # the original text and SPIR-V stay
            if os.path.isfile(tmp):
                os.remove(tmp)
            return stem, 'dxc', err
        os.replace(tmp, os.path.join(a.out, stem + '.spv'))
        return stem, 'structured', message

    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(one, [n for n in names if n.endswith('.hlsl')]))
    count = {}
    for _, kind, _ in results:
        count[kind] = count.get(kind, 0) + 1
    lines = [f'{len(results)} HLSL: ' + ', '.join(f'{k} {v}' for k, v in sorted(count.items()))]
    for stem, kind, message in results:
        if kind in ('left', 'dxc', 'structured'):
            lines.append(f'{kind}\t{stem}\t{message}')
    open(os.path.join(a.out, 'structure.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    print(lines[0])
    return 0 if not count.get('dxc') else 1


if __name__ == '__main__':
    sys.exit(main())
