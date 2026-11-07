// Offline probe of run 51 (docs/switch-port/16-deko3d-devlog.md section Y), compiled by tests/deko3d/test_run51.py.
//
// MIXER: tools/switch-game/nx_audio_mixer.h (the NEON voice loop, through simde's NEON emulation here) against a
// line-by-line transliteration of the generated EU body of sub_8216DBB0 (gta4_recomp.2.cpp: lvx128 with VectorMaskL,
// simde_mm_add_ps(simde_mm_mul_ps(...)), the stores in the generated order), on random voices, gains and steps,
// including a source that overlaps the destination. Every byte of guest memory must match.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <simde/x86/ssse3.h>

#include "nx_audio_mixer.h"

namespace {

alignas(16) const uint8_t kVectorMaskL[16] = {0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
                                              0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};

struct alignas(16) Vec {
  union {
    float f32[4];
    uint8_t u8[16];
  };
};

// The generated body, register for register (r31 is the loop count it keeps in a local).
void GeneratedMix(uint8_t* base, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  Vec v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13;
  auto lvx = [&](Vec& v, uint32_t ea) {
    ea &= ~0xFu;
    simde_mm_store_si128((simde__m128i*)v.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + ea)),
                                                                     simde_mm_load_si128((simde__m128i*)kVectorMaskL)));
  };
  auto stvx = [&](const Vec& v, uint32_t ea) {
    ea &= ~0xFu;
    simde_mm_store_si128((simde__m128i*)(base + ea), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)v.u8),
                                                                            simde_mm_load_si128((simde__m128i*)kVectorMaskL)));
  };
  auto add = [](Vec& d, const Vec& a, const Vec& b) {
    simde_mm_store_ps(d.f32, simde_mm_add_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(b.f32)));
  };
  auto madd = [](Vec& d, const Vec& a, const Vec& c, const Vec& b) {  // vmaddfp d,a,c,b = a*c + b
    simde_mm_store_ps(d.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(a.f32), simde_mm_load_ps(c.f32)),
                                             simde_mm_load_ps(b.f32)));
  };
  lvx(v0, r6);
  uint32_t r31 = 16;
  add(v11, v0, v0);
  lvx(v13, r5);
  add(v12, v13, v0);
  r5 = r3 - r4;
  uint32_t r10 = r3 + 48;
  uint32_t r11 = r4 + 32;
  uint32_t r9 = r31;
  r3 = uint32_t(-32);
  r4 = uint32_t(-16);
  add(v10, v11, v0);
  add(v0, v11, v11);
  add(v11, v13, v11);
  add(v10, v13, v10);
  do {
    const uint32_t r8 = r10 - 48;
    v8 = v13;
    const uint32_t r7 = r10 - 32;
    lvx(v9, r10);
    const uint32_t r6b = r5 + r11;
    lvx(v4, r11 + r31);
    lvx(v3, r11 + r3);
    v7 = v12;
    v6 = v11;
    madd(v9, v4, v10, v9);
    lvx(v2, r8);
    r9 -= 1;
    madd(v8, v3, v8, v2);
    lvx(v5, r11);
    lvx(v4, r11 + r4);
    add(v13, v13, v0);
    lvx(v1, r7);
    r11 += 64;
    lvx(v3, r6b);
    madd(v7, v4, v7, v1);
    madd(v6, v5, v6, v3);
    add(v12, v12, v0);
    add(v11, v11, v0);
    add(v10, v10, v0);
    stvx(v9, r10);
    r10 += 64;
    stvx(v8, r8);
    stvx(v7, r7);
    stvx(v6, r6b);
  } while (r9 != 0);
}

void StoreBigEndianFloat(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  p[0] = uint8_t(u >> 24);
  p[1] = uint8_t(u >> 16);
  p[2] = uint8_t(u >> 8);
  p[3] = uint8_t(u);
}

}  // namespace

int main() {
  std::mt19937 rng(51);
  std::uniform_real_distribution<float> sample(-1.0f, 1.0f), gain(0.0f, 1.0f), step(-0.002f, 0.002f);
  constexpr uint32_t kMemory = 64 * 1024;
  std::vector<uint8_t> a(kMemory + 16), b(kMemory + 16);
  uint8_t* ma = reinterpret_cast<uint8_t*>((uintptr_t(a.data()) + 15) & ~uintptr_t(15));
  uint8_t* mb = reinterpret_cast<uint8_t*>((uintptr_t(b.data()) + 15) & ~uintptr_t(15));
  std::vector<uint8_t> c(kMemory + 16);
  uint8_t* scattered = reinterpret_cast<uint8_t*>((uintptr_t(c.data()) + 15) & ~uintptr_t(15));
  int rounds = 0, errors = 0, overlapping = 0;
  for (int round = 0; round < 4000; ++round) {
    for (uint32_t i = 0; i < kMemory; i += 4) {
      StoreBigEndianFloat(ma + i, sample(rng));
    }
    // dst and src: 1 KB blocks; every 8th round the source overlaps the destination by a few vectors.
    const uint32_t dst = 0x1000 + (rng() % 40) * 0x400;
    uint32_t src = 0x1000 + (rng() % 40) * 0x400;
    if (round % 8 == 7) {
      src = dst + 16 * (1 + rng() % 8);
      ++overlapping;
    }
    const uint32_t gain_ea = 0x100, step_ea = 0x200;
    for (uint32_t k = 0; k < 4; ++k) {
      StoreBigEndianFloat(ma + gain_ea + 4 * k, gain(rng));
      StoreBigEndianFloat(ma + step_ea + 4 * k, step(rng));
    }
    // Run 52: the NEON side sees guest memory as the Switch does, in slices that are not one block: here 1 KiB
    // slices placed in reverse order in `scattered`, reached through the translation callback only (run 51 read
    // base + ea and faulted on the console). After the mix the slices are gathered back into mb for the compare.
    constexpr uint32_t kSlice = 1024, kSlices = kMemory / kSlice;
    for (uint32_t s = 0; s < kSlices; ++s) {
      std::memcpy(scattered + (kSlices - 1 - s) * kSlice, ma + s * kSlice, kSlice);
    }
    GeneratedMix(ma, dst, src, gain_ea, step_ea);
    theft4::nx_audio_mixer::MixVoice(
        [scattered](uint32_t ea) { return scattered + (kSlices - 1 - ea / kSlice) * kSlice + ea % kSlice; }, dst,
        src, gain_ea, step_ea);
    for (uint32_t s = 0; s < kSlices; ++s) {
      std::memcpy(mb + s * kSlice, scattered + (kSlices - 1 - s) * kSlice, kSlice);
    }
    if (std::memcmp(ma, mb, kMemory) != 0) {
      if (++errors <= 4) {
        for (uint32_t i = 0; i < kMemory; ++i) {
          if (ma[i] != mb[i]) {
            std::printf("round %d: first difference at 0x%x (dst 0x%x src 0x%x)\n", round, i, dst, src);
            break;
          }
        }
      }
    }
    ++rounds;
  }
  std::printf("MIXER rounds %d overlapping %d errors %d\n", rounds, overlapping, errors);
  return errors ? 1 : 0;
}
