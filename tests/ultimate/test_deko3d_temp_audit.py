#!/usr/bin/env python3
"""Test of tools/ultimate/deko3d_temp_audit.py on synthetic SPIRV-Cross texts (no game data).

    python tests/ultimate/test_deko3d_temp_audit.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, 'tools', 'ultimate', 'deko3d_temp_audit.py')
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(TOOL))
import test_deko3d_glsl_temps as t  # noqa: E402  (the synthetic shader text)
import deko3d_temp_audit as audit  # noqa: E402
import deko3d_glsl  # noqa: E402

CLEAN = t.HEAD + f'    uint _10 = {t.T0};\n' + t.fetch('_10', t.S0) + t.TAIL
BAD = (t.HEAD + f'    uint _10 = {t.T0};\n    if (in_var_TEXCOORD0.x > 0.5)\n    {{\n        _10 = {t.T1};\n    }}\n'
       + t.fetch('_10', t.S0) + t.fetch(t.T1, t.S1) + t.TAIL)
# a temporary written again that no fetch goes through: nothing to report
HARMLESS = t.HEAD + f'    uint _50 = 1u;\n    _50 = 2u;\n' + t.fetch(t.T0, t.S0) + t.TAIL


class Audit(unittest.TestCase):
    def test_audit(self):
        self.assertEqual(audit.audit(CLEAN, 'frag'), (['0:0:384'], ['0:0:384'], {}))
        before, now, mutable = audit.audit(BAD, 'frag')
        self.assertEqual(before, ['0:0:384', '1:4:388'])   # the first fetch bound to word 0's texture
        self.assertEqual(now, ['0:4:388'])
        self.assertEqual(mutable, {'_10': t.T0})
        before, now, mutable = audit.audit(HARMLESS, 'frag')
        self.assertEqual(before, now)
        self.assertEqual(list(mutable), ['_50'])
        # the converter is left as it was
        self.assertIsNot(deko3d_glsl.immutable_uint_temps, audit.legacy_temps)

    def run_tool(self, files):
        with tempfile.TemporaryDirectory() as d:
            for name, text in files.items():
                with open(os.path.join(d, name), 'w', encoding='utf-8') as f:
                    f.write(text)
            return subprocess.run([sys.executable, TOOL, d, '--show'], capture_output=True, text=True)

    def test_tool(self):
        p = self.run_tool({'ps_AAAA.vk.glsl': CLEAN, 'ps_CCCC.vk.glsl': HARMLESS})
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('2 shaders: 0 with a fetch', p.stdout)
        self.assertIn('(1 have such a temporary at all)', p.stdout)
        p = self.run_tool({'ps_AAAA.vk.glsl': CLEAN, 'ps_BBBB.vk.glsl': BAD})
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn('ps_BBBB.vk.glsl: 2 bound textures before, 1 now', p.stdout)
        self.assertIn('no longer bound', p.stdout)
        self.assertIn('0:384', p.stdout)
        self.assertIn(f'_10 declared as {t.T0}, written again', p.stdout)
        self.assertIn(f'_10 = {t.T1};', p.stdout)
        self.assertNotIn('ps_AAAA', p.stdout)

    def test_no_input(self):
        with tempfile.TemporaryDirectory() as d:
            p = subprocess.run([sys.executable, TOOL, os.path.join(d, 'none')], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2)
            p = subprocess.run([sys.executable, TOOL, d], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2)


if __name__ == '__main__':
    unittest.main(verbosity=1)
