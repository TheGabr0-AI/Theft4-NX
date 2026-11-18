"""gen_biquad_native.py: writes ultimate/app/src/cpu/hot/gta4_biquad_native.inc, the hand-written native of GTA IV's
filter sub_828C2D28 (Theft4-NX U18 MAX-B, 2026-11-18; docs/switch-port/20 section 2.28).

    python tools/ultimate/gen_biquad_native.py [--copy ultimate/app/generated/hot/n_828C2D28.h] [--out <inc>]

What sub_828C2D28 is (its PowerPC, app/generated/renut_recomp.*.cpp): two optional filters of the audio mixer thread,
each a cascade of two identical biquads (transposed direct form II: five coefficients b0 b1 b2 a1 a2 and two states per
biquad), over a channel's 256 samples in place: 32 passes of 8 samples. Filter A (flag r30) keeps its coefficients at
144-160(r1) and its four states at 164-176(r1); filter B (flag r27) at 96-112(r1) and 116-128(r1). The coefficients are
copied to the stack before the loop (sub_829A0E58) and recomputed when the voice's parameters change (sub_828C2C18).
Console profile U18 MAX-A (street): 16.3 % of a core, the mixer thread's second function after the router.

Where its time goes: every coefficient and state is a guest stack slot. Each sample of each biquad pair loads 9 of them
(a byte-swapped 32-bit load and a single-to-double conversion each) and stores 4 (a byte swap and a store): about 70
memory operations around 14 floating-point ones a sample a filter.

What the native does: the copy's own statements (read from the processed copy by this script, so everything outside the
loop is the copy by construction), with the loop (loc_828C2EDC .. its back edge) run a second time, in front of the
original, with each stack slot of 96-176(r1) a local float: the SAME statements in the SAME order (REX_FMADDS_KNOWN,
REX_FMULS, REX_FADDS, the same conversions to and from single), only the slot's guest load replaced by the local and its
guest store by an assignment of the same bits. The slots the original's loop stores (the states of a filter whose flag
is set) are written back to the stack at the loop's exit, so the memory after the loop is the original's. The samples'
loads and stores stay guest accesses in the original order.

The block declines (the original loop runs) when the samples the loop touches (r11 - 8 .. r11 + 1016) overlap the
slots (r1 + 96 .. r1 + 180), or when the key gta4_hand_biquad is off.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COPY = os.path.join(ROOT, 'ultimate', 'app', 'generated', 'hot', 'n_828C2D28.h')
OUT = os.path.join(ROOT, 'ultimate', 'app', 'src', 'cpu', 'hot', 'gta4_biquad_native.inc')
SLOTS = list(range(96, 180, 4))   # 96..176
STATES_A, STATES_B = (164, 168, 172, 176), (116, 120, 124, 128)


def body_of(text, name):
    m = re.search(r'template <class S>\n(?:\[\[[^\]]*\]\] )?static void f_%s\(PPCContext& __restrict ctx, uint8_t\* base, '
                  r'\[\[maybe_unused\]\] S& s\) \{\n' % name, text)
    if not m:
        sys.exit('gen_biquad_native: no template f_%s in the copy' % name)
    end = text.index('\n}\n', m.end())
    return text[m.end():end + 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--copy', default=COPY)
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    text = open(a.copy, encoding='utf-8').read()
    body = body_of(text, 'sub_828C2D28')
    lines = body.split('\n')

    def find(pattern, start=0):
        for i in range(start, len(lines)):
            if re.search(pattern, lines[i]):
                return i
        sys.exit('gen_biquad_native: %r not found' % pattern)

    e0 = find(r'^loc_F0000001:')
    e1 = find(r'^\tgoto loc_828C2EF0;$', e0)          # the end of the loop's only entry block
    r0 = find(r'^loc_828C2EDC:', e1)
    r1 = find(r'^\tif \(!cr6\.eq\) goto loc_828C2EDC;$', r0)  # the back edge
    if find(r'goto loc_F0000001;') > e0:
        sys.exit('gen_biquad_native: the loop entry is not where the script expects it')
    region = lines[r0:r1 + 1]
    src = '\n'.join(region)
    # The loop as the script expects it: no call, r1 used only for the slots, the samples through ll_r11, 32 passes.
    # (ctx.fpscr's flush-mode statements stay as they are: the same mode for the same arithmetic.)
    for bad in (r'f_sub_', r'\bctx\.(?!fpscr\.(?:disable|enable)FlushMode\(\);)', r'\bl_r1\.'):
        if re.search(bad, src):
            sys.exit('gen_biquad_native: the loop has %r' % bad)
    entry = '\n'.join(lines[e0:e1 + 1])
    if 'REX_LOOP_IN(ll_r1, l_r1);' not in entry or 'REX_LOOP_IN(ll_r11, l_r11);' not in entry:
        sys.exit('gen_biquad_native: the entry block does not set ll_r1 / ll_r11')
    if 'l_r10.s64 = 32;' not in '\n'.join(lines[e0 - 30:e0]):
        sys.exit('gen_biquad_native: the pass count (32) is not set before the loop')
    fast = []
    loads = stores = 0
    for ln in region:
        ln2 = re.sub(r'\bloc_828C', 'nb_loc_828C', ln)
        def ld(m):
            nonlocal loads
            k = int(m.group(1))
            if k not in SLOTS:
                sys.exit('gen_biquad_native: a load of %d(r1) in the loop' % k)
            loads += 1
            return 'nb_s%d' % k
        ln2 = re.sub(r'REX_LOAD_D_F32\(ll_r1\.u32, (-?\d+)\)', ld, ln2)
        m = re.match(r'^\tBANJO_HOT_STORE_D_U32\(ll_r1\.u32, (-?\d+), temp\.u32\);$', ln2)
        if m:
            k = int(m.group(1))
            if k not in STATES_A + STATES_B:
                sys.exit('gen_biquad_native: a store to %d(r1) in the loop' % k)
            stores += 1
            ln2 = '\tnb_s%d = temp.f32;' % k
        if re.search(r'\bll_r1\b', ln2):
            sys.exit('gen_biquad_native: another use of r1 in the loop: %s' % ln.strip())
        fast.append('\t' + ln2 if not ln2.startswith('nb_loc_') else ln2)
    used = [k for k in SLOTS if any('nb_s%d)' % k in f or 'nb_s%d =' % k in f for f in fast)]
    decl = '\t\tfloat ' + ', '.join('nb_s%d = REX_LOAD_D_F32(l_r1.u32, %d)' % (k, k) for k in used) + ';'
    back = ['\t\tif (r30.u32 != 0) {'] + [
        '\t\t\t{ PPCRegister nb_t; nb_t.f32 = nb_s%d; BANJO_HOT_STORE_D_U32(l_r1.u32, %d, nb_t.u32); if (s.Stopped()) return; }' % (k, k)
        for k in STATES_A] + ['\t\t}', '\t\tif (r27.u32 != 0) {'] + [
        '\t\t\t{ PPCRegister nb_t; nb_t.f32 = nb_s%d; BANJO_HOT_STORE_D_U32(l_r1.u32, %d, nb_t.u32); if (s.Stopped()) return; }' % (k, k)
        for k in STATES_B] + ['\t\t}']
    block = ([
        '\t// ---- Theft4-NX native (tools/ultimate/gen_biquad_native.py): the loop with the stack slots 96-176(r1) in locals ----',
        '\tif (::banjo::hand::On(::banjo::hand::kRageBiquad) &&',
        '\t    !(uint64_t(l_r11.u32) - 8u < uint64_t(l_r1.u32) + 180u && uint64_t(l_r1.u32) + 96u < uint64_t(l_r11.u32) + 1016u)) {',
        decl,
        # banjo_hot_count only (a no-op in the real and the guard's forms): the fast path's calls, by which filters
        # are on (1 = A, 2 = B, 3 = both), so a PC run shows that the block runs and not the original loop.
        '\t\ts.Hand(::banjo::hand::kRageBiquad, (r30.u32 != 0 ? 1u : 0u) + (r27.u32 != 0 ? 2u : 0u));',
        '\t\tgoto nb_loc_828C2EF0;'] + fast + back + ['\t\tgoto nb_biquad_done;', '\t}'])
    out = lines[:e1] + block + lines[e1:r1 + 1] + ['nb_biquad_done:;'] + lines[r1 + 1:]
    native = '\n'.join(out)
    native = re.sub(r'\breturn;', 'return true;', native)
    for callee in ('82168520', '829A0E58', '828C2C18'):
        if 'f_sub_%s(ctx, base, s)' % callee not in native:
            sys.exit('gen_biquad_native: the call of sub_%s is not where the script expects it' % callee)
        native = native.replace('f_sub_%s(ctx, base, s)' % callee, 'C::sub_%s(ctx, base, s)' % callee)
    if re.search(r'\bf_sub_', native):
        sys.exit('gen_biquad_native: another callee copy in the body: %s' % re.search(r'\bf_sub_\w+', native).group(0))
    srcname = os.path.relpath(a.copy, ROOT).replace(os.sep, '/')
    head = ('// GENERATED by tools/ultimate/gen_biquad_native.py from %s (Theft4-NX U18 MAX-B, 2026-11-18): the\n'
            '// hand-written native of GTA IV\'s filter sub_828C2D28 (hand.txt; key gta4_hand_biquad): the copy\'s statements\n'
            '// with its loop run a second time, in front, with the stack slots 96-176(r1) in locals; see the script\'s\n'
            '// docstring. Included by banjo_hand_natives.h inside namespace banjo::hand. Do not edit: run the script again\n'
            '// on a new copy.\n' % srcname)
    text_out = (head + '\nnamespace n_rage_biquad {\n\ntemplate <class C, class S>\n'
                'inline bool Native(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s) {\n'
                '\tif (!On(kRageBiquad)) [[unlikely]] return false;\n' + native + '}\n\n}  // namespace n_rage_biquad\n')
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text_out)
    print('gen_biquad_native: %s (%d lines; loop %d-%d of the copy body, %d slot loads and %d slot stores in locals)' %
          (os.path.relpath(a.out, ROOT), text_out.count('\n'), r0, r1, loads, stores))


if __name__ == '__main__':
    main()
