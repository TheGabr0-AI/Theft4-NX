// switch-game: the guest audio mixer's voice loop as native NEON (docs/switch-port/16-deko3d-devlog.md, run 51).
//
// sub_8216DBB0 (EU base; 0x82199BC8 on the USA title update 8) adds a voice into a mix buffer:
// dst[256] += src[256] * gain, with four gain vectors (start, start + step, start + 2 step, start + 3 step) advanced by
// 4 step after each 64-byte group, 16 groups. Run 50 measured it as the largest single piece of the mixer thread
// (7.4 %), almost all of it guest 128-bit loads and stores through the stored PPC vector registers.
//
// Adapted from kabba536's GTA IV Switch port (gta4_native_hooks.cpp, sub_82199BC8, used with permission), checked
// against our generated EU body: same loads (all of a group's before its stores), same store order (d+48, d+0, d+16,
// d+32), the gains summed in the same order. One difference from theirs: multiply and add are kept separate (no
// vfmaq), as in the generated code (simde_mm_add_ps(simde_mm_mul_ps(...))), so the result is the generated code's.
// Written against simde's NEON names: native NEON on the console, emulated for tests/deko3d/test_run51.py on the PC.

#pragma once

#include <cstdint>

#include <simde/arm/neon.h>

namespace theft4::nx_audio_mixer {

// `host(ea)`: the host address of guest address ea. On the Switch that is rex::memory::GuestToHost (one table entry
// per 16 MiB slice; guest memory is not one block at `base`): run 51 used base + ea and faulted at 21 s on a voice
// in the physical heap (guest 0xDAA00730). Each 16-byte vector is translated on its own, so a buffer that crosses a
// slice is right too. The PC test passes base + ea.
template <class Host>
inline simde_float32x4_t LoadGuest(Host&& host, uint32_t ea) {
  return simde_vreinterpretq_f32_u8(simde_vrev32q_u8(simde_vld1q_u8(host(ea & ~0xFu))));
}

template <class Host>
inline void StoreGuest(Host&& host, uint32_t ea, simde_float32x4_t v) {
  simde_vst1q_u8(host(ea & ~0xFu), simde_vrev32q_u8(simde_vreinterpretq_u8_f32(v)));
}

// The voice loop with the start gain vector (v13) and the step vector (v0) in registers (run 64: nx_audio_routes.h
// builds them itself instead of storing them on the guest stack for this function to load back).
template <class Host>
inline void MixVoiceVectors(Host&& base, uint32_t dst, uint32_t src, simde_float32x4_t start, simde_float32x4_t step) {
  const simde_float32x4_t step2 = simde_vaddq_f32(step, step);       // v11 = v0 + v0
  simde_float32x4_t g1 = simde_vaddq_f32(start, step);               // v12 = v13 + v0
  const simde_float32x4_t step3 = simde_vaddq_f32(step2, step);      // v10 = v11 + v0
  const simde_float32x4_t step4 = simde_vaddq_f32(step2, step2);     // v0 = v11 + v11
  simde_float32x4_t g2 = simde_vaddq_f32(start, step2);              // v11 = v13 + v11
  simde_float32x4_t g3 = simde_vaddq_f32(start, step3);              // v10 = v13 + v10
  simde_float32x4_t g0 = start;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t d = dst + i * 64;
    const uint32_t s = src + i * 64;
    const simde_float32x4_t d3 = LoadGuest(base, d + 48), s3 = LoadGuest(base, s + 48);
    const simde_float32x4_t s0 = LoadGuest(base, s + 0), d0 = LoadGuest(base, d + 0);
    const simde_float32x4_t s2 = LoadGuest(base, s + 32), s1 = LoadGuest(base, s + 16);
    const simde_float32x4_t d1 = LoadGuest(base, d + 16), d2 = LoadGuest(base, d + 32);
    const simde_float32x4_t r3 = simde_vaddq_f32(simde_vmulq_f32(s3, g3), d3);
    const simde_float32x4_t r0 = simde_vaddq_f32(simde_vmulq_f32(s0, g0), d0);
    const simde_float32x4_t r1 = simde_vaddq_f32(simde_vmulq_f32(s1, g1), d1);
    const simde_float32x4_t r2 = simde_vaddq_f32(simde_vmulq_f32(s2, g2), d2);
    g0 = simde_vaddq_f32(g0, step4);
    g1 = simde_vaddq_f32(g1, step4);
    g2 = simde_vaddq_f32(g2, step4);
    g3 = simde_vaddq_f32(g3, step4);
    StoreGuest(base, d + 48, r3);
    StoreGuest(base, d + 0, r0);
    StoreGuest(base, d + 16, r1);
    StoreGuest(base, d + 32, r2);
  }
}

// r3 dst, r4 src, r5 the address of the start gain vector, r6 the address of the step vector (guest addresses).
template <class Host>
inline void MixVoice(Host&& base, uint32_t dst, uint32_t src, uint32_t gain_ea, uint32_t step_ea) {
  const simde_float32x4_t step = LoadGuest(base, step_ea);    // v0
  const simde_float32x4_t start = LoadGuest(base, gain_ea);   // v13
  MixVoiceVectors(base, dst, src, start, step);
}

}  // namespace theft4::nx_audio_mixer
