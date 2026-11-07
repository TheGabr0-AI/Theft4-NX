// gta4_native deko3d back end: the vertex fix-ups (docs/switch-port/16-deko3d-devlog.md, run 41, section L).
//
// A draw's vertex range is copied from guest memory with every 32-bit word byte-swapped (CopyGuestWords); some element
// types then need their bytes put right: the 16-bit component pairs (each word holds them in the wrong order after the
// swap), the DEC3N normals (the implicit w = 1 in the two high bits) and the D3DCOLORs read as integers (R and B
// swapped). Run 40 moved this work from the upload ring (CPU-uncached, a read is a bus round trip) to a cached scratch
// buffer that is copied into the ring in one pass; the function is here so that tests/deko3d/test_resolve_clear.py
// can compare it with the in-place version of run 39 (commit 17f3a18's Draw()) byte for byte.
//
// No Switch headers here.

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace rex::graphics::gta4_native::dk {

// Bytes of one vertex element of a D3DDECLTYPE as the title packs it (the types VertexAttribute accepts).
inline uint32_t VertexElementBytes(uint32_t type) {
  switch (type) {
    case 0x2C23A5: return 8;
    case 0x2A23B9: return 12;
    case 0x1A23A6: return 16;
    case 0x1A235A:
    case 0x1A215A:
    case 0x1A205A:
    case 0x1A2360: return 8;
    default: return 4;
  }
}

// `fixes`: (offset in the vertex, fix kind in the low 8 bits and the element type above): 1 the 16-bit components'
// halves, 2 DEC3N's w, 3 R and B of a D3DCOLOR. `data`: `vertex_count` vertices of `stride` bytes, already swapped.
inline void ApplyVertexFixes(uint8_t* data, uint64_t vertex_count, uint32_t stride,
                             std::span<const std::pair<uint32_t, uint32_t>> fixes) {
  for (const auto& [offset, fix] : fixes) {
    const uint32_t kind = fix & 0xFF;
    const uint32_t type = fix >> 8;
    for (uint64_t v = 0; v < vertex_count; ++v) {
      uint8_t* p = data + v * stride + offset;
      if (offset + VertexElementBytes(type) > stride) {
        break;
      }
      if (kind == 1) {
        // 16-bit components: after the 32-bit swap each word holds them in the wrong order.
        for (uint32_t w = 0; w < VertexElementBytes(type) / 4; ++w) {
          uint32_t x;
          std::memcpy(&x, p + w * 4, 4);
          x = std::rotl(x, 16);
          std::memcpy(p + w * 4, &x, 4);
        }
      } else if (kind == 2) {
        uint32_t x;
        std::memcpy(&x, p, 4);
        x = (x & 0x3FFFFFFF) | 0x40000000;
        std::memcpy(p, &x, 4);
      } else if (kind == 3) {
        std::swap(p[0], p[2]);
      }
    }
  }
}

}  // namespace rex::graphics::gta4_native::dk
