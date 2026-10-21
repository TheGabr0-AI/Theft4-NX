// gta4_native deko3d back end: the Xbox 360 GPU's texture arithmetic that does not depend on the graphics API
// (docs/switch-port/16-deko3d-devlog.md, M3 B2).
//
// Ported from nfsmw-nx (StevensND and contributors, GPL-3.0), app/src/nfsmw_nativo_xenos.h at 266f943: the tiled
// address of a block (2D and 3D, copies of rexglue's pipeline/texture/util.cpp GetTiledOffset2D/3D), the mip
// layout with its packed tail (GetPackedMipLevel, GetPackedMipOffset), the fast untiling (one address per row,
// 16 contiguous source bytes copied at once; checked block by block against the plain address there) and the
// byte swap of a whole texture with NEON. English names; nothing else changed.

#pragma once

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace rex::graphics::gta4_native::dk {

inline uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
inline uint32_t Log2Floor(uint32_t v) { return v ? 31 - uint32_t(std::countl_zero(v)) : 0; }

// First level of the packed tail: once the short side is 16 texels or less.
inline uint32_t PackedLevel(uint32_t width, uint32_t height) {
  const uint32_t l = Log2Ceil(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
inline void PackedOffset(uint32_t width, uint32_t height, uint32_t block, uint32_t level, uint32_t& x, uint32_t& y) {
  const uint32_t l2_width = Log2Ceil(width);
  const uint32_t l2_height = Log2Ceil(height);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;  // wider than tall: levels stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}

// Address of a block in a texture tiled in 32x32 blocks. pitch in blocks.
inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// The same for a 3D texture tiled in 32x32x4 blocks. pitch and height in blocks.
inline int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = outer + ((((x >> 3) + (outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// Untiles bx x by blocks starting at block (ox, oy) of a 2D level tiled with `pitch` blocks per row into rows of
// bx_host blocks. Same addresses as TiledOffset2D.
template <uint32_t kLog2>
inline void UntileLevel(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* destination) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_per_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_per_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* out = destination + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t at = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) + (((y8 + (x >> 3)) & 3) << 6) +
                         (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(out + size_t(column) * kBytes, source + at, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(out + size_t(column) * kBytes, source + at, kBytes);
        ++column;
      }
    }
  }
}

// Byte swap of a whole texture: for 16-bit units only k8in16 changes anything; for 32-bit units k8in16, k8in32 and
// k16in32 (xenos::Endian). Only complete units are touched.
inline void SwapTextureBytes(uint8_t* data, size_t bytes, uint32_t unit, uint32_t order) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;
  const size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && order != k8in16) || (unit != 2 && unit != 4) || order == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(data + i);
    uint8x16_t r;
    if (unit == 2 || order == k8in16) {
      r = vrev16q_u8(v);
    } else if (order == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(data + i, r);
  }
#endif
  if (unit == 2 || order == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (order == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (order == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// 32-bit words from guest (big-endian) to host order, with NEON (Gta4NativeGraphicsSystem's CopyGuestWordsToHost).
inline void CopyGuestWords(uint8_t* destination, const uint8_t* source, size_t size) {
  size_t offset = 0;
#if defined(__aarch64__)
  for (; size - offset >= 64; offset += 64) {
    uint8x16x4_t block = vld1q_u8_x4(source + offset);
    block.val[0] = vrev32q_u8(block.val[0]);
    block.val[1] = vrev32q_u8(block.val[1]);
    block.val[2] = vrev32q_u8(block.val[2]);
    block.val[3] = vrev32q_u8(block.val[3]);
    vst1q_u8_x4(destination + offset, block);
  }
  for (; size - offset >= 16; offset += 16) {
    vst1q_u8(destination + offset, vrev32q_u8(vld1q_u8(source + offset)));
  }
#endif
  while (size - offset >= 4) {
    uint32_t w;
    std::memcpy(&w, source + offset, 4);
    w = __builtin_bswap32(w);
    std::memcpy(destination + offset, &w, 4);
    offset += 4;
  }
  if (offset != size) {
    std::memcpy(destination + offset, source + offset, size - offset);
  }
}

}  // namespace rex::graphics::gta4_native::dk
