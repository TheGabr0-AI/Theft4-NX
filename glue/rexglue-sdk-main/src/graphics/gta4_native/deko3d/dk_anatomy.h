// gta4_native deko3d back end, run 56: the frame's anatomy (docs/switch-port/16-deko3d-devlog.md, section AD).
//
// Runs 38-55 looked only at the shown frame: the characters' holes, the triangular shards over the city and the sky
// and the washed-out day could each come from any of the frame's passes (the G-buffer, the depth the title resolves
// and hands back, the shadow maps, the light accumulation, the exposure's luminance, the post-processing). With
// nx_dk_anatomy, the frame after a capture saves every resolve's destination (what the title will sample next) and
// the shown image, each with its value ranges and its NaN/Inf count, so the first target that shows a fault names
// the pass to look at. Here: the texels as read back (deko3d's layout, little endian) to 8-bit RGB for a PNG.
// No Switch headers: tests/deko3d/test_run56.py compiles it on the PC.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rex::graphics::gta4_native::dk {

enum class AnatomyFormat : uint8_t { kRGBA8, kRGB10A2, kRG16F, kRGBA16F, kR32F, kZ32FS8, kUnknown };

inline uint32_t AnatomyBytesPerTexel(AnatomyFormat f) {
  switch (f) {
    case AnatomyFormat::kRGBA16F:
    case AnatomyFormat::kZ32FS8: return 8;
    case AnatomyFormat::kUnknown: return 0;
    default: return 4;
  }
}

inline float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  const uint32_t exponent = (h >> 10) & 0x1F;
  const uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (!mantissa) {
      bits = sign;
    } else {  // subnormal: normalise
      int e = -1;
      uint32_t m = mantissa;
      do {
        ++e;
        m <<= 1;
      } while (!(m & 0x400));
      bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FF) << 13;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | mantissa << 13;
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

struct AnatomyStats {
  uint32_t channels = 0;
  float min[4] = {0, 0, 0, 0}, max[4] = {0, 0, 0, 0};
  double mean[4] = {0, 0, 0, 0};
  uint64_t nan = 0, inf = 0;  // texels with a NaN / an infinite channel
  float scale = 1.0f;         // float formats: the value shown as 255 (the 99th percentile of the brightest channel)
  std::string Describe() const {
    std::string s;
    char part[96];
    for (uint32_t c = 0; c < channels; ++c) {
      std::snprintf(part, sizeof(part), "%s%c %.4g..%.4g mean %.4g", c ? ", " : "", "RGBA"[c], min[c], max[c], mean[c]);
      s += part;
    }
    std::snprintf(part, sizeof(part), "; NaN texels %llu, Inf texels %llu; PNG 255 = %.4g", (unsigned long long)nan,
                  (unsigned long long)inf, scale);
    return s + part;
  }
};

// The texel's channels as floats (unorm formats in 0..1; depth: R depth, G stencil).
inline uint32_t AnatomyTexel(AnatomyFormat f, const uint8_t* p, float out[4]) {
  switch (f) {
    case AnatomyFormat::kRGBA8:
      for (int c = 0; c < 4; ++c) out[c] = p[c] / 255.0f;
      return 4;
    case AnatomyFormat::kRGB10A2: {
      uint32_t v;
      std::memcpy(&v, p, 4);
      out[0] = (v & 0x3FF) / 1023.0f;
      out[1] = ((v >> 10) & 0x3FF) / 1023.0f;
      out[2] = ((v >> 20) & 0x3FF) / 1023.0f;
      out[3] = (v >> 30) / 3.0f;
      return 4;
    }
    case AnatomyFormat::kRG16F: {
      uint16_t h[2];
      std::memcpy(h, p, 4);
      out[0] = HalfToFloat(h[0]);
      out[1] = HalfToFloat(h[1]);
      return 2;
    }
    case AnatomyFormat::kRGBA16F: {
      uint16_t h[4];
      std::memcpy(h, p, 8);
      for (int c = 0; c < 4; ++c) out[c] = HalfToFloat(h[c]);
      return 4;
    }
    case AnatomyFormat::kR32F:
      std::memcpy(&out[0], p, 4);
      return 1;
    case AnatomyFormat::kZ32FS8:
      std::memcpy(&out[0], p, 4);
      out[1] = float(p[4]);
      return 2;
    default:
      return 0;
  }
}

// RGB for the PNG and the stats. Unorm formats as they are; float formats scaled so the 99th percentile of the
// brightest channel is 255 (negative clamped to 0); depth: depth stretched over its own range into G and B, the
// stencil in R. NaN and Inf texels are shown magenta, so a reader sees where they are.
inline AnatomyStats AnatomyToRgb(AnatomyFormat f, const uint8_t* texels, uint32_t width, uint32_t height,
                                 std::vector<uint8_t>& rgb) {
  AnatomyStats st;
  const uint32_t bpp = AnatomyBytesPerTexel(f);
  const size_t n = size_t(width) * height;
  rgb.assign(n * 3, 0);
  if (!bpp || !n) {
    return st;
  }
  float v[4];
  std::vector<float> peaks;
  peaks.reserve(std::min<size_t>(n, 1 << 16));
  bool first = true;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t ch = AnatomyTexel(f, texels + i * bpp, v);
    st.channels = ch;
    bool bad = false;
    float peak = 0.0f;
    for (uint32_t c = 0; c < ch; ++c) {
      if (std::isnan(v[c])) {
        ++st.nan;
        bad = true;
        break;
      }
      if (std::isinf(v[c])) {
        ++st.inf;
        bad = true;
        break;
      }
    }
    if (bad) {
      continue;
    }
    for (uint32_t c = 0; c < ch; ++c) {
      if (first) {
        st.min[c] = st.max[c] = v[c];
      } else {
        st.min[c] = std::min(st.min[c], v[c]);
        st.max[c] = std::max(st.max[c], v[c]);
      }
      st.mean[c] += v[c];
      if (c < 3) peak = std::max(peak, v[c]);
    }
    first = false;
    if ((i & 15) == 0) peaks.push_back(peak);
  }
  const uint64_t good = n - st.nan - st.inf;
  for (uint32_t c = 0; c < st.channels; ++c) {
    st.mean[c] = good ? st.mean[c] / double(good) : 0.0;
  }
  const bool is_float = f == AnatomyFormat::kRG16F || f == AnatomyFormat::kRGBA16F || f == AnatomyFormat::kR32F;
  if (is_float && !peaks.empty()) {
    const size_t k = std::min(peaks.size() - 1, peaks.size() * 99 / 100);
    std::nth_element(peaks.begin(), peaks.begin() + std::ptrdiff_t(k), peaks.end());
    st.scale = peaks[k] > 0.0f ? peaks[k] : 1.0f;
  }
  const float dmin = st.min[0], drange = st.max[0] > st.min[0] ? st.max[0] - st.min[0] : 1.0f;
  auto byte = [](float x) { return uint8_t(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
  for (size_t i = 0; i < n; ++i) {
    const uint32_t ch = AnatomyTexel(f, texels + i * bpp, v);
    uint8_t* o = rgb.data() + i * 3;
    bool bad = false;
    for (uint32_t c = 0; c < ch; ++c) bad = bad || !std::isfinite(v[c]);
    if (bad) {
      o[0] = 255, o[1] = 0, o[2] = 255;
      continue;
    }
    if (f == AnatomyFormat::kZ32FS8) {
      const uint8_t d = byte((v[0] - dmin) / drange);
      o[0] = uint8_t(v[1]), o[1] = d, o[2] = d;
    } else if (is_float) {
      for (uint32_t c = 0; c < 3; ++c) o[c] = c < ch ? byte(v[c] / st.scale) : 0;
      if (ch == 1) o[1] = o[2] = o[0];
    } else {
      for (uint32_t c = 0; c < 3; ++c) o[c] = byte(v[c]);
    }
  }
  return st;
}

}  // namespace rex::graphics::gta4_native::dk
