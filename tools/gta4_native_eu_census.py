#!/usr/bin/env python3
"""Census of the native renderer's guest addresses on the European base (docs/switch-port/15-kabba-port.md).

The native renderer (src/graphics/gta4_native) and its hooks (gta4-recomp/src) were written for the
v8 (USA, title update 8) executable. This script lists every v8 address they name and looks for it in
the European base:

  1. addresses: functions hooked (strong definitions of sub_X), functions called (sub_X, __imp__sub_X,
     the FLIRT names of gta4_config.toml), and numeric constants 0x82xxxxxx/0x83xxxxxx (globals,
     call sites, code addresses); comments and string literals are left out, they are not dependencies;
  2. both generated trees (v8 and EU) are read; each function becomes a sequence of normalized
     instructions from the PowerPC listing in the comments: branch targets inside the function become
     offsets from its start, calls and outside targets become X, and the immediates that build an
     address from a lis (lis, the addi/ori/load/store that complete it) become #. Struct offsets and
     other immediates stay;
  3. a v8 function is "trovato" when exactly one EU function has the same normalized sequence, or when
     several do and exactly one lies between the EU images of its two nearest unique neighbours
     ("posizione"); otherwise the EU functions near the expected place (interpolated from those
     neighbours) are compared by 4-instruction shingles: "trovato (simile)" when the best is >= 0.85
     and clearly ahead of the second, "ambiguo" when the best is >= 0.5, "da trovare" otherwise;
  4. an address inside a function (a call site, a return address) or a global built with lis is
     translated through the instruction that names it in a matched function (same index in the
     equal EU sequence).

The Direct3D library moved in one block (EU = v8 - 0x714A0, 06 section 11 b); the column eu_fixed
shows that translation for the library's range, and the census says whether the sequence agrees.

The generated trees hold game code and stay out of the repository: their paths are arguments.

Usage:
  python tools/gta4_native_eu_census.py --v8-gen <v8 generated dir> --eu-gen <generated/eu-base>
      [--out out/gta4-native-census]
"""
import argparse
import bisect
import collections
import glob
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(ROOT, 'glue', 'rexglue-sdk-main')
RECOMP_SRC = os.path.join(SDK, 'gta4-recomp', 'src')
NATIVE_DIRS = [os.path.join(SDK, 'src', 'graphics', 'gta4_native'),
               os.path.join(SDK, 'include', 'rex', 'graphics', 'gta4_native')]
V8_CONFIG = os.path.join(SDK, 'gta4-recomp', 'gta4_config.toml')
EU_D3D_LO, EU_D3D_HI, V8_MINUS_EU = 0x829C7000, 0x829E4600, 0x714A0

# Files of gta4-recomp/src that belong to the native renderer (the hooks file and what it includes).
NATIVE_RECOMP_FILES = {
    'gta4_native_hooks.cpp', 'gta4_phone_trace.inc', 'gta4_tv_trace.inc', 'gta4_fire_escape_trace.inc',
    'gta4_bulb_source_trace.inc', 'gta4_help_trace_hooks.inc', 'gta4_fade_trace_hooks.inc',
    'gta4_phone_trace_hooks.inc', 'gta4_gpu_pass_context.h', 'gta4_temporal_hooks.inc',
}

FUNC_HEAD = re.compile(r'^(?:DEFINE_REX_FUNC|extern "C" REX_FUNC|PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?([A-Za-z_0-9]+)\)')
ASM = re.compile(r'^\s*// ([a-z][a-z0-9.+-]*)(?:\s+(.*))?$')
MAPPING = re.compile(r'\{\s*0x([0-9A-Fa-f]{8}),\s*([A-Za-z_0-9]+)\s*\}')
HEX = re.compile(r'^0x[0-9a-f]+$')
MEM = re.compile(r'^(-?\d+)\((r\d+)\)$')
NO_DEST = ('st', 'cmp', 'b', 'tw', 'td', 'dcb', 'icb', 'mt', 'sync', 'eieio', 'isync', 'nop', 'db16cyc')


class Function:
    __slots__ = ('addr', 'name', 'norm', 'eas', 'n')

    def __init__(self, addr, name):
        self.addr, self.name, self.norm, self.eas, self.n = addr, name, None, None, 0


def normalize(addr, end, raw, want_eas, interned):
    """raw: list of (mnemonic, operand string). Returns (normalized tuple, {index: ea} or None)."""
    lis = {}
    out = []
    eas = {} if want_eas else None
    for i, (mn, ops) in enumerate(raw):
        args = [a.strip() for a in ops.split(',')] if ops else []
        norm_args = []
        dest = args[0] if args and not mn.startswith(NO_DEST) and re.match(r'^[rf]\d+$', args[0]) else None
        new_lis = None
        for j, a in enumerate(args):
            if HEX.match(a):
                t = int(a, 16)
                if mn.startswith('b') and mn not in ('bl',) and addr <= t < end:
                    norm_args.append('L%x' % (t - addr))
                else:
                    norm_args.append('X')
                continue
            m = MEM.match(a)
            if m and m.group(2) in lis:
                if eas is not None:
                    eas[i] = (lis[m.group(2)] + int(m.group(1))) & 0xFFFFFFFF
                norm_args.append('#(%s)' % m.group(2))
                continue
            norm_args.append(a)
        if mn in ('lis', 'addis') and len(args) >= 2:
            if mn == 'lis' or (len(args) == 3 and args[1] == 'r0'):
                try:
                    new_lis = (int(args[-1]) << 16) & 0xFFFFFFFF
                    norm_args[-1] = '#'
                except ValueError:
                    pass
        elif mn == 'mr' and len(args) == 2 and args[1] in lis:
            new_lis = lis[args[1]]
        elif mn in ('addi', 'ori', 'addic') and len(args) == 3 and args[1] in lis:
            try:
                imm = int(args[2])
                v = (lis[args[1]] + imm) & 0xFFFFFFFF if mn != 'ori' else (lis[args[1]] | (imm & 0xFFFF))
                if eas is not None:
                    eas[i] = v
                norm_args[2] = '#'
                new_lis = v
            except ValueError:
                pass
        if dest is not None:
            lis.pop(dest, None)
            if new_lis is not None:
                lis[dest] = new_lis
        tok = mn + ' ' + ','.join(norm_args)
        out.append(interned.setdefault(tok, tok))
    return tuple(out), eas


def read_generated(gen_dir, keep_norm, want_eas_for, data_targets):
    """Returns {addr: Function}. keep_norm(addr, end) -> keep the normalized sequence; data_targets: set of
    addresses whose referencing instructions are recorded (eas kept for functions that hit one)."""
    names = {}
    for init in glob.glob(os.path.join(gen_dir, '*_init.cpp')):
        with open(init, encoding='utf-8', errors='ignore') as f:
            for m in MAPPING.finditer(f.read()):
                names[m.group(2)] = int(m.group(1), 16)
    funcs = {}
    interned = {}
    files = sorted(glob.glob(os.path.join(gen_dir, '*_recomp.*.cpp')),
                   key=lambda p: int(re.search(r'\.(\d+)\.cpp$', p).group(1)))
    for path in files:
        cur = None
        raw = []
        pending = []
        with open(path, encoding='utf-8', errors='ignore') as f:
            for line in f:
                if line.startswith(('DEFINE_REX_FUNC', 'extern "C" REX_FUNC', 'PPC_FUNC_IMPL', 'REX_FUNC(')):
                    m = FUNC_HEAD.match(line)
                    if m:
                        name = m.group(1)
                        addr = int(name[4:], 16) if re.match(r'^sub_[0-9A-F]{8}$', name) else names.get(name)
                        if cur is not None:
                            pending.append((cur, raw))
                        cur, raw = (Function(addr, name) if addr else None), []
                        continue
                if cur is not None and '//' in line:
                    m = ASM.match(line)
                    if m:
                        raw.append((m.group(1), m.group(2) or ''))
        if cur is not None:
            pending.append((cur, raw))
        pending.sort(key=lambda p: p[0].addr)
        for k, (fn, raw) in enumerate(pending):
            end = pending[k + 1][0].addr if k + 1 < len(pending) else fn.addr + 4 * len(raw)
            norm, eas = normalize(fn.addr, end, raw, True, interned)
            fn.n = len(norm)
            hit = bool(eas) and any(v in data_targets for v in eas.values())
            fn.norm = norm if (keep_norm(fn.addr, end) or hit) else hashlib.sha1('\n'.join(norm).encode()).digest()
            fn.eas = eas if (hit or want_eas_for(fn.addr)) else None
            funcs[fn.addr] = fn
    return funcs, names


def digest(fn):
    return fn.norm if isinstance(fn.norm, bytes) else hashlib.sha1('\n'.join(fn.norm).encode()).digest()


def shingles(seq, k=4):
    return {seq[i:i + k] for i in range(max(1, len(seq) - k + 1))}


# ---------------------------------------------------------------------------------------------
# Source side

def strip_comments_and_strings(text):
    """Blank out comments and string literals, keeping line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c == '"' or (c == "'" and i + 2 < n and text[i + 2] == "'"):
            q = c
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            out.append(q + ' ' * (j - i - 1) + q if j < n else ' ' * (j - i))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


# Matched on the text with string literals blanked: extern "C" reads extern " ".
HOOK_DEF = re.compile(r'(?:extern\s+"\s*"\s+(?:void\s+)?|REX_HOOK_RAW\(|REX_FUNC\()\s*(sub_[0-9A-F]{8})\s*\(?\s*(?:PPCContext|\))')
SUB_REF = re.compile(r'\b(?:__imp__)?sub_([0-9A-F]{8})\b')
NUM = re.compile(r'\b0[xX](8[23][0-9A-Fa-f]{6})\b')


def source_files():
    files = []
    for d in [RECOMP_SRC] + NATIVE_DIRS:
        for dirpath, _, names in os.walk(d):
            for n in names:
                if n.endswith(('.cpp', '.h', '.inc', '.mm', '.c')):
                    files.append(os.path.join(dirpath, n))
    return sorted(files)


def flirt_names():
    names = {}
    with open(V8_CONFIG, encoding='utf-8') as f:
        for line in f:
            m = re.match(r'^\s*([A-Za-z_][A-Za-z_0-9]*)\s*=\s*0x([0-9A-Fa-f]{8})', line)
            if m:
                names[m.group(1)] = int(m.group(2), 16)
            m = re.match(r'^\s*"0x([0-9A-Fa-f]{8})"\s*=\s*\{\s*name\s*=\s*"([A-Za-z_][A-Za-z_0-9]*)"', line)
            if m:
                names[m.group(2)] = int(m.group(1), 16)
    return names


def scan_sources():
    """Returns {addr: {'kinds': set, 'files': set, 'native': bool, 'ctx': str}}"""
    refs = collections.defaultdict(lambda: {'kinds': set(), 'files': set(), 'native': False, 'ctx': ''})
    flirt = flirt_names()
    flirt_re = re.compile(r'(?<![\w:.>])(?:__imp__)?(' + '|'.join(sorted(flirt, key=len, reverse=True)) + r')\s*[(,)]') if flirt else None
    for path in source_files():
        rel = os.path.relpath(path, SDK).replace('\\', '/')
        native = (os.path.basename(path) in NATIVE_RECOMP_FILES) or '/gta4_native/' in rel
        with open(path, encoding='utf-8', errors='ignore') as f:
            raw = f.read()
        code = strip_comments_and_strings(raw)
        raw_lines = raw.split('\n')

        def add(addr, kind, pos):
            r = refs[addr]
            r['kinds'].add(kind)
            r['files'].add(os.path.basename(path))
            r['native'] = r['native'] or native
            if not r['ctx']:
                line = code.count('\n', 0, pos)
                r['ctx'] = context_of(raw_lines, line)

        definitions = set()
        for m in HOOK_DEF.finditer(code):
            definitions.add(m.start(1))
            add(int(m.group(1)[4:], 16), 'hook', m.start())
        for m in SUB_REF.finditer(code):
            if m.start() not in definitions:
                add(int(m.group(1), 16), 'call', m.start())
        for m in NUM.finditer(code):
            add(int(m.group(1), 16), 'const', m.start())
        if flirt_re:
            for m in flirt_re.finditer(code):
                add(flirt[m.group(1)], 'call:' + m.group(1), m.start())
    return refs


def context_of(lines, idx):
    """Nearest comment line above idx (up to 6 lines), trimmed, for a role guess."""
    for k in range(idx, max(-1, idx - 7), -1):
        s = lines[k].strip()
        if s.startswith('//') and len(s) > 4:
            return s[2:].strip()[:110]
    return lines[idx].strip()[:110]


# ---------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--v8-gen', required=True)
    ap.add_argument('--eu-gen', required=True)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'gta4-native-census'))
    args = ap.parse_args()

    refs = scan_sources()
    print('source references: %d addresses' % len(refs), file=sys.stderr)
    targets = set(refs)

    tsorted = sorted(targets)

    def has_target(a, end):
        i = bisect.bisect_left(tsorted, a)
        return i < len(tsorted) and tsorted[i] < end

    v8, _ = read_generated(args.v8_gen, has_target, lambda a: False, targets)
    print('v8: %d functions' % len(v8), file=sys.stderr)
    v8_starts = sorted(v8)

    def v8_container(a):
        k = bisect.bisect_right(v8_starts, a) - 1
        if k < 0:
            return None
        fn = v8[v8_starts[k]]
        return fn if a < fn.addr + 4 * max(fn.n, 1) else None

    # Functions that contain a referenced code address need their sequence and eas on both sides.
    containers = {}
    for a in targets:
        if a in v8:
            containers[a] = v8[a]
        else:
            fn = v8_container(a)
            if fn is not None:
                containers[a] = fn
    need_v8 = {fn.addr for fn in containers.values()} | {fn.addr for fn in v8.values() if fn.eas}
    for fa in need_v8:
        if isinstance(v8[fa].norm, bytes):
            print('warning: v8 %08X kept only as digest' % fa, file=sys.stderr)

    # EU: keep every sequence (needed for the shingle search), and eas everywhere.
    eu, _ = read_generated(args.eu_gen, lambda a, end: True, lambda a: True, set())
    print('eu: %d functions' % len(eu), file=sys.stderr)
    eu_starts = sorted(eu)

    by_digest_v8 = collections.defaultdict(list)
    for fn in v8.values():
        by_digest_v8[digest(fn)].append(fn.addr)
    by_digest_eu = collections.defaultdict(list)
    for fn in eu.values():
        by_digest_eu[digest(fn)].append(fn.addr)

    # Anchors: digests unique on both sides with a non-trivial body.
    anchors = {}
    for d, vs in by_digest_v8.items():
        es = by_digest_eu.get(d)
        if len(vs) == 1 and es and len(es) == 1 and v8[vs[0]].n >= 8:
            anchors[vs[0]] = es[0]
    # The two builds link their translation units in a different order (the game code below v8
    # 0x82790000 is shuffled; the engine and the libraries above it move by ~0x6E000-0x72000), so
    # there is no single monotone mapping: the expected EU place of a function comes from the
    # deltas of its nearest unique neighbours in v8 order.
    anchor_v8 = sorted(anchors)
    print('anchors: %d unique pairs' % len(anchors), file=sys.stderr)

    def neighbour_deltas(a, k=3):
        i = bisect.bisect_left(anchor_v8, a)
        near = anchor_v8[max(0, i - k):i] + [x for x in anchor_v8[i:i + k + 1] if x != a][:k]
        return sorted({x - anchors[x] for x in near})

    shingle_cache = {}

    def eu_shingles(e):
        s = shingle_cache.get(e)
        if s is None:
            s = shingle_cache[e] = shingles(eu[e].norm)
        return s

    mnemonic_cache = {}

    def mnemonics(f):
        key = (id(f), f.addr)
        m = mnemonic_cache.get(key)
        if m is None:
            m = mnemonic_cache[key] = tuple(t.split(' ', 1)[0] for t in f.norm)
        return m

    mshingle_cache = {}

    def eu_mnemonic_shingles(e):
        sh = mshingle_cache.get(e)
        if sh is None:
            sh = mshingle_cache[e] = shingles(mnemonics(eu[e]), 6)
        return sh

    def match_function(fa):
        fn = v8[fa]
        if fa in anchors:
            return anchors[fa], 'trovato', 'sequenza unica', 1.0
        deltas = neighbour_deltas(fa)
        cands = by_digest_eu.get(digest(fn), [])
        if len(cands) == 1:
            return cands[0], 'trovato', 'sequenza unica', 1.0
        if cands:
            near = [c for c in cands if any(abs((fa - c) - d) <= 0x800 for d in deltas)]
            if len(near) == 1:
                return near[0], 'trovato', 'sequenza uguale x%d, posizione' % len(cands), 1.0
            return None, 'ambiguo', 'sequenza uguale x%d' % len(cands), 1.0
        if isinstance(fn.norm, bytes):
            return None, 'da trovare', 'sequenza non tenuta', 0.0
        # Shingle search around the places the neighbours point to.
        a = shingles(fn.norm)
        scored = {}
        for d in deltas or [0]:
            exp = fa - d
            for e in eu_starts[bisect.bisect_left(eu_starts, exp - 0x8000):bisect.bisect_right(eu_starts, exp + 0x8000)]:
                ef = eu[e]
                if e in scored or not ef.n or abs(ef.n - fn.n) > max(8, fn.n // 2):
                    continue
                b = eu_shingles(e)
                scored[e] = len(a & b) / max(1, len(a | b))
        scored = sorted(((s, e) for e, s in scored.items()), reverse=True)
        best = scored[0] if scored else (0.0, None)
        second = scored[1][0] if len(scored) > 1 else 0.0
        if best[0] >= 0.85 and best[0] - second >= 0.1:
            return best[1], 'trovato', 'simile %.2f' % best[0], best[0]
        # Second level: mnemonics only (the European build often allocates registers
        # differently), 6-instruction shingles, wider windows.
        am = shingles(mnemonics(fn), 6)
        mscored = {}
        for d in deltas or [0]:
            exp = fa - d
            for e in eu_starts[bisect.bisect_left(eu_starts, exp - 0x20000):bisect.bisect_right(eu_starts, exp + 0x20000)]:
                ef = eu[e]
                if e in mscored or not ef.n or abs(ef.n - fn.n) > max(8, fn.n // 2):
                    continue
                b = eu_mnemonic_shingles(e)
                mscored[e] = len(am & b) / max(1, len(am | b))
        mscored = sorted(((s, e) for e, s in mscored.items()), reverse=True)
        mbest = mscored[0] if mscored else (0.0, None)
        msecond = mscored[1][0] if len(mscored) > 1 else 0.0
        if mbest[0] >= 0.8 and mbest[0] - msecond >= 0.1:
            return mbest[1], 'trovato', 'mnemonici %.2f' % mbest[0], mbest[0]
        if best[0] >= 0.5:
            return best[1], 'ambiguo', 'simile %.2f (secondo %.2f)' % (best[0], second), best[0]
        if mbest[0] >= 0.5:
            return mbest[1], 'ambiguo', 'mnemonici %.2f (secondo %.2f)' % (mbest[0], msecond), mbest[0]
        if not scored and not mscored:
            return None, 'da trovare', 'nessun candidato vicino', 0.0
        return None, 'da trovare', 'migliore %.2f, mnemonici %.2f' % (best[0], mbest[0]), max(best[0], mbest[0])

    def align(vf, ef, i, w=8):
        """Index in ef of v8 instruction i of vf, by the mnemonics around it; (j, score) or (None, 0)."""
        vm, em = mnemonics(vf), mnemonics(ef)
        centre = i * ef.n // max(1, vf.n)
        span = max(64, ef.n // 4)
        scores = []
        for j in range(max(0, centre - span), min(ef.n, centre + span + 1)):
            if em[j] != vm[i]:
                continue
            sc = sum(1 for d in range(-w, w + 1)
                     if 0 <= i + d < vf.n and 0 <= j + d < ef.n and vm[i + d] == em[j + d])
            scores.append((sc, j))
        if not scores:
            return None, 0
        scores.sort(reverse=True)
        if len(scores) > 1 and scores[1][0] == scores[0][0]:
            return None, scores[0][0]
        return scores[0][1], scores[0][0]

    rows = []
    func_cache = {}
    for a in sorted(targets):
        r = refs[a]
        in_d3d = EU_D3D_LO + V8_MINUS_EU <= a < EU_D3D_HI + V8_MINUS_EU
        eu_fixed = '%08X' % (a - V8_MINUS_EU) if in_d3d else ''
        fn = containers.get(a)
        if fn is None:
            # A global: translate through instructions that build it.
            votes = collections.Counter()
            for vf in v8.values():
                if not vf.eas:
                    continue
                for i, ea in vf.eas.items():
                    if ea == a:
                        if vf.addr not in func_cache:
                            func_cache[vf.addr] = match_function(vf.addr)
                        e, st, how, _ = func_cache[vf.addr]
                        if e is None or st != 'trovato' or isinstance(vf.norm, bytes):
                            continue
                        ef = eu[e]
                        if ef.eas is None:
                            continue
                        if how.startswith('sequenza'):
                            j = i if i < len(ef.norm) and ef.norm[i] == vf.norm[i] else None
                        else:
                            j, sc = align(vf, ef, i)
                            if j is not None and sc < 15:
                                j = None
                        v = ef.eas.get(j) if j is not None else None
                        if v is not None:
                            votes[v] += 1
            if not votes:
                status, how, eu_addr = 'da trovare', 'globale: nessuna istruzione corrispondente', ''
            elif len(votes) == 1:
                v, c = votes.most_common(1)[0]
                status, how, eu_addr = 'trovato', 'globale: %d istruzioni concordi' % c, '%08X' % v
            else:
                (v, c), (_, c2) = votes.most_common(2)
                status, how, eu_addr = ('trovato' if c >= 3 * c2 else 'ambiguo'), \
                    'globale: %d contro %d' % (c, c2), '%08X' % v
            kind = 'globale'
        else:
            if fn.addr not in func_cache:
                func_cache[fn.addr] = match_function(fn.addr)
            e, status, how, _ = func_cache[fn.addr]
            if fn.addr == a:
                kind = 'funzione'
                eu_addr = '%08X' % e if e is not None else ''
            else:
                kind = 'interno +0x%X' % (a - fn.addr)
                eu_addr = ''
                if e is not None and how.startswith('sequenza'):
                    eu_addr = '%08X' % (e + (a - fn.addr))
                elif e is not None and not isinstance(fn.norm, bytes):
                    j, sc = align(fn, eu[e], (a - fn.addr) // 4)
                    if j is not None and sc >= 15:
                        eu_addr = '%08X' % (e + 4 * j)
                        how += ', allineato %d/17' % sc
                    else:
                        status, how = 'ambiguo', how + ' (interno non allineato: %d/17)' % sc
        if in_d3d and eu_addr and eu_addr != eu_fixed and kind == 'funzione':
            how += ' (diverso dallo scarto fisso)'
        rows.append((a, kind, ','.join(sorted(r['kinds'])), 'nativo' if r['native'] else 'altro', in_d3d,
                     eu_fixed, eu_addr, status, how, ','.join(sorted(r['files'])), r['ctx']))

    os.makedirs(args.out, exist_ok=True)
    tsv = os.path.join(args.out, 'census.tsv')
    with open(tsv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('v8\ttipo\tuso\tgruppo\td3d\teu_fixed\teu\tstato\tcome\tfile\tcontesto\n')
        for row in rows:
            a, kind, uses, grp, in_d3d, fixed, eu_addr, status, how, files, ctx = row
            f.write('%08X\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                a, kind, uses, grp, 'si' if in_d3d else '', fixed, eu_addr, status, how, files,
                ctx.replace('\t', ' ')))
    summary = collections.Counter((r[3], r[1].split()[0], r[7]) for r in rows)
    for k in sorted(summary):
        print('%-7s %-9s %-11s %d' % (k + (summary[k],)))
    print('written %s' % tsv)



if __name__ == '__main__':
    main()
