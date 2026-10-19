#!/usr/bin/env python3
"""Rewrites SPIRV-Cross's Vulkan GLSL of a ReXGlue guest shader into GLSL that
uam (deko3d's compiler, GLSL 4.60 through Mesa) can take. Probe for
docs/switch-port/13-deko3d-spike.md (D.3), not a production path: each step
is a construct that the translator itself would have to emit differently.

1. XeSystemConstants (scalar layout, mixed types, vec4 at offset 28): becomes
   a std140 block of uvec4 words; every member read is an accessor function
   that builds the value from 32-bit words (uintBitsToFloat for floats).
2. Descriptor sets: dropped; bindings renumbered per kind (uniform blocks,
   storage blocks, combined samplers), as deko3d binds per stage and kind.
3. Separate textures and samplers: each samplerXX(texture, sampler) pair used
   becomes one combined sampler uniform.
4. spirv_instruction NMin/NMax/NClamp (GLSL.std.450 79-81, no GLSL builtin):
   functions with isnan selects (same result: the non-NaN operand).
5. demote (GL_EXT_demote_to_helper_invocation): discard.
6. gl_VertexIndex / gl_InstanceIndex (Vulkan): gl_VertexID / gl_InstanceID.

Usage: vk_glsl_to_deko.py in.glsl out.glsl  (exit 0, or 1 with the reason)
"""
import re
import sys

SCALARS = {'uint': ('uint', 1), 'int': ('int', 1), 'float': ('float', 1),
           'uvec2': ('uint', 2), 'uvec3': ('uint', 3), 'uvec4': ('uint', 4),
           'ivec2': ('int', 2), 'ivec3': ('int', 3), 'ivec4': ('int', 4),
           'vec2': ('float', 2), 'vec3': ('float', 3), 'vec4': ('float', 4)}


def word(expr):
    return f'xe_sc_raw[({expr}) >> 2][({expr}) & 3]'


def conv(base, w):
    return {'uint': w, 'int': f'int({w})', 'float': f'uintBitsToFloat({w})'}[base]


def accessor(t, name, offset, array):
    base, n = SCALARS[t]
    k = offset // 4
    if array:
        idx = f'{k} + i * 4'  # every array of the block has a 16-byte stride (vec4/uvec4)
        comps = [conv(base, word(f'{idx} + {c}')) for c in range(n)]
        body = comps[0] if n == 1 else f'{t}({", ".join(comps)})'
        return f'{t} xe_sc_{name}(int i) {{ return {body}; }}\n'
    comps = [conv(base, word(str(k + c))) for c in range(n)]
    body = comps[0] if n == 1 else f'{t}({", ".join(comps)})'
    return f'{t} xe_sc_{name}() {{ return {body}; }}\n'


def main():
    src = open(sys.argv[1], encoding='utf-8').read()
    src = src.replace('#extension GL_EXT_scalar_block_layout : require\n', '')
    src = src.replace('#extension GL_EXT_demote_to_helper_invocation : require\n', '')
    src = src.replace('#extension GL_EXT_spirv_intrinsics : require\n', '')
    # 1. System constants.
    m = re.search(r'layout\(set = \d+, binding = \d+, scalar\) uniform XeSystemConstants\n\{\n(.*?)\} '
                  r'xe_uniform_system_constants;\n', src, re.S)
    funcs = ''
    if m:
        for mm in re.finditer(r'layout\(offset = (\d+)\) (\w+) (\w+)(\[\d+\])?;', m.group(1)):
            offset, t, name, arr = int(mm.group(1)), mm.group(2), mm.group(3), mm.group(4)
            if t not in SCALARS:
                print('system constant of type ' + t)
                return 1
            funcs += accessor(t, name, offset, bool(arr))
        src = src[:m.start()] + ('layout(set = 1, binding = 0, std140) uniform XeSystemConstantsRaw\n'
                                 '{\n    uvec4 xe_sc_raw[32];\n};\n\n') + src[m.end():]
        src = re.sub(r'xe_uniform_system_constants\.(\w+)\[([^\[\]]+)\]', r'xe_sc_\1(int(\2))', src)
        src = re.sub(r'xe_uniform_system_constants\.(\w+)', r'xe_sc_\1()', src)
        if 'xe_uniform_system_constants' in src:
            print('system constants: an access the rewrite does not handle')
            return 1
    # 3. Combined samplers: collect pairs, then drop the separate declarations.
    pairs = {}
    def combine(mm):
        key = (mm.group(1), mm.group(2), mm.group(3))
        pairs.setdefault(key, f'{mm.group(2)}_with_{mm.group(3)}')
        return pairs[key]
    src = re.sub(r'\b(sampler\w*)\((xe_texture\w+), (xe_sampler\w+)\)', combine, src)
    src = re.sub(r'layout\(set = \d+, binding = \d+\) uniform (texture\w+|sampler) \w+;\n', '', src)
    if re.search(r'\bxe_texture\w+\b', re.sub(r'xe_texture\w+_with_xe_sampler\w+', '', src)):
        print('texture used without a sampler (textureSize/texelFetch on a separate image)')
        return 1
    # 2. Bindings per kind, no sets.
    counters = {'uniform': 0, 'buffer': 0}
    def rebind(mm):
        kind = 'buffer' if 'buffer' in mm.group(2) else 'uniform'
        b = counters[kind]
        counters[kind] += 1
        return f'layout(binding = {b}{mm.group(1)}){mm.group(2)}'
    src = re.sub(r'layout\(set = \d+, binding = \d+((?:, \w+)*)\)( (?:restrict )?(?:readonly )?(?:writeonly )?(?:uniform|buffer) \w+\n)',
                 rebind, src)
    decls = ''.join(f'layout(binding = {i}) uniform {t} {name};\n'
                    for i, ((t, _, _), name) in enumerate(pairs.items()))
    if len(pairs) > 32:
        print('more than 32 combined samplers')
        return 1
    if counters['uniform'] > 16 or counters['buffer'] > 16:
        print('more than 16 uniform or storage blocks')
        return 1
    # 4. NMin/NMax/NClamp.
    src = re.sub(r'spirv_instruction\(set = "GLSL.std.450", id = \d+\) [^;]*;\n', '', src)
    if 'spirv_instruction' in src:
        print('other spirv_instruction')
        return 1
    nfuncs = ''
    for t in ('float', 'vec2', 'vec3', 'vec4'):
        nfuncs += (f'{t} spvNMin({t} a, {t} b) {{ return mix(mix(min(a, b), a, isnan(b)), b, isnan(a)); }}\n'
                   f'{t} spvNMax({t} a, {t} b) {{ return mix(mix(max(a, b), a, isnan(b)), b, isnan(a)); }}\n'
                   f'{t} spvNClamp({t} x, {t} lo, {t} hi) {{ return spvNMin(spvNMax(x, lo), hi); }}\n')
    # 5. demote.
    src = re.sub(r'\bdemote;', 'discard;', src)
    # 6. Vertex and instance index.
    src = re.sub(r'\bgl_VertexIndex\b', 'gl_VertexID', src)
    src = re.sub(r'\bgl_InstanceIndex\b', 'gl_InstanceID', src)
    # Declarations before the first function.
    at = src.index('\nvoid main()')
    src = src[:at] + '\n' + decls + funcs + nfuncs + src[at:]
    open(sys.argv[2], 'w', encoding='utf-8').write(src)
    return 0


if __name__ == '__main__':
    sys.exit(main())
