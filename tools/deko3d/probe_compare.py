#!/usr/bin/env python3
"""The GPU's values of a vertex shader against the same GLSL run on the PC (run 54, 16-deko3d-devlog.md section AB).

    python tools/deko3d/probe_compare.py <file.probe> [--vertices N] [--all]

The recorder (deko3d/dk_shader_probe.h, DkRecorder::Draw) drew one skinned draw again with its vertex shader
instrumented to store, at the end of main, every top-level variable of main and gl_Position for the first 64 vertices,
and saved with them the exact inputs the GPU was given: the vertex attribute states, the vertex buffers in their host
form, the vertex constant bank and the shared block. This tool decodes the attributes as the GPU does (deko3d's
DkVtxAttribSize / DkVtxAttribType, isBgra), runs the same GLSL text (an interpreter of the subset SPIRV-Cross emits for
these shaders, float32 arithmetic) and compares each variable, in program order: the first one whose value differs is
where the GPU's execution leaves the program's meaning (or where the inputs it read differ from the saved ones).

Not interpreted (reported, never guessed): texture fetches in the vertex shader and loops.

Version 2 files (run 55, section AC) also hold gl_Position of every vertex of the draw (up to 8192), its indices, viewport
and depth state: every position is compared too, and <file>.png (or --png) shows the mesh as the GPU placed it (left)
beside the PC's (right).
"""
import argparse
import math
import re
import struct
import sys

# float32, int32 and uint32 arithmetic without numpy: every float result is rounded to float32 through struct.
_F = struct.Struct('<f')
_U = struct.Struct('<I')


def f32(x):
    try:
        return _F.unpack(_F.pack(x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def f32_bits(x):
    return _U.unpack(_F.pack(x))[0]


def bits_f32(u):
    return _F.unpack(_U.pack(u & 0xFFFFFFFF))[0]


def i32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >= 1 << 31 else x


def u32n(x):
    return x & 0xFFFFFFFF


# ---------------------------------------------------------------------------------------------------- the probe file


class ProbeFile:
    pass


def read_probe(data):
    p = ProbeFile()
    o = 0

    def u32():
        nonlocal o
        v = struct.unpack_from('<I', data, o)[0]
        o += 4
        return v

    def u64():
        nonlocal o
        v = struct.unpack_from('<Q', data, o)[0]
        o += 8
        return v

    def raw(n):
        nonlocal o
        b = data[o:o + n]
        o += n
        return b

    def text():
        return raw(u32()).decode('utf-8', 'replace')

    if u32() != 0x52503454:
        raise ValueError('not a probe file (magic)')
    p.version = u32()
    p.hash = u64()
    p.value = u32()
    p.sequence = u64()
    p.vertices = u32()
    p.slots = u32()
    p.vars = [(text(), text()) for _ in range(p.slots)]  # (type, name)
    p.glsl = text()
    p.attribs = []
    for _ in range(u32()):
        p.attribs.append(dict(buffer=u32(), fixed=u32(), offset=u32(), size=u32(), type=u32(), bgra=u32()))
    p.buffers = []
    for _ in range(u32()):
        stride, divisor = u32(), u32()
        p.buffers.append(dict(stride=stride, divisor=divisor, data=raw(u32())))
    bank = struct.unpack('<1024f', raw(256 * 16))
    p.bank = [list(bank[4 * i:4 * i + 4]) for i in range(256)]
    shared = struct.unpack('<256I', raw(1024))
    p.shared = [list(shared[4 * i:4 * i + 4]) for i in range(64)]
    values = struct.unpack(f'<{p.vertices * p.slots * 4}I', raw(p.vertices * p.slots * 16))
    p.values = [[list(values[(v * p.slots + k) * 4:(v * p.slots + k) * 4 + 4]) for k in range(p.slots)]
                for v in range(p.vertices)]
    # Version 2 (run 55): gl_Position of every vertex (up to 8192), then the draw.
    p.positions, p.indices, p.viewport, p.target = [], [], None, (0, 0)
    p.primitive = p.depth_enable = p.depth_function = p.depth_write = p.cull_mode = p.pixel_shader = 0
    if p.version >= 2:
        n = u32()
        flat = struct.unpack(f'<{n * 4}I', raw(n * 16))
        p.positions = [list(flat[4 * i:4 * i + 4]) for i in range(n)]
        p.primitive = u32()
        count = u32()
        p.indices = list(struct.unpack(f'<{count}I', raw(4 * count)))
        p.viewport = struct.unpack('<6f', raw(24))
        p.target = (u32(), u32())
        p.depth_enable, p.depth_function, p.depth_write, p.cull_mode = u32(), u32(), u32(), u32()
        p.pixel_shader = u64()
    return p


def write_probe(p):
    """The inverse of read_probe (tests build synthetic probes with it)."""
    out = bytearray()

    def u32(v):
        out.extend(struct.pack('<I', v))

    def text(t):
        b = t.encode()
        u32(len(b))
        out.extend(b)

    version = getattr(p, 'version', 1)
    u32(0x52503454)
    u32(version)
    out.extend(struct.pack('<Q', p.hash))
    u32(p.value)
    out.extend(struct.pack('<Q', p.sequence))
    u32(p.vertices)
    u32(len(p.vars))
    for t, n in p.vars:
        text(t)
        text(n)
    text(p.glsl)
    u32(len(p.attribs))
    for a in p.attribs:
        for k in ('buffer', 'fixed', 'offset', 'size', 'type', 'bgra'):
            u32(a[k])
    u32(len(p.buffers))
    for b in p.buffers:
        u32(b['stride'])
        u32(b['divisor'])
        u32(len(b['data']))
        out.extend(b['data'])
    for row in p.bank:
        out.extend(struct.pack('<4f', *row))
    for row in p.shared:
        out.extend(struct.pack('<4I', *row))
    for vertex in p.values:
        for slot in vertex:
            out.extend(struct.pack('<4I', *slot))
    if version >= 2:
        u32(len(p.positions))
        for pos in p.positions:
            out.extend(struct.pack('<4I', *pos))
        u32(p.primitive)
        u32(len(p.indices))
        out.extend(struct.pack(f'<{len(p.indices)}I', *p.indices))
        out.extend(struct.pack('<6f', *p.viewport))
        u32(p.target[0])
        u32(p.target[1])
        for v in (p.depth_enable, p.depth_function, p.depth_write, p.cull_mode):
            u32(v)
        out.extend(struct.pack('<Q', p.pixel_shader))
    return bytes(out)


# ---------------------------------------------------------------------------------------------------- vertex fetch

# deko3d.h: DkVtxAttribSize -> (components, bits each); DkVtxAttribType values.
SIZES = {0x12: (1, 32), 0x04: (2, 32), 0x02: (3, 32), 0x01: (4, 32), 0x1b: (1, 16), 0x0f: (2, 16), 0x05: (3, 16),
         0x03: (4, 16), 0x1d: (1, 8), 0x18: (2, 8), 0x13: (3, 8), 0x0a: (4, 8), 0x30: (4, 0), 0x31: (3, 0)}
SNORM, UNORM, SINT, UINT, USCALED, SSCALED, FLOAT = 1, 2, 3, 4, 5, 6, 7


def _norm(v, n, signed):
    if signed:
        return max(f32(v / ((1 << (n - 1)) - 1)), -1.0)
    return f32(v / ((1 << n) - 1))


def fetch_attribute(probe, attrib, vertex):
    """The (x, y, z, w) the GPU reads for one attribute of one vertex."""
    b = probe.buffers[attrib['buffer']]
    record = 0 if b['divisor'] else vertex
    base = record * b['stride'] + attrib['offset']
    data = b['data']
    comps, bits = SIZES.get(attrib['size'], (0, 0))
    t = attrib['type']
    integer = t in (SINT, UINT)
    out = [0, 0, 0, 1] if integer else [0.0, 0.0, 0.0, 1.0]
    signed = t in (SINT, SNORM, SSCALED)
    if attrib['size'] == 0x30:  # 10_10_10_2, R in the low bits
        w = struct.unpack_from('<I', data, base)[0]
        fields = [(w >> 0) & 0x3FF, (w >> 10) & 0x3FF, (w >> 20) & 0x3FF, (w >> 30) & 0x3]
        widths = [10, 10, 10, 2]
        for i in range(4):
            v, n = fields[i], widths[i]
            if signed and v >= 1 << (n - 1):
                v -= 1 << n
            out[i] = _norm(v, n, signed) if t in (SNORM, UNORM) else (float(v) if not integer else v)
        return out
    for i in range(comps):
        if bits == 32:
            fmt = '<f' if t == FLOAT else ('<i' if signed else '<I')
            v = struct.unpack_from(fmt, data, base + 4 * i)[0]
        elif bits == 16:
            fmt = '<e' if t == FLOAT else ('<h' if signed else '<H')
            v = struct.unpack_from(fmt, data, base + 2 * i)[0]
        else:
            v = data[base + i]
            if signed and v >= 128:
                v -= 256
        if t in (SNORM, UNORM):
            v = _norm(v, bits, signed)
        elif t in (USCALED, SSCALED):
            v = float(v)
        elif t == FLOAT:
            v = f32(v)
        out[i] = v
    if attrib['bgra'] and comps >= 3:
        out[0], out[2] = out[2], out[0]
    return out


# ---------------------------------------------------------------------------------------------------- GLSL values


def norm(kind, x):
    if kind == 'float':
        return f32(float(x))
    if kind == 'int':
        return i32(int(x))
    if kind == 'uint':
        return u32n(int(x))
    return bool(x)


class V:
    """A GLSL value: kind float/int/uint/bool and 1-4 components."""
    __slots__ = ('k', 'a')

    def __init__(self, kind, arr):
        self.k = kind
        self.a = [norm(kind, x) for x in arr]

    def n(self):
        return len(self.a)

    def copy(self):
        return V(self.k, list(self.a))


class Unsupported(Exception):
    pass


TYPES = {'float': ('float', 1), 'int': ('int', 1), 'uint': ('uint', 1), 'bool': ('bool', 1)}
for _k, _p in (('float', ''), ('int', 'i'), ('uint', 'u'), ('bool', 'b')):
    for _n in (2, 3, 4):
        TYPES[f'{_p}vec{_n}'] = (_k, _n)
SWZ = {'x': 0, 'y': 1, 'z': 2, 'w': 3, 'r': 0, 'g': 1, 'b': 2, 'a': 3}


def convert(v, kind):
    if v.k == kind:
        return v.copy()
    out = []
    for x in v.a:
        if kind == 'float':
            out.append(float(x))
        elif kind == 'bool':
            out.append(x != 0)
        elif kind == 'int':
            if v.k == 'float':
                out.append(0 if math.isnan(x) else int(max(min(math.trunc(x), 2 ** 31 - 1), -2 ** 31)))
            else:
                out.append(int(x))
        else:  # uint
            if v.k == 'float':
                out.append(0 if math.isnan(x) or x <= 0 else int(min(math.trunc(x), 2 ** 32 - 1)))
            else:
                out.append(int(x))
    return V(kind, out)


def broadcast(a, b):
    if a.n() == b.n():
        return a, b
    if a.n() == 1:
        return V(a.k, a.a * b.n()), b
    if b.n() == 1:
        return a, V(b.k, b.a * a.n())
    raise Unsupported('component counts')


# ---------------------------------------------------------------------------------------------------- the parser

TOKEN = re.compile(r'\s*(?:(\d+\.\d*(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+|\.\d+(?:[eE][-+]?\d+)?)f?|(\d+)(u?)|([A-Za-z_]\w*)|'
                   r'(<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^!~<>=?:;,.()\[\]{}]))')


def tokenize(src):
    out = []
    i = 0
    while i < len(src):
        m = TOKEN.match(src, i)
        if not m or m.end() == i:
            if src[i:].strip() == '':
                break
            raise Unsupported('token at ' + src[i:i + 20])
        i = m.end()
        if m.group(1) is not None:
            out.append(('num', float(m.group(1))))
        elif m.group(2) is not None:
            out.append(('uint' if m.group(3) else 'int', int(m.group(2))))
        elif m.group(4) is not None:
            out.append(('id', m.group(4)))
        elif m.group(5) is not None:
            out.append(('op', m.group(5)))
    return out


BIN = [('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='), ('<', '>', '<=', '>='), ('<<', '>>'), ('+', '-'),
       ('*', '/', '%')]


class Parser:
    def __init__(self, tokens):
        self.t = tokens
        self.i = 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else ('eof', None)

    def take(self, val=None):
        tok = self.peek()
        if val is not None and tok[1] != val:
            raise Unsupported(f'expected {val}, got {tok}')
        self.i += 1
        return tok

    def accept(self, val):
        if self.peek()[1] == val:
            self.i += 1
            return True
        return False

    # statements -> nested tuples
    def block(self):
        self.take('{')
        out = []
        while not self.accept('}'):
            out.append(self.statement())
        return ('block', out)

    def statement(self):
        tok = self.peek()
        if tok[1] == '{':
            return self.block()
        if tok == ('id', 'if'):
            self.take()
            self.take('(')
            cond = self.expr()
            self.take(')')
            then = self.statement()
            other = None
            if self.peek() == ('id', 'else'):
                self.take()
                other = self.statement()
            return ('if', cond, then, other)
        if tok[0] == 'id' and tok[1] in ('for', 'while', 'do', 'switch', 'return', 'discard', 'break', 'continue'):
            raise Unsupported(tok[1])
        if tok == ('id', 'precise'):
            self.take()
            tok = self.peek()
        if tok[0] == 'id' and tok[1] in TYPES and self.peek(1)[0] == 'id':
            self.take()
            name = self.take()[1]
            init = None
            if self.accept('='):
                init = self.expr()
            self.take(';')
            return ('decl', tok[1], name, init)
        target = self.postfix()
        if self.accept('='):
            value = self.expr()
            self.take(';')
            return ('assign', target, value)
        self.take(';')
        return ('expr', target)

    def expr(self):
        cond = self.binary(0)
        if self.accept('?'):
            a = self.expr()
            self.take(':')
            b = self.expr()
            return ('?', cond, a, b)
        return cond

    def binary(self, level):
        if level == len(BIN):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek()[0] == 'op' and self.peek()[1] in BIN[level]:
            op = self.take()[1]
            left = ('bin', op, left, self.binary(level + 1))
        return left

    def unary(self):
        if self.peek()[0] == 'op' and self.peek()[1] in ('-', '!', '~', '+'):
            op = self.take()[1]
            return ('un', op, self.unary())
        return self.postfix()

    def postfix(self):
        node = self.primary()
        while True:
            if self.accept('.'):
                node = ('member', node, self.take()[1])
            elif self.accept('['):
                idx = self.expr()
                self.take(']')
                node = ('index', node, idx)
            else:
                return node

    def primary(self):
        tok = self.take()
        if tok[0] in ('num', 'int', 'uint'):
            return ('lit', tok[0], tok[1])
        if tok[1] == '(':
            e = self.expr()
            self.take(')')
            return e
        if tok[0] == 'id':
            if self.accept('('):
                args = []
                if not self.accept(')'):
                    args.append(self.expr())
                    while self.accept(','):
                        args.append(self.expr())
                    self.take(')')
                return ('call', tok[1], args)
            return ('name', tok[1])
        raise Unsupported(f'primary {tok}')


# ---------------------------------------------------------------------------------------------------- the interpreter


class Interpreter:
    def __init__(self, glsl):
        self.inputs = {}  # name -> (location, type)
        self.globals = {}  # name -> type
        for m in re.finditer(r'layout\(location = (\d+)\) in (\w+) (\w+);', glsl):
            self.inputs[m.group(3)] = (int(m.group(1)), m.group(2))
        main = glsl.find('void main()')
        head = glsl[:main]
        for m in re.finditer(r'^(vec[234]|float|int|uint|ivec[234]|uvec[234]) (_\w+);', head, re.M):
            self.globals[m.group(2)] = m.group(1)
        # specialisation constants as SPIRV-Cross writes them: #define SPIRV_CROSS_CONSTANT_ID_n v, const T name = ...;
        defines = dict(re.findall(r'^#define (SPIRV_CROSS_CONSTANT_ID_\d+) (\S+)', head, re.M))
        self.constants = {}
        for m in re.finditer(r'^const (uint|int|float|bool) (\w+) = (\w+);', head, re.M):
            text = defines.get(m.group(3), m.group(3))
            kind = m.group(1)
            value = (text == 'true') if kind == 'bool' else (float(text.rstrip('f')) if kind == 'float'
                                                               else int(text.rstrip('u')))
            self.constants[m.group(2)] = V(kind, [value])
        self.unsupported = None
        if re.search(r'\btexture\w*\s*\(', glsl[main:]):
            self.unsupported = 'a texture fetch in the vertex shader'
        try:
            body_start = glsl.find('{', main)
            # main's closing brace
            depth, close = 0, -1
            for i in range(body_start, len(glsl)):
                if glsl[i] == '{':
                    depth += 1
                elif glsl[i] == '}':
                    depth -= 1
                    if depth == 0:
                        close = i
                        break
            self.body = Parser(tokenize(glsl[body_start:close + 1])).block()
        except Unsupported as e:
            self.body = None
            self.unsupported = self.unsupported or f'not parsed: {e}'

    def run(self, probe, vertex):
        if self.unsupported:
            raise Unsupported(self.unsupported)
        self.probe = probe
        env = {}
        for name, typ in self.globals.items():
            kind, n = TYPES[typ]
            env[name] = V(kind, [0] * n)
        for name, (loc, typ) in self.inputs.items():
            kind, n = TYPES[typ]
            vals = fetch_attribute(probe, probe.attribs[loc], vertex) if loc < len(probe.attribs) else [0, 0, 0, 1]
            env[name] = convert(V('int' if isinstance(vals[0], int) and kind != 'float' else 'float', vals[:n]), kind) \
                if kind != 'float' else V('float', vals[:n])
        env.update((name, value.copy()) for name, value in self.constants.items())
        env['gl_Position'] = V('float', [0, 0, 0, 0])
        env['gl_ClipDistance'] = V('float', [0])
        env['gl_VertexID'] = V('int', [vertex])
        self.env = env
        # main's own scope is kept: its top-level variables are what the probe reports.
        main_scope = {}
        for statement in self.body[1]:
            self.exec(statement, [env, main_scope])
        merged = dict(env)
        merged.update(main_scope)
        return merged

    def lookup(self, scopes, name):
        for sc in reversed(scopes):
            if name in sc:
                return sc
        if name.startswith('out_var_'):
            scopes[0][name] = V('float', [0, 0, 0, 0])
            return scopes[0]
        raise Unsupported('name ' + name)

    def exec(self, st, scopes):
        kind = st[0]
        if kind == 'block':
            scopes.append({})
            for x in st[1]:
                self.exec(x, scopes)
            scopes.pop()
        elif kind == 'decl':
            _, typ, name, init = st
            k, n = TYPES[typ]
            scopes[-1][name] = convert(self.eval(init, scopes), k) if init is not None else V(k, [0] * n)
        elif kind == 'assign':
            self.store(st[1], self.eval(st[2], scopes), scopes)
        elif kind == 'if':
            if self.eval(st[1], scopes).a[0]:
                self.exec(st[2], scopes)
            elif st[3] is not None:
                self.exec(st[3], scopes)
        elif kind == 'expr':
            self.eval(st[1], scopes)

    def store(self, target, value, scopes):
        if target[0] == 'name':
            sc = self.lookup(scopes, target[1])
            old = sc[target[1]]
            sc[target[1]] = convert(value, old.k)
            return
        if target[0] == 'member' and target[1][0] == 'name':
            sc = self.lookup(scopes, target[1][1])
            old = sc[target[1][1]].copy()
            idx = [SWZ[c] for c in target[2]]
            v = convert(value, old.k)
            if v.n() == 1 and len(idx) > 1:
                v = V(v.k, v.a * len(idx))
            for j, i in enumerate(idx):
                old.a[i] = v.a[j]
            sc[target[1][1]] = old
            return
        if target[0] == 'index' and target[1] == ('member', ('name', 'theft4_probe'), 'v'):
            return  # the instrumentation's own stores
        if target[0] == 'index' and target[1] == ('name', 'gl_ClipDistance'):
            self.env['gl_ClipDistance'] = convert(value, 'float')
            return
        raise Unsupported('store ' + str(target)[:60])

    def eval(self, e, scopes):
        k = e[0]
        if k == 'lit':
            return V('float' if e[1] == 'num' else e[1], [e[2]])
        if k == 'name':
            return self.lookup(scopes, e[1])[e[1]]
        if k == 'member':
            base = self.eval(e[1], scopes)
            return V(base.k, [base.a[SWZ[c]] for c in e[2]])
        if k == 'index':
            base = e[1]
            i = convert(self.eval(e[2], scopes), 'int').a[0]
            if base[0] == 'member' and base[1][0] == 'name':
                if base[2] == 'g_DkVertexConstants':
                    return V('float', self.probe.bank[i] if 0 <= i < 256 else [0, 0, 0, 0])
                if base[2] == 'g_DkShared':
                    return V('uint', self.probe.shared[i] if 0 <= i < 64 else [0, 0, 0, 0])
            v = self.eval(base, scopes)
            return V(v.k, [v.a[i]])
        if k == '?':
            return self.eval(e[2] if self.eval(e[1], scopes).a[0] else e[3], scopes)
        if k == 'un':
            v = self.eval(e[2], scopes)
            if e[1] == '-':
                return V(v.k, [-x for x in v.a])
            if e[1] == '!':
                return V('bool', [not x for x in v.a])
            if e[1] == '~':
                return V(v.k, [~x for x in v.a])
            return v
        if k == 'bin':
            return self.binary(e[1], self.eval(e[2], scopes), self.eval(e[3], scopes))
        if k == 'call':
            return self.call(e[1], [self.eval(a, scopes) for a in e[2]])
        raise Unsupported('eval ' + k)

    def binary(self, op, a, b):
        if op in ('&&', '||'):
            x, y = bool(a.a[0]), bool(b.a[0])
            return V('bool', [(x and y) if op == '&&' else (x or y)])
        a, b = broadcast(a, b)
        if a.k != b.k:
            kind = 'float' if 'float' in (a.k, b.k) else ('uint' if 'uint' in (a.k, b.k) else 'int')
            a, b = convert(a, kind), convert(b, kind)
        out = []
        for x, y in zip(a.a, b.a):
            if op == '==':
                out.append(x == y)
            elif op == '!=':
                out.append(x != y)
            elif op == '<':
                out.append(x < y)
            elif op == '>':
                out.append(x > y)
            elif op == '<=':
                out.append(x <= y)
            elif op == '>=':
                out.append(x >= y)
            elif op == '+':
                out.append(x + y)
            elif op == '-':
                out.append(x - y)
            elif op == '*':
                out.append(x * y)
            elif op == '/':
                if a.k == 'float':
                    out.append(x / y if y != 0 else (math.copysign(math.inf, x) * math.copysign(1, y) if x and not math.isnan(x) else math.nan))
                else:
                    out.append(int(x / y) if y else 0)
            elif op == '%':
                out.append(x % y if y else 0)
            elif op == '&':
                out.append(x & y)
            elif op == '|':
                out.append(x | y)
            elif op == '^':
                out.append(x ^ y)
            elif op == '<<':
                out.append(x << (y & 31))
            elif op == '>>':
                out.append(x >> (y & 31))
            else:
                raise Unsupported('op ' + op)
        if op in ('==', '!=', '<', '>', '<=', '>='):
            return V('bool', out)
        return V(a.k, out)

    def call(self, f, args):
        if f in TYPES:
            kind, n = TYPES[f]
            comps = []
            for a in args:
                comps.extend(convert(a, kind).a)
            if len(comps) == 1 and n > 1:
                comps = comps * n
            return V(kind, comps[:n])
        if f == 'floatBitsToUint':
            return V('uint', [f32_bits(x) for x in args[0].a])
        if f == 'floatBitsToInt':
            return V('int', [f32_bits(x) for x in args[0].a])
        if f in ('uintBitsToFloat', 'intBitsToFloat'):
            return V('float', [bits_f32(int(x)) for x in args[0].a])
        if f in ('equal', 'notEqual', 'greaterThan', 'greaterThanEqual', 'lessThan', 'lessThanEqual'):
            op = {'equal': '==', 'notEqual': '!=', 'greaterThan': '>', 'greaterThanEqual': '>=', 'lessThan': '<',
                  'lessThanEqual': '<='}[f]
            return self.binary(op, args[0], args[1])
        if f == 'mix':
            a, b, t = args
            a, b = broadcast(a, b)
            if t.k == 'bool':
                if t.n() == 1 and a.n() > 1:
                    t = V('bool', t.a * a.n())
                return V(a.k, [y if c else x for x, y, c in zip(a.a, b.a, t.a)])
            a, t = broadcast(a, convert(t, 'float'))
            b, t = broadcast(b, t)
            return V('float', [f32(f32(x * f32(1 - c)) + f32(y * c)) for x, y, c in zip(a.a, b.a, t.a)])
        if f == 'dk_sel':  # XenosRecomp's select helper: c ? a : b, every overload
            return (args[1] if args[0].a[0] else args[2]).copy()
        if f in ('min', 'max'):
            a, b = broadcast(*args)
            if a.k != b.k:
                a, b = convert(a, 'float'), convert(b, 'float')
            pick = []
            for x, y in zip(a.a, b.a):
                if a.k == 'float' and (math.isnan(x) or math.isnan(y)):
                    pick.append(y if math.isnan(x) else x)  # NVIDIA FMNMX: the non-NaN operand
                else:
                    pick.append(min(x, y) if f == 'min' else max(x, y))
            return V(a.k, pick)
        if f == 'clamp':
            return self.call('min', [self.call('max', [args[0], args[1]]), args[2]])

        def each(fn):
            out = []
            for x in args[0].a:
                try:
                    out.append(fn(x))
                except (ValueError, OverflowError):
                    out.append(math.nan)
            return out

        if f == 'abs':
            return V(args[0].k, [abs(x) for x in args[0].a])
        if f == 'floor':
            return V('float', each(lambda x: float(math.floor(x)) if math.isfinite(x) else x))
        if f == 'trunc':
            return V('float', each(lambda x: float(math.trunc(x)) if math.isfinite(x) else x))
        if f == 'fract':
            return V('float', each(lambda x: f32(x - math.floor(x)) if math.isfinite(x) else math.nan))
        if f == 'sqrt':
            return V('float', each(lambda x: math.sqrt(x) if x >= 0 else math.nan))
        if f == 'inversesqrt':
            return V('float', each(lambda x: (1 / math.sqrt(x) if x > 0 else (math.inf if x == 0 else math.nan))))
        if f in ('sin', 'cos', 'exp2', 'log2'):
            fn = {'sin': math.sin, 'cos': math.cos, 'exp2': lambda x: 2.0 ** x,
                  'log2': lambda x: math.log2(x) if x > 0 else (-math.inf if x == 0 else math.nan)}[f]
            return V('float', each(fn))
        if f == 'sign':
            return V(args[0].k, [(x > 0) - (x < 0) for x in args[0].a])
        if f == 'dot':
            a, b = broadcast(args[0], args[1])
            acc = 0.0
            for x, y in zip(a.a, b.a):
                acc = f32(acc + f32(x * y))
            return V('float', [acc])
        if f == 'any':
            return V('bool', [any(args[0].a)])
        if f == 'all':
            return V('bool', [all(args[0].a)])
        raise Unsupported('function ' + f)


# ---------------------------------------------------------------------------------------------------- the comparison


def bits_of(v):
    """A value as the probe stores it: 4 uint32 of raw bits."""
    if v.k == 'float':
        b = [f32_bits(x) for x in v.a]
    else:
        b = [u32n(int(x)) for x in v.a]
    return (b + [0, 0, 0, 0])[:4]


def differs(kind, gpu_bits, cpu_bits, n):
    if kind != 'float':
        return list(gpu_bits[:n]) != list(cpu_bits[:n])
    for gb, cb in zip(gpu_bits[:n], cpu_bits[:n]):
        x, y = bits_f32(gb), bits_f32(cb)
        if math.isnan(x) and math.isnan(y):
            continue
        if math.isnan(x) or math.isnan(y) or math.isinf(x) != math.isinf(y):
            return True
        if math.isinf(x):
            if x != y:
                return True
            continue
        if abs(x - y) > max(1e-4 * max(abs(x), abs(y)), 1e-5):
            return True
    return False


def compare(probe, vertices=None):
    """Per vertex: the first variable (program order) whose GPU value differs from the PC's, or None."""
    interp = Interpreter(probe.glsl)
    results = []
    count = probe.vertices if vertices is None else min(vertices, probe.vertices)
    for v in range(count):
        gpu = probe.values[v]
        if all(all(x == 0xFFFFFFFF for x in slot) for slot in gpu):
            results.append((v, 'not run', None))
            continue
        env = interp.run(probe, v)
        first = None
        for slot, (typ, name) in enumerate(probe.vars):
            kind, n = TYPES[typ]
            cpu = env.get(name)
            if cpu is None:
                continue
            cb = bits_of(cpu)
            if differs(kind, gpu[slot], cb, n):
                first = (slot, name, typ, gpu[slot], cb)
                break
        results.append((v, 'same' if first is None else 'differs', first))
    return results


def compare_positions(probe, limit=None):
    """Run 55 (version 2): gl_Position of every captured vertex, the GPU's against the PC's.
    Returns (pc positions as floats, counts dict, worst (vertex, gpu, pc) or None)."""
    interp = Interpreter(probe.glsl)
    n = len(probe.positions) if limit is None else min(limit, len(probe.positions))
    pcs, worst, worst_err = [], None, 0.0
    counts = {'same': 0, 'differ': 0, 'not run': 0}
    for v in range(n):
        gpu = probe.positions[v]
        env = interp.run(probe, v)
        pc = env['gl_Position'].a
        pcs.append(pc)
        if all(x == 0xFFFFFFFF for x in gpu):
            counts['not run'] += 1
            continue
        if differs('float', gpu, bits_of(env['gl_Position']), 4):
            counts['differ'] += 1
            g = [bits_f32(x) for x in gpu]
            err = max(abs(a - b) for a, b in zip(g, pc))
            if err > worst_err or worst is None:
                worst_err, worst = err, (v, g, pc)
        else:
            counts['same'] += 1
    return pcs, counts, worst


def triangles(probe):
    """The draw's triangles as vertex numbers (gl_VertexID), for the Xenos primitive types the probe sees."""
    idx = probe.indices
    out = []
    if probe.primitive in (4, 8):  # triangle list (rectangle lists draw no index list here)
        for i in range(0, len(idx) - 2, 3):
            t = idx[i:i + 3]
            if 0xFFFFFFFF not in t:
                out.append(t)
    elif probe.primitive in (5, 6):  # fan, strip; all ones restarts
        run = []
        for v in idx + [0xFFFFFFFF]:
            if v == 0xFFFFFFFF:
                for k in range(len(run) - 2):
                    if probe.primitive == 6:
                        out.append([run[k], run[k + 1], run[k + 2]] if k % 2 == 0 else [run[k + 1], run[k], run[k + 2]])
                    else:
                        out.append([run[0], run[k + 1], run[k + 2]])
                run = []
            else:
                run.append(v)
    return out


def _png(path, width, height, pixels):
    """A grey 8-bit PNG without libraries."""
    import zlib
    rows = b''.join(b'\0' + bytes(pixels[y * width:(y + 1) * width]) for y in range(height))

    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def draw_mesh(probe, pc_positions, path, scale=2):
    """The mesh's edges through the draw's viewport (Direct3D's: y down), the GPU's positions left, the PC's right."""
    vx, vy, vw, vh, _, _ = probe.viewport
    tw, th = probe.target
    w, h = max(1, tw // scale), max(1, th // scale)
    pixels = bytearray(2 * w * h)

    def screen(clip):
        x, y, _, cw = clip
        if not (cw > 1e-6) or any(math.isnan(c) or math.isinf(c) for c in clip):
            return None
        return ((vx + (x / cw + 1) * 0.5 * vw) / scale, (vy + (1 - y / cw) * 0.5 * vh) / scale)

    def line(a, b, offset):
        (x0, y0), (x1, y1) = a, b
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        if steps > 4 * (w + h):
            return  # a vertex far outside the screen: not a picture of anything
        for s in range(steps + 1):
            t = s / steps
            x, y = int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t)
            if 0 <= x < w and 0 <= y < h:
                pixels[y * 2 * w + offset + x] = 255

    gpu = [[bits_f32(c) for c in p] for p in probe.positions]
    for offset, positions in ((0, gpu), (w, pc_positions)):
        pts = [screen(p) for p in positions]
        for t in triangles(probe):
            if max(t) >= len(pts):
                continue
            a, b, c = pts[t[0]], pts[t[1]], pts[t[2]]
            if a and b and c:
                line(a, b, offset)
                line(b, c, offset)
                line(c, a, offset)
    for y in range(h):  # the divider
        pixels[y * 2 * w + w] = 128
    _png(path, 2 * w, h, pixels)


def describe(typ, bits):
    kind, n = TYPES[typ]
    if kind == 'float':
        return '(' + ', '.join(f'{bits_f32(x):.6g}' for x in bits[:n]) + ')'
    return '(' + ', '.join(str(int(x)) for x in bits[:n]) + ')'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('probe')
    ap.add_argument('--vertices', type=int, default=None)
    ap.add_argument('--all', action='store_true', help='print every differing vertex, not only the first 8')
    ap.add_argument('--png', help='version 2: the mesh picture (default: <probe>.png)')
    args = ap.parse_args()
    p = read_probe(open(args.probe, 'rb').read())
    print(f'vertex shader {p.hash:016X} variant {p.value}, draw {p.sequence}: {p.vertices} vertices, '
          f'{p.slots} values each')
    try:
        results = compare(p, args.vertices)
    except Unsupported as e:
        print(f'NOT COMPARED: {e}')
        return 2
    position_differ = 0
    if p.version >= 2 and p.positions:
        pcs, counts, worst = compare_positions(p)
        position_differ = counts['differ']
        tris = triangles(p)
        print(f'positions (version 2): {len(p.positions)} vertices: {counts["same"]} same as the PC, {counts["differ"]} '
              f'differ, {counts["not run"]} not run; {len(tris)} triangles (Xenos primitive {p.primitive}, '
              f'{len(p.indices)} indices); target {p.target[0]}x{p.target[1]}, viewport {p.viewport}; depth test '
              f'{p.depth_enable} function {p.depth_function} write {p.depth_write}, cull {p.cull_mode}, pixel shader '
              f'{p.pixel_shader:016X}')
        if worst:
            print(f'    worst vertex {worst[0]}: GPU {worst[1]} PC {worst[2]}')
        png = args.png or args.probe + '.png'
        draw_mesh(p, pcs, png)
        print(f'mesh picture (GPU left, PC right): {png}')
    lines = p.glsl.split('\n')
    differ = [r for r in results if r[1] == 'differs']
    print(f'vertices: {sum(r[1] == "same" for r in results)} same, {len(differ)} differ, '
          f'{sum(r[1] == "not run" for r in results)} not run by the GPU')
    shown = 0
    firsts = {}
    for v, state, first in results:
        if state != 'differs':
            continue
        firsts[first[1]] = firsts.get(first[1], 0) + 1
        if shown < 8 or args.all:
            shown += 1
            slot, name, typ, g, c = first
            src = next((l.strip() for l in lines if re.search(r'\b' + re.escape(name) + r'\b(\.\w+)?\s*=', l)), '')
            print(f'vertex {v}: first difference at {name} ({typ}): GPU {describe(typ, g)} PC {describe(typ, c)}')
            print(f'    {src[:200]}')
    if firsts:
        print('first differing variable, by vertices: ' + ', '.join(
            f'{k} {n}' for k, n in sorted(firsts.items(), key=lambda x: -x[1])))
    return 1 if differ or position_differ else 0


if __name__ == '__main__':
    sys.exit(main())
