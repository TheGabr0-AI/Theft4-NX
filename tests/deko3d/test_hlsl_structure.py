#!/usr/bin/env python3
"""Offline test of run 47 (devlog section S): tools/deko3d/hlsl_structure.py.

    python tests/deko3d/test_hlsl_structure.py [shader.hlsl ...]

1. Synthetic state machines in XenosRecomp's form (forward conditional jumps, if/else with the jump over the else,
   nested, a do-while with a conditional latch, break at the end): the structured program executes the same sequence
   of case bodies as the machine for 2,000 random condition streams each.
2. Shapes that must be left alone (a jump out of a loop, an unconditional backward jump, a jump into a loop): unchanged.
3. Each shader given on the command line (game data: not in the repository; the deferred light-volume shaders of run 46):
   structured, and trace-equivalent to its machine over 2,000 random condition streams; the text outside the machine is
   unchanged.
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'deko3d'))
import hlsl_structure as hs  # noqa: E402


def machine_text(cases):
    """cases: list of (statements, tail) with tail None, ('jump', N), ('cjump', cond, N) or 'break'."""
    out = ['float4 shaderMain()', '{', '\tuint pc = 0;', '\twhile (true)', '\t{', '\t\tswitch (pc)', '\t\t{']
    for i, (stmts, tail) in enumerate(cases):
        out.append(f'\t\tcase {i}:')
        out += [f'\t\t\t{s}' for s in stmts]
        if tail == 'break':
            out.append('\t\t\tbreak;')
        elif tail and tail[0] == 'jump':
            out += [f'\t\t\tpc = {tail[1]};', '\t\t\tcontinue;']
        elif tail and tail[0] == 'cjump':
            out += [f'\t\t\tif ({tail[1]})', '\t\t\t{', f'\t\t\t\tpc = {tail[2]};', '\t\t\t\tcontinue;', '\t\t\t}']
    out += ['\t\t}', '\t\tbreak;', '\t}', '\treturn r0;', '}', '']
    return '\n'.join(out)


def equivalent(text, rounds=2000, seed=1):
    parts = hs.split(text)
    cases = hs.parse_cases(parts[2])
    nodes = hs.build(cases)
    rng = random.Random(seed)
    for _ in range(rounds):
        stream_seed = rng.random()
        counts = {}

        def make_oracle():
            r = random.Random(stream_seed)
            seen = {}

            def oracle(cond):
                # loops stop after a few rounds so that the machine ends
                seen[cond] = seen.get(cond, 0) + 1
                return r.random() < (0.5 if seen[cond] < 20 else 0.0)
            return oracle
        a = hs.run_machine(cases, make_oracle())
        b = []
        hs.run_nodes(nodes, cases, make_oracle(), b)
        if a != b:
            return False, (a[:40], b[:40])
    return True, None


def body(i):
    return [f'r{i % 8}.x = r{i % 8}.x + {i}.0;']


def main():
    failures = []
    # 1. synthetic machines
    synthetic = {
        'straight': [(body(0), None), (body(1), None), (body(2), 'break')],
        'if': [(body(0), ('cjump', 'p0', 3)), (body(1), None), (body(2), None), (body(3), 'break')],
        'if-else': [(body(0), ('cjump', 'p0', 3)), (body(1), None), (body(2), ('jump', 5)), (body(3), None), (body(4), None),
                    (body(5), 'break')],
        'nested (run 46 shape)': [(body(0), None), ([], ('cjump', '!p0', 10)), (body(2), None), ([], ('cjump', '!p0', 8)),
                                  (body(4), None), (body(5), None), (body(6), None), ([], ('jump', 9)), (body(8), None),
                                  ([], ('jump', 15)), (body(10), None), (body(11), None), (body(12), None), (body(13), None),
                                  (body(14), None), (body(15), 'break')],
        'loop': [(['aL = 0;'], None), (body(1), ('cjump', '!p1', 3)), (body(2), None), (['++aL;'], ('cjump', 'aL < 12', 1)),
                 (body(4), 'break')],
        'loop with if-else inside': [(['aL = 0;'], None), (body(1), ('cjump', '!p0', 4)), (body(2), None), ([], ('jump', 5)),
                                     (body(4), None), (body(5), ('cjump', '!p1', 7)), (body(6), None),
                                     (['++aL;'], ('cjump', 'aL < 3', 1)), (body(8), None), (body(9), 'break'), ([], 'break')],
    }
    for name, cases in synthetic.items():
        text = machine_text(cases)
        new, message = hs.structure(text)
        ok, detail = (False, message) if not message.startswith('structured') else equivalent(text)
        print(f'synthetic {name}: {message}; trace-equivalent {ok}')
        if not ok:
            failures.append(f'{name}: {detail}')
    # 2. shapes left alone
    refused = {
        'jump out of a loop': [(['aL = 0;'], None), (body(1), ('cjump', 'p0', 4)), (['++aL;'], ('cjump', 'aL < 3', 1)),
                               (body(3), None), (body(4), 'break')],
        'unconditional backward jump': [(body(0), None), (body(1), ('jump', 0)), (body(2), 'break')],
        'jump into a loop': [(body(0), ('cjump', 'p0', 2)), (['aL = 0;'], None), (body(2), None),
                             (['++aL;'], ('cjump', 'aL < 3', 1)), (body(4), 'break')],
    }
    for name, cases in refused.items():
        text = machine_text(cases)
        new, message = hs.structure(text)
        print(f'refused {name}: {message}')
        if new != text:
            failures.append(f'{name}: changed although it should not be')
    # 3. real shaders
    for path in sys.argv[1:]:
        text = open(path, encoding='utf-8').read()
        new, message = hs.structure(text)
        ok = message.startswith('structured')
        eq = False
        if ok:
            eq, detail = equivalent(text)
            before, _, _, after = hs.split(text)
            outside = new.startswith(before) and new.endswith(after)
        print(f'{os.path.basename(path)}: {message}; trace-equivalent {eq}; text outside the machine kept {ok and outside}')
        if not (ok and eq and outside):
            failures.append(os.path.basename(path))
    if failures:
        print('FAILED')
        for f in failures:
            print('  ' + str(f))
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
