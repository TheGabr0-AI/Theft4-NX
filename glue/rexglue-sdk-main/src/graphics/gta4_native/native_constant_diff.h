// gta4_native: the difference of a guest constant bank against the shadow of the previous draw (docs/switch-port/
// 16-deko3d-devlog.md, run 45). Header only, no Switch headers: tests/deko3d/test_constant_diff.py compiles it on the PC.
//
// The banks are 16-byte registers of big-endian floats in guest memory (256 vertex, 224 pixel). A draw changes a handful of
// them; run 44 measured the front end at 150-200 us per draw, of which the old register-by-register loop (four byte swaps and a
// 16-byte compare for each of 480 registers) was one of the few identifiable parts. This version keeps the shadow as the raw
// big-endian words, compares 64 bytes (four registers) at a time, and byte-swaps only the registers that changed. What it
// hands over is the same: the changed registers as runs of host-endian floats.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "native_recorder.h"

namespace rex::graphics::gta4_native {

// The old loop, kept as the reference the test (and nothing else) runs: `shadow` holds host-endian floats.
inline uint32_t DiffRegistersReference(const uint8_t* guest, float* shadow, uint32_t count, bool whole,
                                       ConstantDelta& delta) {
  const auto load = [](const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
  };
  delta.whole = whole;
  uint32_t changed = 0;
  ConstantRun* run = nullptr;
  for (uint32_t r = 0; r < count; ++r) {
    uint32_t words[4];
    for (uint32_t c = 0; c < 4; ++c) {
      words[c] = load(guest + r * 16 + c * 4);
    }
    if (!whole && std::memcmp(words, shadow + r * 4, 16) == 0) {
      run = nullptr;
      continue;
    }
    std::memcpy(shadow + r * 4, words, 16);
    if (!run) {
      delta.runs.push_back({uint16_t(r), 0, uint32_t(delta.values.size())});
      run = &delta.runs.back();
    }
    ++run->count;
    const float* f = shadow + r * 4;
    delta.values.insert(delta.values.end(), f, f + 4);
    ++changed;
  }
  return changed;
}

// The registers of [0, count) that differ between `guest` and `shadow` (raw big-endian words of the previous draw), appended
// as runs of host-endian floats; the shadow takes the new bytes. `whole`: every register travels. `count` is a multiple of
// four registers in both banks (256 and 224). Returns the number of registers that travelled.
inline uint32_t DiffRegisters(const uint8_t* guest, uint32_t* shadow, uint32_t count, bool whole, ConstantDelta& delta) {
  delta.whole = whole;
  uint32_t changed = 0;
  ConstantRun* run = nullptr;
  uint8_t* shadow_bytes = reinterpret_cast<uint8_t*>(shadow);
  const uint32_t groups = count / 4;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint8_t* source = guest + g * 64;
    uint8_t* kept = shadow_bytes + g * 64;
    if (!whole && std::memcmp(source, kept, 64) == 0) {
      run = nullptr;  // four registers untouched: a run ends here
      continue;
    }
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t r = g * 4 + k;
      if (!whole && std::memcmp(source + k * 16, kept + k * 16, 16) == 0) {
        run = nullptr;
        continue;
      }
      std::memcpy(kept + k * 16, source + k * 16, 16);
      if (!run) {
        // Run 46: no reserve here; the first 16 registers (8 runs) stay inside the delta (native_inline_vector.h).
        delta.runs.push_back({uint16_t(r), 0, uint32_t(delta.values.size())});
        run = &delta.runs.back();
      }
      ++run->count;
      uint32_t words[4];
      std::memcpy(words, source + k * 16, 16);
      for (uint32_t c = 0; c < 4; ++c) {
        words[c] = __builtin_bswap32(words[c]);
        float f;
        std::memcpy(&f, &words[c], 4);
        delta.values.push_back(f);
      }
      ++changed;
    }
  }
  return changed;
}

// The recorder's side: a draw's delta into its bank (host-endian floats); `dirty` marks the registers written. A run
// past the bank is ignored. Returns whether anything was written.
inline bool ApplyConstantDelta(const ConstantDelta& delta, float* bank, bool* dirty, uint32_t registers) {
  bool any = false;
  for (const ConstantRun& run : delta.runs) {
    if (run.first + run.count > registers) {
      continue;
    }
    std::memcpy(bank + run.first * 4, delta.values.data() + run.data, size_t(run.count) * 16);
    for (uint32_t r = run.first; r < uint32_t(run.first + run.count); ++r) {
      dirty[r] = true;
    }
    any = true;
  }
  return any;
}

// Run 50: a hash of registers [first, first + count) of a bank, the same from the front end's shadow (raw big-endian
// words, `big_endian`) and from the recorder's bank (host-endian floats as words). A sampled draw carries the front end's
// hashes; the recorder compares them with its bank when it records the draw, so a delta lost or applied in the wrong
// order between the two shows as a difference (the bone palette is c64-c255).
inline uint64_t ConstantBankHash(const void* words, uint32_t first, uint32_t count, bool big_endian) {
  uint64_t h = 0xCBF29CE484222325ull ^ (uint64_t(first) << 32) ^ count;
  const uint8_t* p = static_cast<const uint8_t*>(words) + size_t(first) * 16;
  for (uint32_t i = 0; i < count * 4; ++i) {
    uint32_t w;
    std::memcpy(&w, p + i * 4, 4);
    w = big_endian ? __builtin_bswap32(w) : w;
    h = (h ^ w) * 0x100000001B3ull;
    h ^= h >> 31;
  }
  return h;
}

}  // namespace rex::graphics::gta4_native