// gta4_native deko3d back end: the packed depth alias (docs/switch-port/16-deko3d-devlog.md, run 38, section I).
//
// The title samples its resolved depth (a D24FS8 resolve, Xenos formats 22 and 23) through a second texture object that
// names the same memory as an A8R8G8B8 texture (its sub_828D9768; the Vulkan path registers it as a virtual resource
// with `packed_depth_source`, and Gta4NativeGraphicsSystem::RecordPackedDepthAlias converts it with
// packed_depth_alias_ps.glsl). On the Xenos that is plain memory aliasing: the resolve wrote 24 bits of depth and 8 of
// stencil in one big-endian word, and an 8_8_8_8 fetch reads its four bytes after the 4-byte swap, i.e. the bytes of
//     packed = (depth24 << 8) | stencil        R = stencil, G = depth bits 7..0, B = 15..8, A = 23..16
// before the fetch constant's swizzle. The recorder keeps a resolved depth as a ZF32_X24S8 image and, until this
// pass, returned it for such a fetch as a float in every channel, which no shader that unpacks the bytes can read.
//
// No Switch headers here: tests/deko3d/test_depth_alias.py compiles this file on the PC.

#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native::dk {

// Xenos texture formats (xenos::TextureFormat) the decision looks at.
inline constexpr uint32_t kFetchFormat8888 = 6;
inline constexpr uint32_t kFetchFormat8888As16 = 50;  // the same bytes; FormatOf maps both to RGBA8
inline constexpr uint32_t kFetchFormat24_8 = 22;
inline constexpr uint32_t kFetchFormat24_8Float = 23;

inline constexpr bool IsDepthFetchFormat(uint32_t format) {
  return format == kFetchFormat24_8 || format == kFetchFormat24_8Float;
}

// A fetch of the title's resolved depth as bytes: the image is a depth image, the fetch constant asks for A8R8G8B8,
// and both have the same size (the Vulkan path refuses an alias of another size too).
inline constexpr bool IsPackedDepthFetch(bool image_is_depth, uint32_t fetch_format, uint32_t fetch_width,
                                         uint32_t fetch_height, uint32_t image_width, uint32_t image_height) {
  return image_is_depth && (fetch_format == kFetchFormat8888 || fetch_format == kFetchFormat8888As16) &&
         fetch_width == image_width && fetch_height == image_height && image_width && image_height;
}

// Xenos Float32To20e4 (truncating), the same steps as depth20e4 in the shader below; `depth` is the guest's [0, 1].
inline constexpr uint32_t Float32To20e4(float depth, uint32_t bits_of_depth) {
  if (!(depth > 0.0f)) {
    return 0;
  }
  uint32_t bits = bits_of_depth;
  if (bits >= 0x3FFFFFF8u) {
    return 0xFFFFFFu;
  }
  if (bits < 0x38800000u) {
    const uint32_t exponent = bits >> 23;
    const uint32_t shift = 113u - exponent < 24u ? 113u - exponent : 24u;
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  return (bits >> 3) & 0xFFFFFFu;
}

// What the pass writes for one texel: the four bytes of `packed`, R first. `float_depth`: guest format 23.
// `depth_bits` is the IEEE bit pattern of `depth`; `rounded_24` the round-to-even of clamp(depth) * 16777215.
inline constexpr uint32_t PackDepthStencil(bool float_depth, float depth, uint32_t depth_bits, uint32_t rounded_24,
                                           uint32_t stencil) {
  const uint32_t d = float_depth ? Float32To20e4(depth, depth_bits) : rounded_24;
  return (d << 8) | (stencil & 255u);
}

// The pixel shader of the pass (compiled by the console's uam at start, like the output pass). A full-screen triangle
// runs it over an RGBA8 image of the depth's size; binding 0 is the resolved depth, binding 1 its stencil, binding 0
// of the uniform block says whether the depth is the 20e4 float of format 23. The swizzle of the guest's fetch
// constant is not applied here: the sampled view of the alias carries it, like any other texture's.
inline constexpr char kPackedDepthAliasPs[] = R"(#version 460
layout(std140, binding = 0) uniform Alias { uvec4 mode; } u;
layout(binding = 0) uniform sampler2D depth_source;
layout(binding = 1) uniform usampler2D stencil_source;
layout(location = 0) out vec4 o;
uint depth20e4(float depth) {
  if (!(depth > 0.0)) return 0u;
  uint bits = floatBitsToUint(depth);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (bits < 0x38800000u) {
    uint shift = min(113u - (bits >> 23u), 24u);
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  return (bits >> 3u) & 0xFFFFFFu;
}
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  float depth = texelFetch(depth_source, pixel, 0).r;
  uint stencil = texelFetch(stencil_source, pixel, 0).r & 255u;
  uint packed_depth = u.mode.x != 0u ? depth20e4(depth) : uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0));
  uint packed_word = (packed_depth << 8u) | stencil;
  o = vec4(uvec4(packed_word, packed_word >> 8u, packed_word >> 16u, packed_word >> 24u) & 255u) / 255.0;
}
)";

// The vertex shader: a full-screen triangle, no inputs and no uniforms.
inline constexpr char kPackedDepthAliasVs[] = R"(#version 460
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

}  // namespace rex::graphics::gta4_native::dk
