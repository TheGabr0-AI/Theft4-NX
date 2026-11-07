// Probe of run 45 (devlog section P): native_constant_diff.h. The old register loop and the 64-byte-group loop run on the same
// sequences of guest banks and must hand over the same runs and values. Host clang, no Switch headers.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "native_constant_diff.h"

using namespace rex::graphics::gta4_native;

static bool Same(const ConstantDelta& a, const ConstantDelta& b) {
  if (a.whole != b.whole || a.runs.size() != b.runs.size() || a.values.size() != b.values.size()) return false;
  for (size_t i = 0; i < a.runs.size(); ++i) {
    if (a.runs[i].first != b.runs[i].first || a.runs[i].count != b.runs[i].count || a.runs[i].data != b.runs[i].data)
      return false;
  }
  return std::memcmp(a.values.data(), b.values.data(), a.values.size() * 4) == 0;
}

int main() {
  std::mt19937 rng(0x45);
  uint32_t draws = 0, bad = 0, changed_total = 0, runs_total = 0, shadow_bad = 0;
  for (const uint32_t count : {256u, 224u}) {
    std::vector<uint8_t> guest(count * 16);
    for (auto& b : guest) b = uint8_t(rng());
    std::vector<float> old_shadow(count * 4);
    std::vector<uint32_t> new_shadow(count * 4);
    bool whole = true;
    for (uint32_t draw = 0; draw < 4000; ++draw) {
      // A draw changes a few words, sometimes a register, sometimes a whole block, sometimes nothing; now and then the bank is
      // new (the first draw of a device: whole).
      const uint32_t mode = draw % 7;
      if (mode == 1) {
        guest[rng() % guest.size()] ^= 1;
      } else if (mode == 2) {
        const uint32_t r = rng() % count;
        for (uint32_t i = 0; i < 16; ++i) guest[r * 16 + i] = uint8_t(rng());
      } else if (mode == 3) {
        const uint32_t g = rng() % (count / 4);
        for (uint32_t i = 0; i < 64; ++i) guest[g * 64 + i] = uint8_t(rng());
      } else if (mode == 4) {
        for (uint32_t k = 0; k < 20; ++k) guest[rng() % guest.size()] = uint8_t(rng());
      } else if (mode == 5) {
        for (auto& b : guest) b = uint8_t(rng());
      } else if (mode == 6 && draw % 1000 == 6) {
        whole = true;
      }
      ConstantDelta a, b;
      const uint32_t na = DiffRegistersReference(guest.data(), old_shadow.data(), count, whole, a);
      const uint32_t nb = DiffRegisters(guest.data(), new_shadow.data(), count, whole, b);
      ++draws;
      if (na != nb || !Same(a, b)) ++bad;
      // The new shadow is the guest's bytes; the old one is the same words host-endian.
      if (std::memcmp(new_shadow.data(), guest.data(), guest.size()) != 0) ++shadow_bad;
      changed_total += nb;
      runs_total += uint32_t(b.runs.size());
      whole = false;
    }
  }
  std::printf("CONSTDIFF draws %u differences %u shadow_wrong %u registers %u runs %u\n", draws, bad, shadow_bad,
              changed_total, runs_total);
  return 0;
}