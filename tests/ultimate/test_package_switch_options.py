#!/usr/bin/env python3
"""Test of tools/ultimate/package_switch.py's option checks (option_problems): LTO, PGO and the flavours.

    python tests/ultimate/test_package_switch_options.py

No build folder is needed: the function takes CMakeCache.txt's values.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ultimate'))
import package_switch as ps  # noqa: E402

BASE = {'REXGLUE_SWITCH_NVK': 'OFF', 'PLUME_SWITCH_NVK_LINK': 'OFF', 'KATAMARI_BUILTIN_CONFIG': 'ON',
        'BANJO_RENUT_RELEASE_SHADERS': 'OFF', 'PLUME_DEKO3D': 'ON', 'REXGLUE_PLATFORM_SWITCH': 'ON'}
EVERYDAY = dict(BASE, SASRT_HOT_GUARD_FORMS='OFF', SASRT_HOT_IN_PLACE='ON')
GUARD = dict(BASE, SASRT_HOT_GUARD_FORMS='ON', SASRT_HOT_IN_PLACE='OFF')


def problems(cache, flavour='everyday', **kw):
    return ps.option_problems(cache, flavour, **kw)[0]


class Options(unittest.TestCase):
    def test_the_builds_packaged_so_far_still_pass(self):
        self.assertEqual(problems(EVERYDAY), [])
        self.assertEqual(problems(GUARD, 'guard'), [])
        self.assertEqual(problems(dict(EVERYDAY, BANJO_SWITCH_LTO='OFF', BANJO_PGO='')), [])

    def test_the_old_checks_are_kept(self):
        self.assertEqual(len(problems(dict(EVERYDAY, REXGLUE_SWITCH_NVK='ON'))), 1)
        self.assertEqual(len(problems(dict(EVERYDAY, BANJO_RENUT_RELEASE_SHADERS='ON'))), 1)
        self.assertEqual(len(problems(EVERYDAY, 'guard')), 2)     # the everyday options under the guard's name
        self.assertEqual(problems(EVERYDAY, 'release'), ["unknown flavour 'release'"])
        self.assertEqual(len(problems({})), len(EVERYDAY))

    def test_an_instrumented_build_is_refused(self):
        c = dict(EVERYDAY, BANJO_PGO='generate')
        self.assertIn('instrumented', problems(c)[0])
        self.assertEqual(problems(c, allow_pgo_generate=True), [])

    def test_a_profile_needs_lto_and_not_the_guard(self):
        self.assertIn('without BANJO_SWITCH_LTO', problems(dict(EVERYDAY, BANJO_PGO='use'))[0])
        self.assertEqual(problems(dict(EVERYDAY, BANJO_PGO='use', BANJO_SWITCH_LTO='ON')), [])
        self.assertIn('guard forms', ' '.join(problems(dict(GUARD, BANJO_PGO='use', BANJO_SWITCH_LTO='ON'), 'guard')))
        self.assertIn('not empty, generate or use', problems(dict(EVERYDAY, BANJO_PGO='yes'))[0])

    def test_lto_as_asked(self):
        on, off = dict(EVERYDAY, BANJO_SWITCH_LTO='ON'), dict(EVERYDAY, BANJO_SWITCH_LTO='OFF')
        self.assertEqual(problems(on, lto='on'), [])
        self.assertEqual(problems(off, lto='off'), [])
        self.assertEqual(problems(EVERYDAY, lto='off'), [])           # not in the cache: off
        self.assertIn('--lto on', problems(off, lto='on')[0])
        self.assertIn('--lto on', problems(EVERYDAY, lto='on')[0])
        self.assertIn('--lto off', problems(on, lto='off')[0])
        self.assertEqual(problems(on), [])                            # not asked: either is packaged
        for v in ('ON', 'on', '1', 'TRUE'):
            self.assertTrue(ps.is_on(v))
        for v in ('OFF', '', None, '0', 'FALSE'):
            self.assertFalse(ps.is_on(v))


if __name__ == '__main__':
    unittest.main(verbosity=1)
