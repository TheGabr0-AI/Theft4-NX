#!/usr/bin/env python3
"""Theft4-NX: every guest address in the Ultimate Base's app and SDK code, against GTA IV's hook bindings (doc 20, U4).

The base binds its hooks to Beautiful Katamari's functions. On GTA IV's generated code a Katamari address that is
still hooked replaces whatever GTA IV function happens to start there. This census lists:

  1. every hook definition in the code (comments stripped): the macro, the address, the host function, the file and
     line, and whether it is live (BANJO_RENUT_HOOK* only with KATAMARI_RENUT_HOOKS, REX_HOOK_RAW / PPC_FUNC / ...
     always; the *_UNHOOKED_* forms never);
  2. every other 82xxxxxx in code (data addresses, calls through __imp__, constants): what tools/direct_calls.py and
     leaf_dead_stores.py count as "named", and what code may read or call at run time;
  3. the check against tools/ultimate/gta4_hook_bindings.csv (address, name, source in docs/switch-port/18): each
     binding is a function start in the generated code (generated/renut_init.cpp) and has exactly one live hook; no
     live hook sits at an address the table does not name.

    python tools/ultimate/hook_census.py [--ultimate ultimate] [--bindings tools/ultimate/gta4_hook_bindings.csv]
                                         [--report out.txt] [--strict]

--strict: exit 1 when a check fails (a live hook outside the table, a binding without its hook, a binding that is not
a function start, or an address in code that is neither a binding nor listed as reviewed in the table's
"reviewed" rows).
"""
import argparse
import csv
import os
import re
import sys

CODE_SUFFIXES = ('.cpp', '.cc', '.c', '.h', '.hpp', '.inc')
HOOK_MACROS = {
    # macro: liveness
    'BANJO_RENUT_HOOK': 'katamari_renut_hooks',
    'BANJO_RENUT_HOOK_SHARED': 'never',  # only renut_native_sub_X, for another renderer's hook: no hook at sub_X
    'BANJO_RENUT_HOOK_RAW': 'katamari_renut_hooks',
    'BANJO_RENUT_HOOK_RAW_SHARED': 'never',  # the same
    'BANJO_RENUT_UNHOOKED_HOOK': 'never',
    'BANJO_RENUT_UNHOOKED_HOOK_SHARED': 'never',
    'BANJO_RENUT_UNHOOKED_HOOK_RAW': 'never',
    'BANJO_RENUT_UNHOOKED_HOOK_RAW_SHARED': 'never',
    'REX_HOOK_RAW': 'always',
    'REX_HOOK': 'always',
    'REX_STUB': 'always',
    'PPC_HOOK': 'always',
    'PPC_FUNC': 'always',
    'REX_FUNC': 'always',
    'BANJO_HOT_HOOK_EX': 'always',
}
HOOK_RE = re.compile(r'\b(' + '|'.join(sorted(HOOK_MACROS, key=len, reverse=True)) +
                     r')\s*\(\s*(?:__imp__)?sub_([0-9A-Fa-f]{8})\b\s*(?:,\s*([A-Za-z_][\w:]*))?')
ADDR_RE = re.compile(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])')


def strip_comments(text):
    # Keep line numbers: replace comments by the same number of newlines.
    def blank(m):
        return '\n' * m.group(0).count('\n')
    text = re.sub(r'/\*.*?\*/', blank, text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)  # strings: log messages name addresses for the reader
    return text


def defined_macros(ult):
    """Every macro name a source #defines or a CMake file passes as a definition (any value): what an #if can see."""
    names = set()
    for root in (os.path.join(ult, 'app'), os.path.join(ult, 'sdk', 'src'), os.path.join(ult, 'sdk', 'include'),
                 os.path.join(ult, 'sdk', 'cmake')):
        for d, _, fs in os.walk(root):
            if os.sep + 'generated' in d or os.sep + 'thirdparty' in d or os.sep + 'out' in d:
                continue
            for f in fs:
                p = os.path.join(d, f)
                if f.endswith(CODE_SUFFIXES):
                    for m in re.finditer(r'#\s*define\s+(\w+)', open(p, encoding='utf-8', errors='ignore').read()):
                        names.add(m.group(1))
                elif f.endswith('.cmake') or f == 'CMakeLists.txt':
                    text = open(p, encoding='utf-8', errors='ignore').read()
                    for m in re.finditer(r'(?:compile_definitions|COMPILE_DEFINITIONS)\s*\(?([^)]*)', text):
                        for w in re.findall(r'\b([A-Z][A-Z0-9_]{2,})(?:=|\b)', m.group(1)):
                            names.add(w)
                    for m in re.finditer(r'-D([A-Z][A-Z0-9_]+)', text):
                        names.add(m.group(1))
    return names


DEFINED = set()


def active_lines(text):
    """Per line: False inside an #if / #ifdef on a macro nothing defines (it is 0), True elsewhere; complex conditions
    count as true (conservative)."""
    stack, out = [], []
    for line in text.split('\n'):
        s = line.strip()
        m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)', s)
        if m:
            kw, cond = m.group(1), m.group(2).strip()
            if kw in ('if', 'ifdef', 'ifndef'):
                val = None
                n = re.fullmatch(r'(!?)\s*(?:defined\s*\(?\s*)?(\w+)\s*\)?', cond)
                if kw == 'ifdef':
                    val = cond in DEFINED
                elif kw == 'ifndef':
                    val = cond not in DEFINED
                elif n and not n.group(2).isdigit():
                    val = n.group(2) in DEFINED or None  # a defined macro's value is unknown: conservative
                    if n.group(2) not in DEFINED:
                        val = n.group(1) == '!'
                elif cond == '0':
                    val = False
                stack.append(val)
            elif kw == 'elif' and stack:
                stack[-1] = None
            elif kw == 'else' and stack:
                stack[-1] = None if stack[-1] is None else not stack[-1]
            elif kw == 'endif' and stack:
                stack.pop()
        out.append(all(v is not False for v in stack))
    return out


def scan(roots):
    hooks, others = [], []
    for root in roots:
        for d, _, fs in os.walk(root):
            for f in fs:
                if not f.endswith(CODE_SUFFIXES):
                    continue
                p = os.path.join(d, f)
                try:
                    text = strip_comments(open(p, encoding='utf-8', errors='ignore').read())
                except OSError:
                    continue
                active = active_lines(text)
                for n, line in enumerate(text.split('\n'), 1):
                    if not active[n - 1]:
                        continue
                    hit = set()
                    for m in HOOK_RE.finditer(line):
                        addr = m.group(2).upper()
                        hit.add(addr)
                        hooks.append({'addr': addr, 'macro': m.group(1), 'live': HOOK_MACROS[m.group(1)],
                                      'host': m.group(3) or '', 'file': p, 'line': n})
                    for m in ADDR_RE.finditer(line):
                        addr = m.group(1).upper()
                        if addr not in hit:
                            others.append({'addr': addr, 'file': p, 'line': n, 'text': line.strip()[:140]})
    return hooks, others


def compiled_files(ult):
    """Sources the app's CMake names (app/CMakeLists.txt, app/cmake/*.cmake, src/renut_engine/gpu/gpu.cmake), every SDK
    source, and the headers / .inc files those include (by file name, to a fixed point). Conservative: a source named
    under a platform condition counts as compiled."""
    app = os.path.join(ult, 'app')
    cmakes = [os.path.join(app, 'CMakeLists.txt'), os.path.join(app, 'src', 'renut_engine', 'gpu', 'gpu.cmake')]
    cmakes += [os.path.join(app, 'cmake', f) for f in os.listdir(os.path.join(app, 'cmake')) if f.endswith('.cmake')]
    rels = set()
    for c in cmakes:
        text = re.sub(r'(?m)#[^\n]*', ' ', open(c, encoding='utf-8', errors='ignore').read())
        for m in re.finditer(r'\$\{RENUT_GPU_DIR\}/([\w/.+-]+\.(?:cpp|c|cc))', text):
            rels.add('src/renut_engine/gpu/' + m.group(1))
        for m in re.finditer(r'(?<![\w/])(src/[\w/.+-]+\.(?:cpp|c|cc))', text):
            rels.add(m.group(1))
    compiled = set()
    for d, _, fs in os.walk(os.path.join(app, 'src')):
        for f in fs:
            p = os.path.join(d, f)
            if os.path.relpath(p, app).replace('\\', '/') in rels:
                compiled.add(os.path.normcase(p))
    for d, _, fs in os.walk(os.path.join(ult, 'sdk', 'src')):
        for f in fs:
            if f.endswith(('.cpp', '.c', '.cc')):
                compiled.add(os.path.normcase(os.path.join(d, f)))
    headers = {}
    for root in (os.path.join(app, 'src'), os.path.join(ult, 'sdk', 'include'), os.path.join(ult, 'sdk', 'src')):
        for d, _, fs in os.walk(root):
            for f in fs:
                if f.endswith(('.h', '.hpp', '.inc')):
                    headers.setdefault(f, []).append(os.path.normcase(os.path.join(d, f)))
    todo = list(compiled)
    while todo:
        p = todo.pop()
        try:
            text = open(p, encoding='utf-8', errors='ignore').read()
        except OSError:
            continue
        for m in re.finditer(r'#\s*include\s*[<"]([^>"]+)[>"]', text):
            for h in headers.get(os.path.basename(m.group(1)), []):
                if h not in compiled:
                    compiled.add(h)
                    todo.append(h)
    return compiled


def function_starts(gen):
    starts = set()
    for f in os.listdir(gen):
        if f.endswith('_init.cpp') or (f.startswith('renut_init') and f.endswith('.cpp')):
            for m in re.finditer(r'\{\s*0x(82[0-9A-Fa-f]{6})\s*,', open(os.path.join(gen, f), encoding='utf-8').read()):
                starts.add(m.group(1).upper())
    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ultimate', default='ultimate')
    ap.add_argument('--bindings', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gta4_hook_bindings.csv'))
    ap.add_argument('--report')
    ap.add_argument('--strict', action='store_true')
    a = ap.parse_args()

    ult = os.path.abspath(a.ultimate)
    DEFINED.update(defined_macros(ult))
    hooks, others = scan([os.path.join(ult, 'app', 'src'), os.path.join(ult, 'sdk', 'src'), os.path.join(ult, 'sdk', 'include')])
    starts = function_starts(os.path.join(ult, 'app', 'generated'))
    compiled = compiled_files(ult)
    built = lambda p: os.path.normcase(p) in compiled

    bindings, reviewed, reviewed_files = {}, {}, {}
    if os.path.isfile(a.bindings):
        for row in csv.DictReader(l for l in open(a.bindings, encoding='utf-8') if not l.startswith('#')):
            kind = row['kind'].strip()
            if kind == 'reviewed-file':  # every non-hook address in that file, for the reason given
                reviewed_files[row['address'].strip().replace('\\', '/')] = row
                continue
            addr = row['address'].strip().upper().removeprefix('0X')
            (reviewed if kind == 'reviewed' else bindings)[addr] = row

    out, problems = [], []
    rel = lambda p: os.path.relpath(p, os.path.dirname(ult)).replace('\\', '/')
    out.append(f'function starts in the generated code: {len(starts)}')
    live = [h for h in hooks if h['live'] != 'never' and built(h['file'])]
    out.append(f'hook definitions: {len(hooks)} ({len(live)} live in compiled files: with KATAMARI_RENUT_HOOKS or '
               f'always)')
    out.append('')
    out.append('== hook definitions (address, macro, liveness, compiled?, host, GTA IV function start?, binding, place)')
    for h in sorted(hooks, key=lambda h: (h['addr'], h['file'], h['line'])):
        b = bindings.get(h['addr'])
        tag = b['name'] if b else ('REVIEWED: ' + reviewed[h['addr']]['name'] if h['addr'] in reviewed else '-')
        out.append(f"{h['addr']} {h['macro']:<36} {h['live']:<21} {'built' if built(h['file']) else 'not-built':<9} "
                   f"{h['host']:<34} {'start' if h['addr'] in starts else 'no-start':<8} {tag:<34} "
                   f"{rel(h['file'])}:{h['line']}")
        if h['live'] != 'never' and built(h['file']) and not b:
            problems.append(f"live hook at {h['addr']} not in the GTA IV bindings: {rel(h['file'])}:{h['line']}")
    out.append('')
    out.append('== bindings (address, name, function start, live hooks)')
    for addr, b in sorted(bindings.items()):
        n = [h for h in live if h['addr'] == addr]
        out.append(f"{addr} {b['name']:<34} {'start' if addr in starts else 'NO-START':<8} {len(n)} "
                   + ' '.join(f"{rel(h['file'])}:{h['line']}" for h in n))
        if addr not in starts:
            problems.append(f"binding {addr} {b['name']} is not a function start in the generated code")
        if len(n) != 1:
            problems.append(f"binding {addr} {b['name']} has {len(n)} live hooks")
    out.append('')
    out.append('== other guest addresses in code (address, GTA IV function start?, reviewed?, place, text)')
    for o in sorted(others, key=lambda o: (o['addr'], o['file'], o['line'])):
        tag = 'binding' if o['addr'] in bindings else ('reviewed' if o['addr'] in reviewed else (
            'rev-file' if rel(o['file']) in reviewed_files else '-'))
        out.append(f"{o['addr']} {'start' if o['addr'] in starts else 'no-start':<8} {tag:<8} "
                   f"{'built' if built(o['file']) else 'not-built':<9} {rel(o['file'])}:{o['line']}  {o['text']}")
        if tag == '-' and built(o['file']):
            problems.append(f"address {o['addr']} in code, not reviewed: {rel(o['file'])}:{o['line']}")
    out.append('')
    by_file = {}
    for o in others:
        k = rel(o['file']) + ('' if built(o['file']) else '  (not built)')
        by_file[k] = by_file.get(k, 0) + 1
    out.append('== other addresses by file')
    for f, c in sorted(by_file.items(), key=lambda x: -x[1]):
        out.append(f'{c:6} {f}')
    out.append('')
    out.append(f'problems: {len(problems)}')
    out += ['  ' + p for p in problems[:400]]
    text = '\n'.join(out) + '\n'
    if a.report:
        open(a.report, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'hook_census: {len(hooks)} hook definitions ({len(live)} live), {len(others)} other addresses in code, '
          f'{len(bindings)} bindings, {len(reviewed)} reviewed addresses, {len(reviewed_files)} reviewed files, '
          f'{len(starts)} function starts; problems {len(problems)}'
          + (f' -> {a.report}' if a.report else ''))
    if a.strict and problems:
        sys.exit(1)


if __name__ == '__main__':
    main()
