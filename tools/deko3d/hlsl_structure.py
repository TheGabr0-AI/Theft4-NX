#!/usr/bin/env python3
"""XenosRecomp's `while (true) { switch (pc) { ... } }` state machine as structured HLSL (docs/switch-port/16-deko3d-devlog.md, run 47).

XenosRecomp writes a shader with jumps or loops as one loop around a switch on a program counter: every case falls into the
next, a jump is `pc = N; continue;` (conditional inside `if (...) { }`). DXC turns that into SSA where all 32 float4 registers
are live across every case of the loop; uam then gives the vertex shader 255 GPRs, up to 92,672 bytes of scratch per warp and
745 KB of code (run 46: the deferred light-volume vertex shaders 0872615BD9E7930D and 22F0AB2B742F2053 take 430-910 ms of GPU
for ONE draw, and are the cut-scene stall and the in-game freeze).

The machine GTA IV's shaders produce is an if/else and do-while program written with jumps:
  - a conditional forward jump at the end of case k to T: the cases k+1 .. T-1 run when the condition is false;
    when case T-1 ends with an unconditional jump to E > T, it is an if/else: T .. E-1 is the else;
  - a conditional backward jump at the end of case L to H <= L: the cases H .. L are a do-while on that condition;
  - `break;` ends the shader (only in the last cases).
This pass rebuilds that program; any other shape (a jump that leaves its region, a jump into a loop, `pc` used otherwise)
leaves the shader as it is. The bodies of the cases are copied verbatim, in their order.

    python tools/deko3d/hlsl_structure.py <in.hlsl> <out.hlsl>   (prints what it did)

Tested by tests/deko3d/test_hlsl_structure.py: trace equivalence (the sequence of case bodies executed) between the state
machine and the structured program over random condition outcomes, on synthetic machines and on the given shaders.
"""
import re
import sys

HEAD_RE = re.compile(r'\n(?P<i>\t*)uint pc = 0;\n(?P=i)while \(true\)\n(?P=i)\{\n(?P=i)\tswitch \(pc\)\n(?P=i)\t\{\n')
CASE_RE = re.compile(r'^\t*case (\d+):\s*$')
JUMP_UNCOND_RE = re.compile(r'^\s*pc = (\d+);\s*$')
CONTINUE_RE = re.compile(r'^\s*continue;\s*$')
IF_RE = re.compile(r'^\s*if \((.*)\)\s*$')


class Unstructurable(Exception):
    pass


class Case:
    def __init__(self, number, body, tail, target=None, cond=None):
        self.number = number
        self.body = body      # statement lines, without the tail
        self.tail = tail      # 'fall', 'jump' (unconditional), 'cjump' (conditional), 'break'
        self.target = target
        self.cond = cond


def parse_cases(block_lines):
    """The cases of the switch body: [Case], in order."""
    cases = []
    current = None
    for line in block_lines:
        m = CASE_RE.match(line)
        if m:
            current = [int(m.group(1)), []]
            cases.append(current)
        elif current is None:
            if line.strip():
                raise Unstructurable('text before the first case')
        else:
            current[1].append(line)
    out = []
    for i, (number, lines) in enumerate(cases):
        if number != i:
            raise Unstructurable('cases not numbered 0..n-1 in order')
        while lines and not lines[-1].strip():
            lines.pop()
        s = [l.strip() for l in lines]
        tail, target, cond, body = 'fall', None, None, lines
        if len(s) >= 5 and IF_RE.match(lines[-5]) and s[-4] == '{' and JUMP_UNCOND_RE.match(lines[-3]) and s[-2] == 'continue;' \
                and s[-1] == '}':
            tail, target, cond, body = 'cjump', int(JUMP_UNCOND_RE.match(lines[-3]).group(1)), IF_RE.match(lines[-5]).group(1), lines[:-5]
        elif len(s) >= 2 and JUMP_UNCOND_RE.match(lines[-2]) and s[-1] == 'continue;':
            tail, target, body = 'jump', int(JUMP_UNCOND_RE.match(lines[-2]).group(1)), lines[:-2]
        elif s and s[-1] == 'break;':
            # XenosRecomp sometimes writes two (`break; break;` in the last case)
            n_breaks = len(s) - len(list(__import__('itertools').dropwhile(lambda x: x == 'break;', reversed(s))))
            tail, body = 'break', lines[:-n_breaks]
        for l in body:
            if re.search(r'\bpc\b', l) or CONTINUE_RE.match(l) or l.strip() == 'break;' or re.search(r'\bcase\b|\bswitch\b', l):
                raise Unstructurable(f'case {number}: pc, continue, break or a switch inside a body')
        out.append(Case(number, body, tail, target, cond))
    return out


# ---- the structured program: a list of nodes
# ('body', k)                         the statements of case k
# ('if', cond, then_nodes, else_nodes)  `if (cond)` (else_nodes may be empty)
# ('do', nodes, cond)                 do { nodes } while (cond)
# ('exit',)                           the shader's end (`break` of the machine)

def build(cases):
    n = len(cases)
    loops = {}  # head -> latch
    for c in cases:
        if c.tail in ('jump', 'cjump') and c.target > n:
            raise Unstructurable(f'jump past the end at case {c.number}')
        if c.tail in ('jump', 'cjump') and c.target <= c.number:
            if c.tail != 'cjump':
                raise Unstructurable(f'unconditional backward jump at case {c.number}')
            if c.target in loops:
                raise Unstructurable('two loops with one head')
            loops[c.target] = c.number
    for h, l in loops.items():
        for c in cases:
            # nothing jumps into a loop from outside it, nothing inside jumps out of it (but to the case after the latch)
            if c.tail in ('jump', 'cjump') and not (h <= c.number <= l) and h < c.target <= l:
                raise Unstructurable(f'jump {c.number}->{c.target} into loop {h}..{l}')
            if c.tail in ('jump', 'cjump') and h <= c.number <= l and c.number != l and not (h < c.target <= l + 1):
                raise Unstructurable(f'jump {c.number}->{c.target} out of loop {h}..{l}')
    building = set()  # loop heads whose body is being built
    latches = set()   # latches of those loops: their tail is the loop test

    def tail(k):
        return 'fall' if k in latches else cases[k].tail

    def region(lo, hi, drop_last_tail=False):
        nodes = []
        k = lo
        while k < hi:
            if k in loops and k not in building:
                latch = loops[k]
                if latch >= hi:
                    raise Unstructurable(f'loop {k}..{latch} leaves its region {lo}..{hi}')
                building.add(k)
                latches.add(latch)
                try:
                    inner = region(k, latch + 1)
                finally:
                    building.discard(k)
                    latches.discard(latch)
                nodes.append(('do', inner, cases[latch].cond))
                k = latch + 1
                continue
            c = cases[k]
            nodes.append(('body', k))
            t = tail(k)
            if t == 'fall':
                k += 1
            elif t == 'break':
                for j in range(k + 1, n):
                    if any(l.strip() for l in cases[j].body) and any(
                            x.tail in ('jump', 'cjump') and x.target == j for x in cases):
                        raise Unstructurable(f'break at case {k} before code that runs')
                if building:
                    raise Unstructurable(f'break inside a loop at case {k}')
                nodes.append(('exit',))
                return nodes
            elif t == 'jump':
                if k != hi - 1 or not (drop_last_tail or c.target == hi):
                    raise Unstructurable(f'unconditional jump {k}->{c.target} leaves region {lo}..{hi}')
                return nodes
            else:  # conditional forward jump
                target = c.target
                if target > hi:
                    raise Unstructurable(f'conditional jump {k}->{target} leaves region {lo}..{hi}')
                p = target - 1
                if p > k and tail(p) == 'jump' and target < cases[p].target <= hi:
                    end = cases[p].target
                    then_nodes = region(k + 1, target, drop_last_tail=True)
                    else_nodes = region(target, end)
                    nodes.append(('if', f'!({c.cond})', then_nodes, else_nodes))
                    k = end
                else:
                    nodes.append(('if', f'!({c.cond})', region(k + 1, target), []))
                    k = target
        return nodes

    return region(0, n)


def emit(nodes, cases, indent):
    out = []
    for node in nodes:
        kind = node[0]
        if kind == 'body':
            for line in cases[node[1]].body:
                out.append(indent + line.lstrip('\t') if line.strip() else line)
        elif kind == 'if':
            out.append(f'{indent}if ({node[1]})')
            out.append(indent + '{')
            out += emit(node[2], cases, indent + '\t')
            out.append(indent + '}')
            if node[3]:
                out.append(indent + 'else')
                out.append(indent + '{')
                out += emit(node[3], cases, indent + '\t')
                out.append(indent + '}')
        elif kind == 'do':
            out.append(indent + 'do')
            out.append(indent + '{')
            out += emit(node[1], cases, indent + '\t')
            out.append(f'{indent}}} while ({node[2]});')
        elif kind == 'exit':
            pass  # the structured program ends here (only the last cases may break)
    return out


def split(text):
    """(before, indent, switch body lines, after) of the state machine, or None."""
    m = HEAD_RE.search(text)
    if not m:
        return None
    indent = m.group('i')
    start = m.end()
    end_mark = f'\n{indent}\t}}\n'
    end = text.find(end_mark, start)
    if end < 0:
        raise Unstructurable('no end of the switch')
    body = text[start:end].split('\n')
    rest = text[end + len(end_mark):]
    # After the switch: the loop's own `break;` (if any) and its closing brace.
    tail_m = re.match(rf'(?:{indent}\tbreak;\n)?{indent}\}}\n', rest)
    if not tail_m:
        raise Unstructurable('unexpected text after the switch')
    return text[:m.start() + 1], indent, body, rest[tail_m.end():]


def structure(text):
    """(new text, message). The text is unchanged when there is no state machine or it cannot be structured."""
    try:
        parts = split(text)
        if parts is None:
            return text, 'no state machine'
        before, indent, body, after = parts
        cases = parse_cases(body)
        nodes = build(cases)
        lines = emit(nodes, cases, indent)
    except Unstructurable as e:
        return text, f'left as it is: {e}'
    return before + '\n'.join(lines) + '\n' + after, f'structured: {len(cases)} cases'


# ---- the trace model the test uses

def run_machine(cases, oracle, limit=100000):
    trace, pc, steps = [], 0, 0
    while pc < len(cases):
        steps += 1
        if steps > limit:
            raise RuntimeError('machine did not end')
        c = cases[pc]
        trace.append(c.number)
        if c.tail == 'fall':
            pc += 1
        elif c.tail == 'break':
            break
        elif c.tail == 'jump':
            pc = c.target
        else:
            pc = c.target if oracle(c.cond) else pc + 1
    return trace


def run_nodes(nodes, cases, oracle, trace):
    for node in nodes:
        kind = node[0]
        if kind == 'body':
            trace.append(node[1])
        elif kind == 'if':
            # node[1] is '!(cond)': the then-part runs when the machine's condition is false
            if not oracle(node[1][2:-1]):
                if run_nodes(node[2], cases, oracle, trace):
                    return True
            else:
                if run_nodes(node[3], cases, oracle, trace):
                    return True
        elif kind == 'do':
            while True:
                if run_nodes(node[1], cases, oracle, trace):
                    return True
                if not oracle(node[2]):
                    break
        elif kind == 'exit':
            return True
    return False


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding='utf-8').read()
    new, message = structure(text)
    open(dst, 'w', encoding='utf-8', newline='\n').write(new)
    print(message)


if __name__ == '__main__':
    main()
