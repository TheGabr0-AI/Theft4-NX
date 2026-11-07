// deko3d recorder: one skinned vertex recomputed on the CPU from the data the GPU is given (run 53, 16-deko3d-devlog.md
// section AA).
//
// Why: runs 48-52 showed characters in pieces while their inputs (weights summing to 1, indices inside the palette) and
// their constants (front end against recorder, 0 differences in 240,531 checks) looked right. Those checks compare our
// own front end with our own recorder; they cannot say whether the palette itself is a sensible set of bone matrices,
// nor whether the skinned vertex lands near its bones. This does, for the first vertex of a sampled skinned draw, with
// the decoding the GPU does:
//   - weights and indices as the vertex fetch gives them: D3DCOLOR (0x182886) is read BGRA, so (x, y, z, w) = guest
//     bytes (1, 2, 3, 0); UBYTE4N (0x1A2086) after the 32-bit swap read RGBA, so (x, y, z, w) = bytes (3, 2, 1, 0);
//   - the bone register as the translated shaders compute it: floor(index / 255 * 765.0059 + 0.5) (3 registers a
//     bone), rows c[64 + r], c[65 + r], c[66 + r];
//   - the transform as RAGE's skinning: p'_k = dot(row_k, (p, 1)) summed with the weights.
// It reports whether each used bone is a rigid transform (rows or columns orthonormal within 5 %), and how far the
// skinned vertex is from the weighted bone origins. Plausible data with a broken picture puts the fault in the shader
// path; implausible data puts it before (inputs, palette).
//
// No deko3d or SDK headers: tests/deko3d/test_run53.py builds it.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rex::graphics::gta4_native::dk {

struct SkinCheck {
  bool decoded = false;       // types understood
  int bones_used = 0;         // influences with a weight > 0
  int bones_not_rigid = 0;    // used bones whose 3x3 is not orthonormal by rows nor by columns
  int bones_zero = 0;         // used bones whose three rows are all zero
  int bones_past_palette = 0; // register + 2 past c255
  float weight_sum = 0.0f;
  float distance = 0.0f;      // |skinned vertex - weighted bone origin|
  float skinned[3] = {};
  float origin[3] = {};
  uint32_t registers[4] = {};
  float weights[4] = {};
};

inline bool SkinComponents(uint32_t type, const uint8_t* guest, float out[4]) {
  if (type == 0x182886) {  // D3DCOLOR: BGRA
    out[0] = guest[1] / 255.0f;
    out[1] = guest[2] / 255.0f;
    out[2] = guest[3] / 255.0f;
    out[3] = guest[0] / 255.0f;
    return true;
  }
  if (type == 0x1A2086) {  // UBYTE4N
    out[0] = guest[3] / 255.0f;
    out[1] = guest[2] / 255.0f;
    out[2] = guest[1] / 255.0f;
    out[3] = guest[0] / 255.0f;
    return true;
  }
  return false;
}

inline float LoadGuestFloat(const uint8_t* p) {
  const uint32_t u = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// `bank`: the vertex constants (256 vec4, host floats) the draw's GPU block holds. `position`: the guest bytes of a
// float3/float4 position (big-endian).
inline SkinCheck CheckSkinnedVertex(const float* bank, const uint8_t* position, uint32_t weights_type,
                                    const uint8_t* weights_guest, uint32_t indices_type, const uint8_t* indices_guest) {
  SkinCheck r;
  float w[4], ix[4];
  if (!SkinComponents(weights_type, weights_guest, w) || !SkinComponents(indices_type, indices_guest, ix)) {
    return r;
  }
  r.decoded = true;
  const float p[4] = {LoadGuestFloat(position), LoadGuestFloat(position + 4), LoadGuestFloat(position + 8), 1.0f};
  for (int i = 0; i < 4; ++i) {
    r.weights[i] = w[i];
    r.weight_sum += w[i];
    const uint32_t reg = uint32_t(std::floor(ix[i] * 765.005859375f + 0.5f));
    r.registers[i] = reg;
    if (w[i] <= 0.0f) {
      continue;
    }
    ++r.bones_used;
    if (reg + 2 >= 192) {
      ++r.bones_past_palette;
      continue;
    }
    const float* row[3] = {bank + (64 + reg) * 4, bank + (65 + reg) * 4, bank + (66 + reg) * 4};
    bool zero = true;
    for (int k = 0; k < 3; ++k) {
      for (int c = 0; c < 4; ++c) {
        zero = zero && row[k][c] == 0.0f;
      }
    }
    r.bones_zero += zero;
    auto orthonormal = [](const float a[3], const float b[3], const float c[3]) {
      auto dot = [](const float x[3], const float y[3]) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
      return std::fabs(dot(a, a) - 1.0f) < 0.05f && std::fabs(dot(b, b) - 1.0f) < 0.05f &&
             std::fabs(dot(c, c) - 1.0f) < 0.05f && std::fabs(dot(a, b)) < 0.05f && std::fabs(dot(a, c)) < 0.05f &&
             std::fabs(dot(b, c)) < 0.05f;
    };
    const float r0[3] = {row[0][0], row[0][1], row[0][2]}, r1[3] = {row[1][0], row[1][1], row[1][2]},
                r2[3] = {row[2][0], row[2][1], row[2][2]};
    const float c0[3] = {row[0][0], row[1][0], row[2][0]}, c1[3] = {row[0][1], row[1][1], row[2][1]},
                c2[3] = {row[0][2], row[1][2], row[2][2]};
    r.bones_not_rigid += !(orthonormal(r0, r1, r2) || orthonormal(c0, c1, c2));
    for (int k = 0; k < 3; ++k) {
      r.skinned[k] += w[i] * (row[k][0] * p[0] + row[k][1] * p[1] + row[k][2] * p[2] + row[k][3] * p[3]);
      r.origin[k] += w[i] * row[k][3];
    }
  }
  const float dx = r.skinned[0] - r.origin[0], dy = r.skinned[1] - r.origin[1], dz = r.skinned[2] - r.origin[2];
  r.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  return r;
}

}  // namespace rex::graphics::gta4_native::dk
