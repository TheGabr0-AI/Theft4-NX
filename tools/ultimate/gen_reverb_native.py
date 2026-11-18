"""gen_reverb_native.py: writes ultimate/app/src/cpu/hot/gta4_reverb_native.inc, the hand-written native of GTA IV's
reverb sub_828C6740 (Theft4-NX U18 MAX, 2026-11-17; docs/switch-port/20 section 2.27).

    python tools/ultimate/gen_reverb_native.py [--copy ultimate/app/generated/hot/n_828C6740.h] [--out <inc>]

What sub_828C6740 is (read from its PowerPC, app/generated/renut_recomp.*.cpp): a Freeverb-style reverb of the audio
mixer thread. For each channel of the voice (bit mask at 78(r31), count from sub_82168520's struct at 89(r1)) it runs
8 comb filters (Freeverb's lengths 1116 1188 1276 1356 1424 1491 1556 1616 at 0x82095558) four samples per VMX vector,
the damping recursion as 4x4 dot products (vmsum4fp128), over 64 steps of the channel's 256 samples, then 4 allpass
filters (556 440 340 224) over the comb sum, and writes wet * v29 + dry * v30 over the input.
Console profile U18-1 B0: 15.5 % of the mixer thread (XThread at 0x2D, 100-110 % of a core: the audio's limit).

Where its time goes (the copy's ARM code, objdump of the U18-1 ELF): per comb per step the copy reads two guest pointer
tables through byte-swapped loads (16(r31) -> per-channel index array, 8(r31) -> per-channel buffer array), the comb's
index twice, its length, divides for the ring index (divwu), reloads the filter state from the stack, and checks a
store card at each of its two stores: about 80 instructions around 25 of arithmetic.

What the native does: the copy's own statements (this script reads them from the processed copy, so everything outside
the two loops is the copy by construction: the frame, every stack store, the callee, every register written back),
with the two innermost loop nests (the combs' 8 steps of a pass, loc_828C6B34; the allpasses' 8 steps, loc_828C6DC0)
each replaced by a block that keeps the tables, indices, lengths and filter states in registers for the 8 steps and
runs the SAME simde operations and REX_VMADDFP in the SAME order on the same values, each comb's and allpass's load and
store in the original order (so overlapping buffers stay exact), the indices and states written back at the block's
end, and every register the original leaves at the loop's exit set to the original's value (exact= in hand.txt).
The ring index (idx + 4) mod length without a divide unless idx + 4 >= 2 * length.

A block falls back to the original statements (kept verbatim behind it) when a length is 0 (the original traps) or
when anything the block reads once could be written by its own stores: the pointer words at 8/16 (44/48) of the
object, the two table entries, the index, buffer and length tables, 96(r1), the filter states, against every comb
(allpass) buffer range of the pass, the stack states and the comb sum (the allpasses' output) ranges.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COPY = os.path.join(ROOT, 'ultimate', 'app', 'generated', 'hot', 'n_828C6740.h')
OUT = os.path.join(ROOT, 'ultimate', 'app', 'src', 'cpu', 'hot', 'gta4_reverb_native.inc')

COMB_FAST = r'''
	// ---- Theft4-NX native (tools/ultimate/gen_reverb_native.py): the 8 combs over this pass's 8 steps ----
	{
		const uint32_t nb_buf = REX_LOAD_D_U32(l_r1.u32, 96);
		const uint32_t nb_p_idx = REX_LOAD_D_U32(r31.u32, 16), nb_p_base = REX_LOAD_D_U32(r31.u32, 8);
		const uint32_t nb_idx_tab = REX_LOAD_U32(l_r11.u32 + nb_p_idx), nb_base_tab = REX_LOAD_U32(l_r11.u32 + nb_p_base);
		const uint32_t nb_len_tab = r29.u32 - 32u;
		uint32_t nb_idx[8], nb_cb[8], nb_len[8];
		bool nb_ok = ::banjo::hand::On(::banjo::hand::kRageReverb) && l_r6.u32 == 8u;
		for (uint32_t k = 0; k < 8 && nb_ok; ++k) {
			nb_idx[k] = REX_LOAD_U32(nb_idx_tab + 4u * k);
			nb_cb[k] = REX_LOAD_U32(nb_base_tab + 4u * k);
			nb_len[k] = REX_LOAD_U32(nb_len_tab + 4u * k);
			nb_ok = nb_len[k] != 0;
		}
		const uint32_t nb_state = (l_r1.u32 + 176u) & ~0xFu;
		if (nb_ok) {
			// What the block writes: each comb's blocks over the 8 steps, the stack states, the comb sum.
			::banjo::hand::reverb::Range w[10];
			for (uint32_t k = 0; k < 8; ++k)
				w[k] = ::banjo::hand::reverb::RingRange(nb_cb[k], nb_idx[k], nb_len[k]);
			w[8] = ::banjo::hand::reverb::R(nb_state, 128u);
			w[9] = ::banjo::hand::reverb::R((l_r3.u32) & ~0xFu, 128u);
			// What it reads once.
			const ::banjo::hand::reverb::Range p[7] = {
				::banjo::hand::reverb::R(r31.u32 + 8u, 12u),
				::banjo::hand::reverb::R(l_r11.u32 + nb_p_idx, 4u),
				::banjo::hand::reverb::R(l_r11.u32 + nb_p_base, 4u),
				::banjo::hand::reverb::R(nb_idx_tab, 32u),
				::banjo::hand::reverb::R(nb_base_tab, 32u),
				::banjo::hand::reverb::R(nb_len_tab, 32u),
				::banjo::hand::reverb::R(l_r1.u32 + 96u, 4u)};
			for (uint32_t i = 0; i < 10 && nb_ok; ++i) {
				nb_ok = w[i].ok;
				for (uint32_t j = 0; j < 7 && nb_ok; ++j)
					nb_ok = !w[i].Overlaps(p[j]);
			}
			// The states are kept in registers: no comb block and not the comb sum may write them.
			for (uint32_t i = 0; i < 10 && nb_ok; ++i)
				nb_ok = i == 8 || !w[i].Overlaps(w[8]);
		}
		if (nb_ok) {
			ctx.fpscr.enableFlushMode();
			const simde__m128 v9 = simde_mm_load_ps(l_v9.f32), v8 = simde_mm_load_ps(l_v8.f32);
			const simde__m128 v7 = simde_mm_load_ps(l_v7.f32), v6 = simde_mm_load_ps(l_v6.f32);
			const simde__m128 v5 = simde_mm_load_ps(l_v5.f32), v10 = simde_mm_load_ps(l_v10.f32);
			const simde__m128 v4 = simde_mm_load_ps(l_v4.f32);
			const simde__m128i v2 = simde_mm_load_si128((simde__m128i*)l_v2.u8), v3 = simde_mm_load_si128((simde__m128i*)l_v3.u8);
			simde__m128i st[8];
			for (uint32_t k = 0; k < 8; ++k)
				st[k] = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(nb_state + 16u * k)));
			simde__m128i x0{}, x11{}, x12{}, x13{}, x25{}, x26{}, x27{}, x28{};
			uint32_t q_last = 0;
			uint64_t n_last = 0;
			for (uint32_t step = 0; step < 8; ++step) {
				x11 = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR((l_r4.u32 + nb_buf) & ~0xFu)));
				x12 = simde_mm_load_si128((simde__m128i*)l_v1.u8);
				for (uint32_t k = 0; k < 8; ++k) {
					x28 = simde_mm_castps_si128(simde_mm_mul_ps(v9, simde_mm_castsi128_ps(st[k])));
					ea = ((uint32_t)(nb_idx[k] << 2) + nb_cb[k]) & ~0xFu;
					const simde__m128i v0 = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)));
					const simde__m128 f0 = simde_mm_castsi128_ps(v0);
					x13 = simde_mm_castps_si128(simde_mm_dp_ps(v8, f0, 0xFF));
					x12 = simde_mm_castps_si128(simde_mm_add_ps(simde_mm_castsi128_ps(x12), f0));
					x27 = simde_mm_castps_si128(simde_mm_dp_ps(v6, f0, 0xFF));
					x26 = simde_mm_castps_si128(simde_mm_dp_ps(v7, f0, 0xFF));
					x25 = simde_mm_castps_si128(simde_mm_dp_ps(v5, f0, 0xFF));
					st[k] = x13;
					x27 = rex::ppc::simde_mm_perm_epi8_(x27, x13, v2);
					x0 = rex::ppc::simde_mm_perm_epi8_(x25, x26, v3);
					x0 = simde_mm_unpackhi_epi32(x27, x0);
					x0 = simde_mm_castps_si128(REX_VMADDFP(simde_mm_castsi128_ps(x0), v10, simde_mm_castsi128_ps(x28)));
					x0 = simde_mm_castps_si128(REX_VMADDFP(simde_mm_castsi128_ps(x11), v4, simde_mm_castsi128_ps(x0)));
					if (s.Note((u32)(ea), 16)) REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(x0));
					if (s.Stopped()) return true;
					n_last = uint64_t(nb_idx[k]) + 4u;  // the original's l_r7 / l_r8: the 32-bit index + 4 in 64 bits
					nb_idx[k] = ::banjo::hand::reverb::Ring(uint32_t(n_last), nb_len[k], q_last);
				}
				ea = (l_r3.u32) & ~0xFu;
				if (s.Note((u32)(ea), 16)) REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(x12));
				if (s.Stopped()) return true;
				l_r4.s64 = l_r4.s64 + 16;
				l_r3.s64 = l_r3.s64 + 16;
			}
			for (uint32_t k = 0; k < 8; ++k) {
				ea = nb_state + 16u * k;
				if (s.Note((u32)(ea), 16)) REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(st[k]));
				if (s.Stopped()) return true;
			}
			for (uint32_t k = 0; k < 8; ++k) {
				BANJO_HOT_STORE_D_U32_C(nb_idx_tab + 4u * k, 0, nb_idx[k]);
				if (s.Stopped()) return true;
			}
			// The registers as the original leaves them at the loop's exit (step 8, comb 8).
			simde_mm_store_si128((simde__m128i*)l_v0.u8, x0);
			simde_mm_store_si128((simde__m128i*)l_v11.u8, x11);
			simde_mm_store_si128((simde__m128i*)l_v12.u8, x12);
			simde_mm_store_si128((simde__m128i*)l_v13.u8, x13);
			simde_mm_store_si128((simde__m128i*)v25.u8, x25);
			simde_mm_store_si128((simde__m128i*)v26.u8, x26);
			simde_mm_store_si128((simde__m128i*)v27.u8, x27);
			simde_mm_store_si128((simde__m128i*)v28.u8, x28);
			l_r9.s64 = l_r1.s64 + 176 + 128;
			l_r10.s64 = 32;
			l_r8.u64 = uint64_t(nb_idx_tab) + 28u;
			r14.u64 = q_last;
			l_r5.s64 = int64_t(r14.s32) * int64_t(int32_t(nb_len[7]));
			l_r7.u64 = n_last - l_r5.u64;
			l_r6.s64 = 0;
			cr6.compare<uint32_t>(l_r6.u32, 0, xer);
			goto nb_comb_done;
		}
	}
'''

ALLPASS_FAST = r'''
	// ---- Theft4-NX native (tools/ultimate/gen_reverb_native.py): the 4 allpasses over this pass's 8 steps ----
	{
		const uint32_t nb_buf = REX_LOAD_D_U32(l_r1.u32, 96);
		const uint32_t nb_p_idx = REX_LOAD_D_U32(r31.u32, 48), nb_p_base = REX_LOAD_D_U32(r31.u32, 44);
		const uint32_t nb_idx_tab = REX_LOAD_U32(l_r11.u32 + nb_p_idx), nb_base_tab = REX_LOAD_U32(l_r11.u32 + nb_p_base);
		const uint32_t nb_len_tab = r29.u32;
		uint32_t nb_idx[4], nb_ab[4], nb_len[4];
		bool nb_ok = ::banjo::hand::On(::banjo::hand::kRageReverb) && l_r7.u32 == 8u;
		for (uint32_t k = 0; k < 4 && nb_ok; ++k) {
			nb_idx[k] = REX_LOAD_U32(nb_idx_tab + 4u * k);
			nb_ab[k] = REX_LOAD_U32(nb_base_tab + 4u * k);
			nb_len[k] = REX_LOAD_U32(nb_len_tab + 4u * k);
			nb_ok = nb_len[k] != 0;
		}
		if (nb_ok) {
			::banjo::hand::reverb::Range w[5];
			for (uint32_t k = 0; k < 4; ++k)
				w[k] = ::banjo::hand::reverb::RingRange(nb_ab[k], nb_idx[k], nb_len[k]);
			w[4] = ::banjo::hand::reverb::R((l_r3.u32 + nb_buf) & ~0xFu, 128u);
			const ::banjo::hand::reverb::Range p[7] = {
				::banjo::hand::reverb::R(r31.u32 + 44u, 8u),
				::banjo::hand::reverb::R(l_r11.u32 + nb_p_idx, 4u),
				::banjo::hand::reverb::R(l_r11.u32 + nb_p_base, 4u),
				::banjo::hand::reverb::R(nb_idx_tab, 16u),
				::banjo::hand::reverb::R(nb_base_tab, 16u),
				::banjo::hand::reverb::R(nb_len_tab, 16u),
				::banjo::hand::reverb::R(l_r1.u32 + 96u, 4u)};
			for (uint32_t i = 0; i < 5 && nb_ok; ++i) {
				nb_ok = w[i].ok;
				for (uint32_t j = 0; j < 7 && nb_ok; ++j)
					nb_ok = !w[i].Overlaps(p[j]);
			}
		}
		if (nb_ok) {
			ctx.fpscr.enableFlushMode();
			const simde__m128 v31f = simde_mm_load_ps(v31.f32), v30f = simde_mm_load_ps(v30.f32), v29f = simde_mm_load_ps(v29.f32);
			simde__m128i x0{}, x11{}, x12{}, x13{};
			uint32_t q_last = 0;
			uint64_t n_last = 0;
			for (uint32_t step = 0; step < 8; ++step) {
				x0 = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR((l_r4.u32) & ~0xFu)));
				x12 = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR((l_r3.u32 + nb_buf) & ~0xFu)));
				for (uint32_t k = 0; k < 4; ++k) {
					ea = ((uint32_t)(nb_idx[k] << 2) + nb_ab[k]) & ~0xFu;
					x13 = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)));
					x11 = simde_mm_castps_si128(REX_VMADDFP(simde_mm_castsi128_ps(x13), v31f, simde_mm_castsi128_ps(x0)));
					x0 = simde_mm_castps_si128(simde_mm_sub_ps(simde_mm_castsi128_ps(x13), simde_mm_castsi128_ps(x0)));
					if (s.Note((u32)(ea), 16)) REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(x11));
					if (s.Stopped()) return true;
					n_last = uint64_t(nb_idx[k]) + 4u;  // the original's l_r7 / l_r8: the 32-bit index + 4 in 64 bits
					nb_idx[k] = ::banjo::hand::reverb::Ring(uint32_t(n_last), nb_len[k], q_last);
				}
				x13 = simde_mm_castps_si128(simde_mm_mul_ps(simde_mm_castsi128_ps(x12), v30f));
				x0 = simde_mm_castps_si128(REX_VMADDFP(simde_mm_castsi128_ps(x0), v29f, simde_mm_castsi128_ps(x13)));
				ea = (l_r3.u32 + nb_buf) & ~0xFu;
				if (s.Note((u32)(ea), 16)) REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(x0));
				if (s.Stopped()) return true;
				l_r4.s64 = l_r4.s64 + 16;
				l_r3.s64 = l_r3.s64 + 16;
			}
			for (uint32_t k = 0; k < 4; ++k) {
				BANJO_HOT_STORE_D_U32_C(nb_idx_tab + 4u * k, 0, nb_idx[k]);
				if (s.Stopped()) return true;
			}
			simde_mm_store_si128((simde__m128i*)l_v0.u8, x0);
			simde_mm_store_si128((simde__m128i*)l_v11.u8, x11);
			simde_mm_store_si128((simde__m128i*)l_v12.u8, x12);
			simde_mm_store_si128((simde__m128i*)l_v13.u8, x13);
			l_r10.u64 = nb_buf;
			l_r9.u64 = uint64_t(nb_idx_tab) + 12u;
			l_r5.u64 = q_last;
			l_r6.s64 = int64_t(l_r5.s32) * int64_t(int32_t(nb_len[3]));
			l_r8.u64 = n_last - l_r6.u64;
			l_r7.s64 = 0;
			cr6.compare<uint32_t>(l_r7.u32, 0, xer);
			goto nb_allpass_done;
		}
	}
'''

HELPERS = r'''
namespace reverb {
// A guest address range [lo, hi) as 64-bit numbers; ok = false when it wraps past 4 GB (the block then declines).
struct Range {
  uint64_t lo = 0, hi = 0;
  bool ok = true;
  [[gnu::always_inline]] bool Overlaps(const Range& o) const { return lo < o.hi && o.lo < hi; }
};
// The 16-byte blocks a ring's 8 steps can touch: indices from idx (its first step) and every index below len (the
// following ones, (idx + 4) mod len), at base + 4 * index rounded down to 16.
[[gnu::always_inline]] inline Range RingRange(uint32_t base, uint32_t idx, uint32_t len) {
  const uint64_t top = uint64_t(idx > len - 1 ? idx : len - 1) * 4u + base;
  Range r;
  r.lo = base & ~0xFull;
  r.hi = (top & ~0xFull) + 16u;
  r.ok = r.hi <= 0x100000000ull;
  return r;
}
// [lo, lo + n) of a guest address (no wrap: lo is a 32-bit address, hi up to 4 GB + n).
[[gnu::always_inline]] inline Range R(uint32_t lo, uint32_t n) {
  Range r;
  r.lo = lo;
  r.hi = uint64_t(lo) + n;
  return r;
}
// (n) mod len as the original's divwu / mullw / subf make it (len != 0); q the quotient.
[[gnu::always_inline]] inline uint32_t Ring(uint32_t n, uint32_t len, uint32_t& q) {
  if (n < len) [[likely]] {
    q = 0;
    return n;
  }
  if (n - len < len) {
    q = 1;
    return n - len;
  }
  q = n / len;
  return n - q * len;
}
}  // namespace reverb
'''


def body_of(text, name):
    m = re.search(r'template <class S>\n(?:\[\[[^\]]*\]\] )?static void f_%s\(PPCContext& __restrict ctx, uint8_t\* base, '
                  r'\[\[maybe_unused\]\] S& s\) \{\n' % name, text)
    if not m:
        sys.exit('gen_reverb_native: no template f_%s in the copy' % name)
    end = text.index('\n}\n', m.end())
    return text[m.end():end + 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--copy', default=COPY)
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    text = open(a.copy, encoding='utf-8').read()
    body = body_of(text, 'sub_828C6740')
    lines = body.split('\n')

    def find(pattern, start=0):
        for i in range(start, len(lines)):
            if re.search(pattern, lines[i]):
                return i
        sys.exit('gen_reverb_native: %r not found' % pattern)

    c0 = find(r'^loc_828C6B34:')
    c1 = find(r'goto loc_828C6B34;', c0)
    p0 = find(r'^loc_828C6DC0:', c1)
    p1 = find(r'goto loc_828C6DC0;', p0)
    # The loops' own statements as the script expects them (a copy written otherwise stops here, not later).
    comb = '\n'.join(lines[c0:c1 + 1])
    allp = '\n'.join(lines[p0:p1 + 1])
    for need in ('REX_LOAD_D_U32(r31.u32, 16)', 'REX_LOAD_D_U32(r31.u32, 8)', 'simde_mm_dp_ps(simde_mm_load_ps(l_v8.f32)',
                 'rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*)v27.u8)', 'REX_VMADDFP',
                 'l_r5.s64 = r29.s64 + -32;', 'rex::ppc::divwu(l_r7.u32, l_r5.u32)', 'l_r6.s64 = l_r6.s64 + -1;'):
        if need not in comb:
            sys.exit('gen_reverb_native: the comb loop has no %r' % need)
    for need in ('REX_LOAD_D_U32(r31.u32, 48)', 'REX_LOAD_D_U32(r31.u32, 44)', 'simde_mm_load_ps(v31.f32)',
                 'simde_mm_sub_ps(simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(l_v0.f32))',
                 'REX_LOAD_U32(l_r10.u32 + r29.u32)', 'simde_mm_load_ps(v30.f32)', 'simde_mm_load_ps(v29.f32)',
                 'l_r7.s64 = l_r7.s64 + -1;'):
        if need not in allp:
            sys.exit('gen_reverb_native: the allpass loop has no %r' % need)
    out = (lines[:c0] + COMB_FAST.strip('\n').split('\n') + lines[c0:c1 + 1] + ['nb_comb_done:;'] +
           lines[c1 + 1:p0] + ALLPASS_FAST.strip('\n').split('\n') + lines[p0:p1 + 1] + ['nb_allpass_done:;'] +
           lines[p1 + 1:])
    native = '\n'.join(out)
    native = re.sub(r'\breturn;', 'return true;', native)
    if 'f_sub_82168520(ctx, base, s)' not in native:
        sys.exit('gen_reverb_native: the call of sub_82168520 is not where the script expects it')
    native = native.replace('f_sub_82168520(ctx, base, s)', 'C::sub_82168520(ctx, base, s)')
    if re.search(r'\bf_sub_', native):
        sys.exit('gen_reverb_native: another callee copy in the body: %s' % re.search(r'\bf_sub_\w+', native).group(0))
    src = os.path.relpath(a.copy, ROOT).replace(os.sep, '/')
    head = ('// GENERATED by tools/ultimate/gen_reverb_native.py from %s (Theft4-NX U18 MAX, 2026-11-17): the hand-written\n'
            '// native of GTA IV\'s reverb sub_828C6740 (hand.txt; key gta4_hand_reverb). The copy\'s statements with its\n'
            '// two innermost loop nests in registers; see the script\'s docstring. Included by banjo_hand_natives.h inside\n'
            '// namespace banjo::hand. Do not edit: run the script again on a new copy.\n' % src)
    text_out = (head + HELPERS + '\nnamespace n_rage_reverb {\n\ntemplate <class C, class S>\n'
                'inline bool Native(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s) {\n'
                '\tif (!On(kRageReverb)) [[unlikely]] return false;\n' + native + '}\n\n}  // namespace n_rage_reverb\n')
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text_out)
    print('gen_reverb_native: %s (%d lines, comb loop %d-%d, allpass loop %d-%d of the copy body)' %
          (os.path.relpath(a.out, ROOT), text_out.count('\n'), c0, c1, p0, p1))


if __name__ == '__main__':
    main()
