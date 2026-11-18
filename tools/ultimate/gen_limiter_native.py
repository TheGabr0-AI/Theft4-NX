"""gen_limiter_native.py: writes ultimate/app/src/cpu/hot/gta4_limiter_native.inc, the hand-written native of GTA IV's
look-ahead limiter sub_828C5A58 (Theft4-NX U18 MAX, 2026-11-17; docs/switch-port/20 section 2.27).

    python tools/ultimate/gen_limiter_native.py [--copy ultimate/app/generated/hot/n_828C5A58.h] [--out <inc>]

What sub_828C5A58 is (its PowerPC): a limiter of the audio mixer thread. With 652(r31) set, for each channel of the
voice (mask at 658(r31)) and each of its 256 samples n it takes the peak |x| over a 21-sample window (the 20 samples
before n, the first ones from the channel's 20-sample history at 12 + 80 * channel of the object, then sample n), with
a loop of fabs / fsubs / fsel per window sample, then an envelope (attack or release by fsel), a gain with a divide,
and writes gain * level * the sample delayed by 20 to a scratch buffer (0x8212ED00) that is copied over the channel at
the end (and its last 20 samples into the history). Console profile U18-1 B0: 12.1 % of the mixer thread, ~80 % of it
in the window loop (objdump: each fsel is a single-to-double conversion, an integer test of the double's bits for its
sign and NaN, and two selects: about 300 instructions a sample).

What the native does: the copy's own statements (read from the processed copy by this script), with, per channel,
the 276 window values (history then channel, sign bits cleared) gathered once, and per sample the window's maximum
taken as the largest bit pattern of those words. For values that are not NaN, fsel(m - |x| >= 0, m, |x|) in the
original's order IS the maximum (fsubs of two non-negative singles is negative exactly when the second is larger,
+0 when equal, NaN only for inf - inf where both are inf), and for non-negative singles the order of the values is the
order of their bit patterns; f0 = double(that single), as lfs + fabs leave it. The original's statements still run:
for a channel whose window values hold a NaN, whose scratch output range overlaps the history or the channel, for the
last sample of every channel (so every register leaves as the original leaves it), and with the key off.

The guard cannot compare this function (two real calls of the timebase before its loop stop the logged run), so the
native checks itself: gta4_hand_limiter_verify recomputes every fast sample's peak with the original statements'
arithmetic (REX_FSUBS and rex::ppc::GeZeroAnyMode in the original's order); a difference logs, keeps the reference
value and switches the fast path off for the rest of the run.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COPY = os.path.join(ROOT, 'ultimate', 'app', 'generated', 'hot', 'n_828C5A58.h')
OUT = os.path.join(ROOT, 'ultimate', 'app', 'src', 'cpu', 'hot', 'gta4_limiter_native.inc')

PREP = r'''
	// ---- Theft4-NX native (tools/ultimate/gen_limiter_native.py): this channel's window values, once ----
	nb_peak_ok = false;
	if (::banjo::hand::limiter::FastOn()) {
		const uint32_t nb_h = (uint32_t)((r29.u32 + 3u) << 2) + r31.u32;
		const uint32_t nb_b = r30.u32;
		const ::banjo::hand::reverb::Range nb_rh = ::banjo::hand::reverb::R(nb_h, 80u);
		const ::banjo::hand::reverb::Range nb_rb = ::banjo::hand::reverb::R(nb_b, 1024u);
		const ::banjo::hand::reverb::Range nb_ro = ::banjo::hand::reverb::R(r26.u32, 1024u);
		if (nb_rh.hi <= 0x100000000ull && nb_rb.hi <= 0x100000000ull && nb_ro.hi <= 0x100000000ull &&
		    !nb_ro.Overlaps(nb_rh) && !nb_ro.Overlaps(nb_rb) && uint32_t(l_r4.u32 + l_r6.u32) == r26.u32)
			nb_peak_ok = ::banjo::hand::limiter::Gather(base, nb_h, nb_b, nb_peak);
	}
'''

FAST = r'''
	// ---- Theft4-NX native: the window's peak as the largest of its 21 words (not the channel's last sample) ----
	if (nb_peak_ok && ll_r10.u32 < 255u) {
		const uint32_t nb_n = ll_r10.u32;
		uint32_t nb_bits = ::banjo::hand::limiter::WindowMax(nb_peak + nb_n);
		if (::banjo::hand::limiter::Verify()) [[unlikely]] {
			const uint64_t nb_ref = ::banjo::hand::limiter::Reference(nb_peak, nb_n);
			const double nb_fast = rex::ppc::SingleToDoublePure(nb_bits);
			if (std::bit_cast<uint64_t>(nb_fast) != nb_ref) [[unlikely]] {
				::banjo::hand::limiter::Mismatch(nb_n, std::bit_cast<uint64_t>(nb_fast), nb_ref);
				ll_f0.u64 = nb_ref;
				goto loc_828C5CC0;
			}
		}
		ll_f0.f64 = rex::ppc::SingleToDoublePure(nb_bits);
		goto loc_828C5CC0;
	}
'''

HELPERS = r'''
namespace limiter {
bool VerifyKey();                                          // banjo_hot_guest.cpp: gta4_hand_limiter_verify
void Mismatch(uint32_t n, uint64_t fast, uint64_t ref);    // logs, switches the fast path off
extern bool g_off;                                         // set by Mismatch
[[gnu::always_inline]] inline bool FastOn() { return On(kRageLimiter) && !g_off; }
[[gnu::always_inline]] inline bool Verify() {
  static const bool on = VerifyKey();
  return on;
}
// The window values of one channel: the 20 history words, then the 256 channel words, each byte-swapped and its sign
// bit cleared (what fabs leaves of it). False (the original statements run) when one is a NaN.
[[gnu::always_inline]] inline bool Gather(uint8_t* base, uint32_t h, uint32_t b, uint32_t* out) {
  uint32_t nan = 0;
  for (uint32_t i = 0; i < 20; ++i) {
    uint32_t w;
    std::memcpy(&w, base + h + 4u * i, 4);
    w = __builtin_bswap32(w) & 0x7FFFFFFFu;
    nan |= w > 0x7F800000u;
    out[i] = w;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t w;
    std::memcpy(&w, base + b + 4u * i, 4);
    w = __builtin_bswap32(w) & 0x7FFFFFFFu;
    nan |= w > 0x7F800000u;
    out[20 + i] = w;
  }
  return nan == 0;
}
// The largest of p[0..20] (sample n's window: history n..19 and channel n-20..n, or channel n-20..n).
[[gnu::always_inline]] inline uint32_t WindowMax(const uint32_t* p) {
#if defined(__aarch64__)
  uint32x4_t m = vmaxq_u32(vmaxq_u32(vld1q_u32(p), vld1q_u32(p + 4)), vmaxq_u32(vld1q_u32(p + 8), vld1q_u32(p + 12)));
  m = vmaxq_u32(m, vld1q_u32(p + 16));
  const uint32_t a = vmaxvq_u32(m);
  return a > p[20] ? a : p[20];
#else
  uint32_t a = p[0];
  for (uint32_t i = 1; i < 21; ++i)
    a = p[i] > a ? p[i] : a;
  return a;
#endif
}
// The original statements' peak (f0 as double bits): f0 = |x[n]|, then for history n..19 and the channel's samples
// before n, f13 = |x|, f6 = fsubs(f0, f13), f0 = fsel(f6, f0, f13), in that order.
inline uint64_t Reference(const uint32_t* p, uint32_t n) {
  PPCRegister f0, f13, f6;
  f0.f64 = rex::ppc::SingleToDoublePure(p[20 + n]);
  auto step = [&](uint32_t w) {
    f13.f64 = rex::ppc::SingleToDoublePure(w);
    f6.f64 = REX_FSUBS(f0.f64, f13.f64);
    f0.f64 = rex::ppc::GeZeroAnyMode(f6.f64) ? f0.f64 : f13.f64;
  };
  for (uint32_t i = n; i < 20; ++i)
    step(p[i]);
  for (uint32_t j = n >= 20 ? n - 20 : 0; j < n; ++j)
    step(p[20 + j]);
  return f0.u64;
}
}  // namespace limiter
'''


def body_of(text, name):
    m = re.search(r'template <class S>\n(?:\[\[[^\]]*\]\] )?static void f_%s\(PPCContext& __restrict ctx, uint8_t\* base, '
                  r'\[\[maybe_unused\]\] S& s\) \{\n' % name, text)
    if not m:
        sys.exit('gen_limiter_native: no template f_%s in the copy' % name)
    end = text.index('\n}\n', m.end())
    return text[m.end():end + 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--copy', default=COPY)
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    text = open(a.copy, encoding='utf-8').read()
    body = body_of(text, 'sub_828C5A58')
    lines = body.split('\n')

    def find(pattern, start=0):
        for i in range(start, len(lines)):
            if re.search(pattern, lines[i]):
                return i
        sys.exit('gen_limiter_native: %r not found' % pattern)

    ch = find(r'^loc_828C5AE0:')
    l0 = find(r'^loc_828C5B10:', ch)
    s0 = find(r'^loc_828C5B30:', l0)
    sel = find(r'^\s*ll_f12\.f64 = rex::ppc::GeZeroAnyMode\(ll_f13\.f64\) \? f28\.f64 : f27\.f64;$', s0)
    cc = find(r'^loc_828C5CC0:', sel)
    # The statements the inserts rely on.
    chunk = '\n'.join(lines[ch:l0])
    for need in ('r30.u64 = r27.u64 + l_r11.u64;', 'l_r6.u64 = r30.u64;', 'l_r4.u64 = r26.u64 - r30.u64;'):
        if need not in chunk:
            sys.exit('gen_limiter_native: the channel start has no %r' % need)
    chunk = '\n'.join(lines[s0:sel + 1])
    for need in ('ll_f0.f64 = REX_LOAD_D_F64ANY(ll_r6.u32, 0);', 'REX_FABS(ll_f0, ll_f0);', 'ctx.fpscr.disableFlushMode();'):
        if need not in chunk:
            sys.exit('gen_limiter_native: the sample start has no %r' % need)
    window = '\n'.join(lines[sel + 1:cc])
    if 'BANJO_HOT_STORE' in window or 'STORE' in window:
        sys.exit('gen_limiter_native: the window loops store something: the native does not cover that')
    out = (['\tbool nb_peak_ok = false;', '\tuint32_t nb_peak[276];'] + lines[:l0] + PREP.strip('\n').split('\n') +
           lines[l0:sel + 1] + FAST.strip('\n').split('\n') + lines[sel + 1:])
    native = '\n'.join(out)
    native = re.sub(r'\breturn;', 'return true;', native)
    calls = sorted(set(re.findall(r'\bf_(sub_[0-9A-F]{8})\(ctx, base, s\)', native)))
    for c in calls:
        native = native.replace('f_%s(ctx, base, s)' % c, 'C::%s(ctx, base, s)' % c)
    if re.search(r'\bf_sub_', native):
        sys.exit('gen_limiter_native: a callee copy left: %s' % re.search(r'\bf_sub_\w+', native).group(0))
    reals = sorted(set(re.findall(r'\b(__imp__sub_[0-9A-F]{8})\(ctx, base\)', native)))
    src = os.path.relpath(a.copy, ROOT).replace(os.sep, '/')
    head = ('// GENERATED by tools/ultimate/gen_limiter_native.py from %s (Theft4-NX U18 MAX, 2026-11-17): the\n'
            '// hand-written native of GTA IV\'s look-ahead limiter sub_828C5A58 (hand.txt; keys gta4_hand_limiter,\n'
            '// gta4_hand_limiter_verify). The copy\'s statements with the window peak from the gathered words; see the\n'
            '// script\'s docstring. Included by banjo_hand_natives.h inside namespace banjo::hand, after the reverb\'s\n'
            '// (its address ranges). Do not edit: run the script again on a new copy.\n' % src)
    ext = ''.join('REX_EXTERN(%s);\n' % r for r in reals)
    text_out = (head + HELPERS + '\nnamespace n_rage_limiter {\n\n' + ext + '\ntemplate <class C, class S>\n'
                'inline bool Native(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s) {\n'
                '\tif (!On(kRageLimiter)) [[unlikely]] return false;\n' + native + '}\n\n}  // namespace n_rage_limiter\n')
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text_out)
    print('gen_limiter_native: %s (%d lines); hand.txt calls=%s; real calls kept: %s' %
          (os.path.relpath(a.out, ROOT), text_out.count('\n'), ','.join(c[4:] for c in calls), ', '.join(reals)))


if __name__ == '__main__':
    main()
