// deko3d recorder: one Xenos rectangle (three vertices) as a quad (run 52, 16-deko3d-devlog.md section Z).
//
// As the Vulkan path (graphics_system.cpp, kRectangleList): the corner is the vertex opposite the longest edge in the
// position's x/y, the fourth vertex is c1 - c0 + c2 for the float32 elements only (every other byte is c2's), and the
// quad is c0 c1 c3 c2. Vertices in host byte order. No deko3d or SDK headers: tests/deko3d/test_run52.py builds it.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace rex::graphics::gta4_native::dk {

// `floats`: (offset, component count) of the float32 elements; `position_offset` < 0: no usable position (v0 is
// taken as the corner). `out` gets 4 * stride bytes; `fourth` is stride bytes of scratch in cached memory (out may be
// CPU-uncached: each byte of it is written once).
inline void ExpandRectangle(const uint8_t* v0, const uint8_t* v1, const uint8_t* v2, uint32_t stride,
                            int32_t position_offset, std::span<const std::pair<uint32_t, uint32_t>> floats,
                            uint8_t* fourth, uint8_t* out) {
  const uint8_t* v[3] = {v0, v1, v2};
  uint32_t corner = 0;
  if (position_offset >= 0 && uint32_t(position_offset) + 8 <= stride) {
    float p[3][2];
    for (int k = 0; k < 3; ++k) {
      std::memcpy(p[k], v[k] + position_offset, 8);
    }
    float longest = -1.0f;
    for (uint32_t c = 0; c < 3; ++c) {
      const float dx = p[(c + 2) % 3][0] - p[(c + 1) % 3][0];
      const float dy = p[(c + 2) % 3][1] - p[(c + 1) % 3][1];
      const float length = dx * dx + dy * dy;
      if (length > longest) {
        longest = length;
        corner = c;
      }
    }
  }
  const uint8_t* c0 = v[corner];
  const uint8_t* c1 = v[(corner + 1) % 3];
  const uint8_t* c2 = v[(corner + 2) % 3];
  std::memcpy(fourth, c2, stride);
  for (const auto& [offset, components] : floats) {
    for (uint32_t k = 0; k < components && offset + 4 * k + 4 <= stride; ++k) {
      float a, b, c;
      std::memcpy(&a, c0 + offset + 4 * k, 4);
      std::memcpy(&b, c1 + offset + 4 * k, 4);
      std::memcpy(&c, c2 + offset + 4 * k, 4);
      const float x = b - a + c;
      std::memcpy(fourth + offset + 4 * k, &x, 4);
    }
  }
  std::memcpy(out, c0, stride);
  std::memcpy(out + stride, c1, stride);
  std::memcpy(out + 2 * stride, fourth, stride);
  std::memcpy(out + 3 * stride, c2, stride);
}

}  // namespace rex::graphics::gta4_native::dk
