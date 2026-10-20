#!/usr/bin/env python3
"""Copies of the native renderer's hooks with the v8 addresses moved to the European base
(docs/switch-port/15-kabba-port.md, step 3).

The hooks (gta4-recomp/src) name v8 (USA, title update 8) functions, globals and code addresses.
This script writes a copy of the source folder in which every address the census
(tools/gta4_native_eu_census.py, out/gta4-native-census/census.tsv) found ("trovato") is replaced by
its European one; the originals are not touched. What the census did not find:

  - a function: every sub_X becomes gta4eu_unresolved_X and every __imp__sub_X
    gta4eu_unresolved_imp_X. A hook on it is then a function nobody calls (the game keeps its own
    code there); a call to it reaches a stub (written to gta4eu_unresolved_stubs.cpp) that logs once
    and returns 0 in r3. The list is printed;
  - a global or a code address: left as it is and listed. Return-address checks then never match
    and the hook takes its retail path; a global read reads the wrong word (the list says where).

MANUAL holds addresses found by hand (with how), applied like the census results.

Usage:
  python tools/gta4_native_eu_translate.py [--census out/gta4-native-census/census.tsv]
      [--src glue/rexglue-sdk-main/gta4-recomp/src] [--out out/gta4-native-eu/src]
"""
import argparse
import csv
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gta4_native_eu_census as census  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# v8 -> EU found by hand. The font owner slots: v8 sub_821F6098 stores them at 0x82B999C0 + 476,
# 976, 1476 (font objects of 500 bytes, owner at +476). In the European base the font objects
# start at 0x82B8DE78 (sub_821DE390: addi r31 = 0x82B8DE78, then +500 per font), and EU sub_821DBDC0
# and sub_821DE770 (v8 sub_821F1EE0 and sub_821F5788, same instruction index) both build
# 0x82B8E054 = 0x82B8DE78 + 476 where v8 builds 0x82B99B9C. font3 and font2: same stride, to be
# verified on screen.
MANUAL = {
    0x82B99B9C: (0x82B8E054, 'font1 owner, two functions agree'),
    0x82B99D90: (0x82B8E248, 'font3 owner, stride 500: da verificare'),
    0x82B99F84: (0x82B8E43C, 'font2 owner, stride 500: da verificare'),
}

TOKEN = re.compile(r'(__imp__sub_|(?<![\w])sub_)?(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
TEXT_EXT = ('.cpp', '.h', '.inc', '.hpp', '.c', '.mm')
# The native renderer's hooks and what they include (tools/pc-eu-base, THEFT4_GTA4_NATIVE).
COMPILED = ('gta4_native_hooks.cpp', 'gta4_phone_trace.inc', 'gta4_tv_trace.inc',
            'gta4_fire_escape_trace.inc', 'gta4_bulb_source_trace.inc', 'gta4_help_trace_hooks.inc',
            'gta4_fade_trace_hooks.inc', 'gta4_phone_trace_hooks.inc', 'gta4_aspect_hooks.cpp',
            'gta4_font_selection_trace.cpp', 'gta4_presentation_options.cpp',
            'gta4_temporal_hooks.inc', 'theft4_lra_app_stubs.cpp')
NAMES_HEADER = 'gta4eu_names.h'
# gta4_app.cpp is the LibertyRecomp application (macOS): only its cvar definitions are needed (the
# hooks and the plugin read them), written to APP_CVARS with the one controller function the hooks
# call, reduced to reading its cvar.
APP_CVARS = 'gta4eu_app_cvars.cpp'


def app_cvar_definitions(path):
    """The REXCVAR_DEFINE_* statements of a file, each up to its closing ');'."""
    text = open(path, encoding='utf-8').read()
    out = []
    for m in re.finditer(r'^REXCVAR_DEFINE_[A-Z0-9]+\(', text, re.M):
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == '(':
                depth += 1
            elif text[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            elif text[i] == '"':
                i += 1
                while text[i] != '"':
                    i += 2 if text[i] == '\\' else 1
            i += 1
        out.append(text[m.start():i + 1] + ';')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--census', default=os.path.join(ROOT, 'out', 'gta4-native-census', 'census.tsv'))
    ap.add_argument('--src', default=os.path.join(ROOT, 'glue', 'rexglue-sdk-main', 'gta4-recomp', 'src'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'gta4-native-eu', 'src'))
    ap.add_argument('--compiled', nargs='*', default=list(COMPILED),
                    help='translation units that are built (their unresolved hooks need no stub)')
    args = ap.parse_args()
    flirt = census.flirt_names()

    mapping, unresolved_funcs, unresolved_other = {}, set(), {}
    with open(args.census, encoding='utf-8') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            a = int(row['v8'], 16)
            if row['stato'] == 'trovato' and row['eu']:
                mapping[a] = int(row['eu'], 16)
            elif row['tipo'] == 'funzione':
                unresolved_funcs.add(a)
            else:
                unresolved_other[a] = '%s, %s' % (row['tipo'], row['stato'])
    for a, (e, _) in MANUAL.items():
        mapping[a] = e
        unresolved_other.pop(a, None)

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    used_unresolved, defined_unresolved, left, compiled_text = set(), set(), {}, []
    for dirpath, _, names in os.walk(args.src):
        rel = os.path.relpath(dirpath, args.src)
        os.makedirs(os.path.join(args.out, rel), exist_ok=True)
        for n in names:
            src = os.path.join(dirpath, n)
            dst = os.path.join(args.out, rel, n)
            if not n.endswith(TEXT_EXT):
                shutil.copy2(src, dst)
                continue
            text = open(src, encoding='utf-8', errors='surrogateescape').read()

            # One pass, so an EU address equal to another v8 address is never moved twice.
            def repl(m):
                prefix, hexa = m.group(1) or '', m.group(2)
                a = int(hexa, 16)
                if a in mapping:
                    e = '%08X' % mapping[a]
                    return prefix + (e.lower() if hexa != hexa.upper() else e)
                if prefix and a in unresolved_funcs:
                    used_unresolved.add(a)
                    return ('gta4eu_unresolved_imp_' if prefix == '__imp__sub_' else 'gta4eu_unresolved_') + hexa
                if a in unresolved_other or a in unresolved_funcs:
                    left.setdefault(a, set()).add(os.path.join(rel, n).replace(os.sep, '/'))
                return m.group(0)

            text = TOKEN.sub(repl, text)
            if n in args.compiled:
                compiled_text.append(text)
                for m in re.finditer(r'void (gta4eu_unresolved_(?:imp_)?[0-9A-F]{8})\(PPCContext', text):
                    defined_unresolved.add(m.group(1))
            if n.endswith('.cpp'):
                text = '#include "%s"  // gta4_native_eu_translate.py\n' % NAMES_HEADER + text
            open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(text)

    # The v8 FLIRT names (gta4_config.toml) the hooks use: the European code has only sub_X.
    with open(os.path.join(args.out, NAMES_HEADER), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/gta4_native_eu_translate.py.\n#pragma once\n'
                '#include "gta4_init.h"\n\n')
        # Only the names the built files use: some (ReadFile) are also Windows API names.
        used_text = '\n'.join(compiled_text)
        for name, a in sorted(flirt.items()):
            if a in mapping and re.search(r'\b(?:__imp__)?%s\b' % name, used_text):
                f.write('#define %s sub_%08X\n#define __imp__%s __imp__sub_%08X\n' %
                        (name, mapping[a], name, mapping[a]))
        f.write('\n')
        for a in sorted(used_unresolved):
            for name in ('gta4eu_unresolved_%08X' % a, 'gta4eu_unresolved_imp_%08X' % a):
                f.write('extern "C" void %s(PPCContext& ctx, uint8_t* base);\n' % name)

    with open(os.path.join(args.out, APP_CVARS), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/gta4_native_eu_translate.py from gta4_app.cpp: its cvars only.\n'
                '#include <rex/cvar.h>\n#include <rex/platform.h>\n'
                '#include <rex/graphics/gta4_native/anti_aliasing_policy.h>\n\n')
        f.write('\n'.join(app_cvar_definitions(os.path.join(args.src, 'gta4_app.cpp'))) + '\n\n')
        f.write('namespace rex::graphics::gta4_native {\n'
                '// gta4_app.cpp runs a frontend controller; here the configured value is the active one.\n'
                'AntiAliasingMode GetActiveAntiAliasingMode() {\n'
                '  const auto mode = ParseAntiAliasingMode(REXCVAR_GET(gta4_native_anti_aliasing));\n'
                '  return mode ? *mode : AntiAliasingMode::kOff;\n'
                '}\n}  // namespace rex::graphics::gta4_native\n')

    stubs = os.path.join(args.out, 'gta4eu_unresolved_stubs.cpp')
    with open(stubs, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/gta4_native_eu_translate.py: v8 functions the census did not find\n'
                '// in the European base. A call logs once and returns 0.\n'
                '#include <atomic>\n#include <rex/logging.h>\n#include "%s"\n\n' % NAMES_HEADER)
        for a in sorted(used_unresolved):
            for name in ('gta4eu_unresolved_%08X' % a, 'gta4eu_unresolved_imp_%08X' % a):
                if name in defined_unresolved:
                    continue
                f.write('extern "C" void %s(PPCContext& ctx, uint8_t*) {\n'
                        '  static std::atomic<bool> logged{false};\n'
                        '  if (!logged.exchange(true))\n'
                        '    REXLOG_WARN("gta4-native-eu: v8 %08X has no European address, call skipped");\n'
                        '  ctx.r3.u64 = 0;\n}\n' % (name, a))
    print('mapped %d addresses (%d by hand); unresolved functions used: %d' %
          (len(mapping), len(MANUAL), len(used_unresolved)))
    for a in sorted(used_unresolved):
        print('  function %08X' % a)
    print('left untranslated (globals/code addresses): %d' % len(left))
    for a in sorted(left):
        print('  %08X %-28s %s' % (a, unresolved_other.get(a, 'funzione'), ','.join(sorted(left[a]))))
    print('written %s' % args.out)


if __name__ == '__main__':
    main()
