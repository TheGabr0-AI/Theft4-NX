#!/usr/bin/env python3
"""Regression test of ultimate/tools/deko3d_glsl.py: a fetch gets a bound texture (dk_tex_bK, the banjo-dk line's texb)
only when its texture and sampler words are fixed SharedConstants words.

    python tests/ultimate/test_deko3d_glsl_temps.py

The defect: the converter resolved a descriptor index through SPIRV-Cross's `uint _N = expr;` temporaries by their
declaration alone. A temporary written again (a phi with its first value from the dominating block, a loop's carried
value, an out argument) was taken for the word it was declared with, and its fetch bound that word's texture whatever
the temporary held at the fetch. Synthetic texts in SPIRV-Cross's form; no game data.
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'ultimate', 'tools'))
import deko3d_glsl  # noqa: E402

HEAD = '''#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 7, binding = 0, std140) uniform type_SharedConstants
{
    uvec4 v[96];
} g_UboShared;

layout(set = 0, binding = 0) uniform texture2D g_Texture2DDescriptorHeap[];
layout(set = 3, binding = 0) uniform sampler g_SamplerDescriptorHeap[];

layout(location = 0) in vec2 in_var_TEXCOORD0;
layout(location = 0) out vec4 out_var_SV_Target0;

void main()
{
'''
TAIL = '}\n'
T0, S0 = 'g_UboShared.v[0u].x', 'g_UboShared.v[24u].x'      # fetch constant 0: bytes 0 and 384
T1, S1 = 'g_UboShared.v[0u].y', 'g_UboShared.v[24u].y'      # fetch constant 1: bytes 4 and 388


def fetch(t, s, out='out_var_SV_Target0'):
    return (f'    {out} = texture(sampler2D(g_Texture2DDescriptorHeap[nonuniformEXT({t} & 67108863u)], '
            f'g_SamplerDescriptorHeap[nonuniformEXT({s})]), in_var_TEXCOORD0);\n')


def convert(body):
    text, _ = deko3d_glsl.convert(HEAD + body + TAIL, 'frag', deko3d_glsl.DEFAULT_UBO_MAP, 'handle')
    meta = next(l for l in text.split('\n') if l.startswith('// banjo-dk'))
    texb = re.search(r' texb=(\S+)', meta)
    return text, meta, (texb.group(1).split(',') if texb else [])


class FixedWords(unittest.TestCase):
    """What was right before stays: fixed words are bound, through temporaries or not."""

    def test_direct_words_are_bound(self):
        text, meta, texb = convert(fetch(T0, S0))
        self.assertEqual(texb, ['0:0:384'])
        self.assertIn('texture(dk_tex_b0, in_var_TEXCOORD0)', text)
        self.assertNotIn('dkh(', text)
        self.assertIn(' tex=0', meta)

    def test_single_assignment_temporaries_are_bound(self):
        body = f'    uint _10 = {T1};\n    uint _11 = {S1};\n    uint _12 = _10 & 67108863u;\n' + fetch('_12', '_11')
        text, _, texb = convert(body)
        self.assertEqual(texb, ['0:4:388'])
        self.assertIn('texture(dk_tex_b0, in_var_TEXCOORD0)', text)

    def test_sampler_variant_through_a_temporary(self):
        body = f'    uint _11 = {S0} + 2u;\n' + fetch(T0, '_11')
        _, _, texb = convert(body)
        self.assertEqual(texb, ['0:0:384:2'])

    def test_a_comparison_is_not_a_write(self):
        body = (f'    uint _10 = {T0};\n    bool _20 = _10 == 0u;\n    bool _21 = _10 != 5u;\n    bool _22 = _10 <= 7u;\n'
                f'    bool _23 = _10 >= 7u;\n' + fetch('_10', S0))
        _, _, texb = convert(body)
        self.assertEqual(texb, ['0:0:384'])

    def test_another_variables_write_does_not_count(self):
        # _1 is written; _10 and a struct member _10 of something else are not the same variable
        body = (f'    uint _10 = {T0};\n    uint _1 = 3u;\n    _1 = 4u;\n    uint x_10 = 0u;\n    x_10 = 1u;\n'
                + fetch('_10', S0))
        _, _, texb = convert(body)
        self.assertEqual(texb, ['0:0:384'])


class MutableTemporaries(unittest.TestCase):
    """A temporary written after its declaration is not a fixed word: its fetch stays bindless and reads the
    temporary itself."""

    def assert_bindless(self, body, expect_in):
        text, meta, texb = convert(body)
        self.assertEqual(texb, [], meta)
        self.assertNotIn('dk_tex_b', text)
        self.assertIn(' tex=1', meta)
        self.assertIn(expect_in, text)
        return text

    def test_texture_word_reassigned_in_a_branch(self):
        body = (f'    uint _10 = {T0};\n    if (in_var_TEXCOORD0.x > 0.5)\n    {{\n        _10 = {T1};\n    }}\n'
                + fetch('_10', S0))
        self.assert_bindless(body, 'sampler2D(dkh((_10 & 67108863u), (g_UboShared.v[24u].x)))')

    def test_sampler_word_reassigned_in_a_branch(self):
        body = (f'    uint _11 = {S0};\n    if (in_var_TEXCOORD0.x > 0.5)\n    {{\n        _11 = {S1};\n    }}\n'
                + fetch(T0, '_11'))
        self.assert_bindless(body, 'sampler2D(dkh((g_UboShared.v[0u].x & 67108863u), (_11)))')

    def test_reassigned_before_the_fetch_in_straight_line_code(self):
        body = f'    uint _10 = {T0};\n    _10 = {T1};\n' + fetch('_10', S0)
        self.assert_bindless(body, 'dkh((_10 & 67108863u)')

    def test_masked_copy_of_a_mutable_temporary(self):
        body = (f'    uint _10 = {T0};\n    for (int i = 0; i < 2; i++)\n    {{\n        _10 = {T1};\n    }}\n'
                f'    uint _12 = _10 & 67108863u;\n' + fetch('_12', S0))
        self.assert_bindless(body, 'dkh((_12 & 67108863u)')

    def test_compound_assignment_and_increment(self):
        for write in ('_11 += 1048576u;', '_11 |= 1u;', '_11 <<= 1u;', '_11++;', '++_11;', '_11--;'):
            with self.subTest(write=write):
                body = f'    uint _11 = {S0};\n    {write}\n' + fetch(T0, '_11')
                self.assert_bindless(body, '(_11)))')

    def test_variant_temporary_written_again(self):
        # `word + 2u` was taken for variant 2 of the word's sampler: not when the temporary changes
        body = f'    uint _11 = {S0} + 2u;\n    if (in_var_TEXCOORD0.y > 0.5)\n    {{\n        _11 = {S1};\n    }}\n' \
               + fetch(T0, '_11')
        text = self.assert_bindless(body, '(_11)))')
        self.assertNotIn('2097152u', text)  # no variant offset invented for it either

    def test_out_argument_of_a_builtin(self):
        body = f'    uint _10 = {T0};\n    uint _30 = uaddCarry(1u, 2u, _10);\n' + fetch('_10', S0)
        self.assert_bindless(body, 'dkh((_10 & 67108863u)')

    def test_out_argument_of_a_function(self):
        src = (HEAD.replace('void main()', 'void pick(out uint w)\n{\n    w = 1u;\n}\n\nvoid main()')
               + f'    uint _10 = {T0};\n    pick(_10);\n' + fetch('_10', S0) + TAIL)
        text, _ = deko3d_glsl.convert(src, 'frag', deko3d_glsl.DEFAULT_UBO_MAP, 'handle')
        self.assertNotIn('dk_tex_b', text)
        self.assertNotIn(' texb=', text)

    def test_declared_twice(self):
        body = (f'    if (in_var_TEXCOORD0.x > 0.5)\n    {{\n        uint _10 = {T0};\n'
                f'        out_var_SV_Target0 = vec4(float(_10));\n    }}\n    uint _10 = {T1};\n' + fetch('_10', S0))
        self.assert_bindless(body, 'dkh((_10 & 67108863u)')

    def test_texture_used_alone(self):
        body = (f'    uint _10 = {T0};\n    if (in_var_TEXCOORD0.x > 0.5)\n    {{\n        _10 = {T1};\n    }}\n'
                '    ivec2 _40 = textureSize(g_Texture2DDescriptorHeap[nonuniformEXT(_10 & 67108863u)], 0);\n'
                '    out_var_SV_Target0 = vec4(vec2(_40), 0.0, 1.0);\n')
        self.assert_bindless(body, 'textureSize(sampler2D(uvec2(uint(_10), 0u)), 0)')

    def test_only_the_mutable_fetch_stays_bindless(self):
        body = (f'    uint _10 = {T0};\n    uint _13 = {T1};\n    if (in_var_TEXCOORD0.x > 0.5)\n    {{\n'
                f'        _13 = {T0};\n    }}\n' + fetch('_10', S0) + fetch('_13', S1))
        text, meta, texb = convert(body)
        self.assertEqual(texb, ['0:0:384'])
        self.assertIn('texture(dk_tex_b0, in_var_TEXCOORD0)', text)
        self.assertIn('sampler2D(dkh((_13 & 67108863u), (g_UboShared.v[24u].y)))', text)
        self.assertIn(' tex=1', meta)


class Helper(unittest.TestCase):
    def test_immutable_uint_temps(self):
        src = ('    uint _1 = a;\n    uint _2 = b;\n    _2 = c;\n    uint _3;\n    _3 = d;\n    const uint _4 = e;\n'
               '    bool k = _1 == _4;\n    s._1 = 2u;\n    uint _5 = f;\n    _5 >>= 1u;\n    uint _6 = g;\n    --_6;\n')
        self.assertEqual(deko3d_glsl.immutable_uint_temps(src), {'_1': 'a', '_4': 'e'})


if __name__ == '__main__':
    unittest.main(verbosity=1)
