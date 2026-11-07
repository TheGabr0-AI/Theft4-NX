// gta4_native: the index rules of the Vulkan path (docs/switch-port/16-deko3d-devlog.md, run 49, section V).
//
// Up to run 48 the recorder swapped the title's indices and nothing else: 32-bit indices kept their top byte, the
// primitive restart was switched on for every indexed draw that had it in the device state (lists too), and a 16-bit
// draw whose reset index was above 0xFFFF had it cut to 0xFFFF. Gta4NativeGraphicsSystem (LibertyRecompAndroid,
// graphics_system.cpp, ConvertGuestIndices and the restart upload of the indexed draw) does what Xenos does instead:
// - an index is 24 bits (xenos::kVertexIndexMask): a 32-bit index loses its top byte;
// - the restart is a strip thing: only line strips, triangle strips and fans use it, and a 16-bit draw only when the
//   reset index fits in 16 bits;
// - the guest's reset value becomes the host's (all ones), so the host restart index is fixed.
// The scan for the vertex range and the conversion are one pass here (run 48 made two over the guest's indices).
//
// No Switch headers here: tests/deko3d/test_index_policy.py compiles it on the PC. The front end uses the same
// scan for the vertex range of a draw it copies or hashes at submit (native_draw_snapshot.h).

#pragma once

#include <cstdint>
#include <cstring>

namespace rex::graphics::gta4_native {

inline constexpr uint32_t kXenosIndexMask = 0x00FFFFFFu;

// Xenos primitive types (xenos::PrimitiveType) the restart applies to: line strip 3, triangle fan 5, triangle strip 6.
inline bool RestartAppliesTo(uint32_t primitive_type) {
  return primitive_type == 3 || primitive_type == 5 || primitive_type == 6;
}

struct IndexPolicy {
  bool restart = false;        // the host restart is on (index all ones)
  uint32_t guest_restart = 0;  // the guest's reset value, 24 bits
};

inline IndexPolicy IndexPolicyFor(uint32_t primitive_type, bool restart_enabled, uint32_t restart_index, bool index32) {
  IndexPolicy p;
  p.guest_restart = restart_index & kXenosIndexMask;
  p.restart = restart_enabled && RestartAppliesTo(primitive_type) && (index32 || p.guest_restart <= 0xFFFFu);
  return p;
}

struct IndexConversion {
  uint32_t lo = UINT32_MAX;  // the smallest and largest index that is not a restart (lo > hi: none)
  uint32_t hi = 0;
  uint32_t masked = 0;    // 32-bit indices whose top byte was not zero
  uint32_t restarts = 0;  // indices turned into the host restart value
};

// `count` big-endian guest indices at `source` into host order at `destination`, by the policy.
inline IndexConversion ConvertIndices(const uint8_t* source, uint32_t count, bool index32, const IndexPolicy& p,
                                      uint8_t* destination) {
  IndexConversion r;
  if (!p.restart && count) {
    // Run 50: without restart (run 49: every draw of the title) the loops have no branch and the compiler vectorizes
    // them; run 49's one loop with the restart test cost 6.4-8.2 us per draw on the recorder (the scan alone was 1.9).
    if (index32) {
      uint32_t lo = UINT32_MAX, hi = 0, masked = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t raw;
        std::memcpy(&raw, source + size_t(i) * 4, 4);
        raw = __builtin_bswap32(raw);
        const uint32_t v = raw & kXenosIndexMask;
        masked += v != raw;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        std::memcpy(destination + size_t(i) * 4, &v, 4);
      }
      r.lo = lo, r.hi = hi, r.masked = masked;
    } else {
      uint16_t lo = UINT16_MAX, hi = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t v;
        std::memcpy(&v, source + size_t(i) * 2, 2);
        v = __builtin_bswap16(v);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
        std::memcpy(destination + size_t(i) * 2, &v, 2);
      }
      r.lo = lo, r.hi = hi;
    }
    return r;
  }
  if (index32) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t raw;
      std::memcpy(&raw, source + size_t(i) * 4, 4);
      raw = __builtin_bswap32(raw);
      uint32_t v = raw & kXenosIndexMask;
      r.masked += v != raw;
      if (p.restart && v == p.guest_restart) {
        v = UINT32_MAX;
        ++r.restarts;
      } else {
        r.lo = v < r.lo ? v : r.lo;
        r.hi = v > r.hi ? v : r.hi;
      }
      std::memcpy(destination + size_t(i) * 4, &v, 4);
    }
  } else {
    const uint16_t guest_restart = uint16_t(p.guest_restart);
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, source + size_t(i) * 2, 2);
      v = __builtin_bswap16(v);
      if (p.restart && v == guest_restart) {
        v = UINT16_MAX;
        ++r.restarts;
      } else {
        r.lo = v < r.lo ? v : r.lo;
        r.hi = v > r.hi ? v : r.hi;
      }
      std::memcpy(destination + size_t(i) * 2, &v, 2);
    }
  }
  return r;
}

// The same lo/hi as ConvertIndices, without writing anything (the front end's range of a draw at submit).
inline IndexConversion ScanIndices(const uint8_t* source, uint32_t count, bool index32, const IndexPolicy& p) {
  IndexConversion r;
  if (!p.restart && count) {
    // Run 50: branch-free, as ConvertIndices (the front end runs this on the title's thread for every copied draw).
    if (index32) {
      uint32_t lo = UINT32_MAX, hi = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t raw;
        std::memcpy(&raw, source + size_t(i) * 4, 4);
        const uint32_t v = __builtin_bswap32(raw) & kXenosIndexMask;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
      }
      r.lo = lo, r.hi = hi;
    } else {
      uint16_t lo = UINT16_MAX, hi = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t raw;
        std::memcpy(&raw, source + size_t(i) * 2, 2);
        const uint16_t v = __builtin_bswap16(raw);
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
      }
      r.lo = lo, r.hi = hi;
    }
    return r;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v;
    if (index32) {
      uint32_t raw;
      std::memcpy(&raw, source + size_t(i) * 4, 4);
      v = __builtin_bswap32(raw) & kXenosIndexMask;
    } else {
      uint16_t raw;
      std::memcpy(&raw, source + size_t(i) * 2, 2);
      v = __builtin_bswap16(raw);
      if (p.restart && v == uint16_t(p.guest_restart)) {
        continue;
      }
    }
    if (index32 && p.restart && v == p.guest_restart) {
      continue;
    }
    r.lo = v < r.lo ? v : r.lo;
    r.hi = v > r.hi ? v : r.hi;
  }
  return r;
}

}  // namespace rex::graphics::gta4_native
