#!/usr/bin/env python3
"""NVK's shader compiler (NAK) without its "Cannot flip unop" panic, by patching the built libvulkan.a (run 51).

    python tools/switch-game/nak_flip_patch.py <libvulkan.a> <patched libvulkan.a> [--check]

On Maxwell (the Switch's GM20B) NAK swaps the two sources of a float compare when only the second is a register, and
flips the comparison to match: FloatCmpOp::flip() (src/nouveau/compiler/nak/ir.rs) panics for IsNum / IsNan
(ford / funord: isnan(a) || isnan(b) and its negation), which are symmetric, so the flip is the operation itself.
kabba536's GTA IV port found GTA IV's shaders hit it thousands of times (every such pipeline fails: a black screen)
and rebuilds Mesa with `FloatCmpOp::IsNum | FloatCmpOp::IsNan => self` (source/build_nvk.sh, used with permission).
Rebuilding Mesa needs a Rust cross toolchain this machine does not have, so the same change is made in the binary:
rustc compiled flip() inline into each `legalize` of the set ops as a jump table indexed by the compare op (one byte
per op, the target = table base + 4 * byte). The bytes for IsNum (12) and IsNan (13) point at the panic; they are set
to OrdEq's byte (0), whose case keeps the op unchanged (`OrdEq | OrdNe => self`). Nothing else moves: same sizes, same
relocations. The result is checked by decoding: every patched table must have had exactly the panic target at 12 and
13, and OrdEq's target must be the store of the unchanged op.
"""
import struct
import sys

PANIC = b'Cannot flip unop'
FLOAT_CMP_ORD_EQ, FLOAT_CMP_IS_NUM, FLOAT_CMP_IS_NAN = 0, 12, 13


def members(data):
    assert data[:8] == b'!<arch>\n', 'not an ar archive'
    p, names = 8, b''
    while p < len(data):
        h = data[p:p + 60]
        name = h[:16].decode().strip()
        size = int(h[48:58])
        start = p + 60
        if name == '//':
            names = data[start:start + size]
        elif name.startswith('/') and name[1:].isdigit():
            o = int(name[1:])
            name = names[o:names.index(b'\n', o)].decode().rstrip('/')
        yield name, start, size
        p = start + size + (size & 1)


class Elf:
    def __init__(self, d):
        self.d = d
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
        self.secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize) for i in range(shnum)]
        self.shstr = self.secs[shstrndx]
        symsec = next(s for s in self.secs if s[1] == 2)
        self.symstr = self.secs[symsec[6]]
        self.syms = [struct.unpack_from('<IBBHQQ', d, symsec[4] + i * 24) for i in range(symsec[5] // 24)]

    def secname(self, i):
        o = self.shstr[4] + self.secs[i][0]
        return self.d[o:self.d.index(b'\0', o)].decode()

    def relas(self, target):
        for s in self.secs:
            if s[1] == 4 and s[7] == target:
                for j in range(s[5] // 24):
                    off, info, add = struct.unpack_from('<QQq', self.d, s[4] + j * 24)
                    yield off, info >> 32, info & 0xffffffff, add


def adr_target(word, pc):
    # ADR: immlo bits 29-30, immhi bits 5-23
    imm = ((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return pc + imm


def patch_object(d):
    """Returns (patched bytes, report lines). d is one archive member (an ELF object)."""
    if PANIC not in d:
        return d, []
    elf = Elf(d)
    out = bytearray(d)
    report = []
    panic_sec = next(i for i, s in enumerate(elf.secs)
                     if s[1] == 1 and s[4] <= d.find(PANIC) < s[4] + s[5])
    panic_off = d.find(PANIC) - elf.secs[panic_sec][4]
    for ti, ts in enumerate(elf.secs):
        name = elf.secname(ti) if ts[1] == 1 else ''
        if not name.startswith('.text.') or 'legalize' not in name:
            continue
        relocs = list(elf.relas(ti))
        panic_at = [off for off, sym, typ, add in relocs
                    if elf.syms[sym][3] == panic_sec and elf.syms[sym][4] + add == panic_off and typ == 275]
        if not panic_at:
            continue
        table_name = '.rodata.' + name[len('.text.'):]
        table_sec = next((i for i in range(len(elf.secs)) if elf.secname(i) == table_name), None)
        code = d[ts[4]:ts[4] + ts[5]]
        # adr x10, base right after the adrp/add of the table
        base = None
        for k in range(0, len(code) - 4, 4):
            w, = struct.unpack_from('<I', code, k)
            if (w & 0x9f000000) == 0x10000000:  # ADR
                base = adr_target(w, k)
                break
        if table_sec is None or base is None:
            report.append(f'{name}: panic reference but no byte jump table found: NOT patched')
            continue
        tbl = elf.secs[table_sec]
        table = bytearray(d[tbl[4]:tbl[4] + tbl[5]])
        if len(table) < 14:
            report.append(f'{name}: table of {len(table)} bytes: NOT patched')
            continue
        targets = [base + 4 * b for b in table[:14]]
        panic_target = panic_at[0]
        if targets[FLOAT_CMP_IS_NUM] != panic_target or targets[FLOAT_CMP_IS_NAN] != panic_target:
            report.append(f'{name}: IsNum/IsNan do not lead to the panic (0x{targets[12]:x}, 0x{targets[13]:x}, '
                          f'panic 0x{panic_target:x}): NOT patched')
            continue
        if panic_target in targets[:12]:
            report.append(f'{name}: an ordered compare also leads to the panic: NOT patched')
            continue
        table[FLOAT_CMP_IS_NUM] = table[FLOAT_CMP_ORD_EQ]
        table[FLOAT_CMP_IS_NAN] = table[FLOAT_CMP_ORD_EQ]
        out[tbl[4]:tbl[4] + len(table)] = table
        report.append(f'{name}: IsNum/IsNan 0x{panic_target:x} (panic) -> 0x{targets[0]:x} (as OrdEq: unchanged)')
    return bytes(out), report


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, 'rb').read())
    patched = 0
    for name, start, size in members(bytes(data)):
        body = bytes(data[start:start + size])
        new, report = patch_object(body)
        for line in report:
            print(f'{name}: {line}')
        if new != body:
            data[start:start + size] = new
            patched += sum(1 for line in report if '(panic) ->' in line)
    if not patched:
        print('nothing patched')
        return 1
    open(dst, 'wb').write(data)
    print(f'patched {patched} jump tables -> {dst}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
