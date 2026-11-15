/**
 * blur-nx (2026-11-13, M-23 of docs/cpu-moonshots-50-1113.md): a guest single-precision value stored big-endian from
 * the vector unit. The generated code's single stores (stfs) go `fmov w, s; rev w; str w`: the move from the vector
 * unit to the general registers is the slow step on the Cortex-A57 (5 cycles). Swapped in the vector unit and stored
 * from there it is `rev32 v.8b; str s`: the same four bytes. Used by tools/float_loads.py's REX_STORE_*F32ANY forms
 * (with CG-13 on: REX_FPCR_ONE_MODE) and checked against the integer form by app/src/cpu/banjo_forms_selfcheck.cpp.
 * Other targets: the integer form.
 */
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace rex::ppc {

[[gnu::always_inline]] inline void FloatToBigEndian(void* p, float f) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  const float32x2_t v = vset_lane_f32(f, vdup_n_f32(0.0f), 0);
  const float s = vget_lane_f32(vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_f32(v))), 0);  // rev32
  std::memcpy(p, &s, 4);                                                                   // str s
#else
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(p, &u, 4);
#endif
}

// The load's twin (tools/float_loads.py's RexFloatFromBigEndian, in the generated pch): for the self-check.
[[gnu::always_inline]] inline float FloatFromBigEndian(const void* p) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  float f;
  std::memcpy(&f, p, 4);  // ldr s
  const float32x2_t v = vset_lane_f32(f, vdup_n_f32(0.0f), 0);
  return vget_lane_f32(vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_f32(v))), 0);  // rev32
#else
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
#endif
}

}  // namespace rex::ppc
