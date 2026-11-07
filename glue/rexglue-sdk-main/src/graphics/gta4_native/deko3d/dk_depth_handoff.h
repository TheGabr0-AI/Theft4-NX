// gta4_native deko3d back end: the scene depth hand-off (docs/switch-port/16-deko3d-devlog.md, run 44, section O).
//
// The title ends its G-buffer phase (sub_824F6AF0, the "deferred phase end" hook) with the resolves of the G-buffer
// and then one explicit command, DepthSurfaceHandoffCommand: the resolved scene depth (source_texture, a D24FS8 resolve,
// Xenos format 22 or 23) goes into the title's primary depth surface, the one the lights and the forward phase draw
// with, and the surface's stencil is rebuilt as a scene coverage mask (ForwardStencilHandoffPolicy::
// kRebuildSceneCoverage: 0xFF where the scene has depth, 0x80 where it has none; forward_stencil.h). On the Xenos that
// was a draw of PS_depthCopy0 through the packed depth alias; the Vulkan path replaces it with one full-screen pass
// (Gta4NativeGraphicsSystem::RecordDepthSurfaceHandoff, scene_depth_handoff_ps.glsl, depth_handoff_util.h) and this is
// the same pass for deko3d: clear the destination to depth 0 and stencil 0x80, then draw a full-screen triangle with
// the depth test always, the depth write on, the stencil test always with REPLACE of 0xFF, a pixel shader that discards
// where the packed scene depth is zero and writes the source depth otherwise.
//
// Until run 44 this front end dropped the command (counter "depth hand-offs", one per deferred frame), so the primary
// depth surface never held the scene: whatever tested against it in the lighting phase (the two-sided z-fail light
// volumes, the light draws) and the forward phase saw an empty or stale buffer.
//
// No Switch headers here: tests/deko3d/test_depth_handoff.py compiles this file on the PC.

#pragma once

#include <cstdint>

#include <rex/graphics/gta4_native/forward_stencil.h>

#include "dk_depth_alias.h"

namespace rex::graphics::gta4_native::dk {

// What the recorder does with one hand-off, in the order the Vulkan path rejects (RecordDepthSurfaceHandoff: surface
// creation, same object, source unwritten, aspects, extent, scene depth encoding).
enum class HandoffVerdict : uint8_t {
  kDone,
  kPolicyNotImplemented,  // kPreserve: nothing in the title sends it (the only sender uses kRebuildSceneCoverage)
  kPassUnavailable,       // the pass did not compile at start
  kNoSurface,             // the source or destination surface could not be made
  kSameObject,
  kSourceUnwritten,       // the resolved depth was never written (or is not a depth image)
  kExtent,
  kSceneDepthEncoding,    // the resolve is not guest format 22 (24_8) or 23 (24_8 float)
};

struct HandoffInputs {
  ForwardStencilHandoffPolicy policy = ForwardStencilHandoffPolicy::kPreserve;
  bool pass_available = false;
  bool source_surface = false;       // the deferred depth surface (the Vulkan path makes it too)
  bool destination_surface = false;  // the primary depth surface
  bool same_image = false;           // the resolved depth is the destination
  bool source_written = false;       // resolved_[source_texture] exists, was written, and is a depth image
  uint32_t source_width = 0, source_height = 0;
  uint32_t destination_width = 0, destination_height = 0;
  uint32_t source_guest_format = 0;  // the resolve's Xenos texture format
};

constexpr HandoffVerdict PlanDepthHandoff(const HandoffInputs& in) {
  if (in.policy != ForwardStencilHandoffPolicy::kRebuildSceneCoverage) {
    return HandoffVerdict::kPolicyNotImplemented;
  }
  if (!in.pass_available) {
    return HandoffVerdict::kPassUnavailable;
  }
  if (!in.source_surface || !in.destination_surface) {
    return HandoffVerdict::kNoSurface;
  }
  if (in.same_image) {
    return HandoffVerdict::kSameObject;
  }
  if (!in.source_written) {
    return HandoffVerdict::kSourceUnwritten;
  }
  if (in.source_width != in.destination_width || in.source_height != in.destination_height) {
    return HandoffVerdict::kExtent;
  }
  if (!IsDepthFetchFormat(in.source_guest_format)) {
    return HandoffVerdict::kSceneDepthEncoding;
  }
  return HandoffVerdict::kDone;
}

// What the pass leaves in one pixel of the destination: `covered` is whether the packed scene depth is not zero.
struct HandoffPixel {
  float depth;
  uint32_t stencil;
};

// The pass as one pixel sees it: the destination starts at depth 0 and stencil 0x80; a covered pixel gets the source depth
// and stencil 0xFF (the stencil REPLACE of the pixels that were not discarded).
constexpr HandoffPixel SceneHandoffPixel(bool float_depth, float source_depth, uint32_t source_depth_bits,
                                         uint32_t rounded_24) {
  const uint32_t packed = float_depth ? Float32To20e4(source_depth, source_depth_bits) : rounded_24;
  if (packed == 0) {
    return {0.0f, kForwardEmptySceneStencil};
  }
  return {source_depth, ForwardStencilFromPackedSceneDepth(packed)};
}

// The pixel shader (compiled by the console's uam at start, with the alias pass's vertex shader kPackedDepthAliasVs): binding
// 0 of the uniform block says whether the source is the 20e4 float of format 23, binding 0 the resolved depth.
// Same encoding and the same discard as scene_depth_handoff_ps.glsl.
inline constexpr char kSceneDepthHandoffPs[] = R"(#version 460
layout(std140, binding = 0) uniform Handoff { uvec4 mode; } u;
layout(binding = 0) uniform sampler2D source_depth;
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
  float depth = texelFetch(source_depth, ivec2(gl_FragCoord.xy), 0).r;
  uint packed_depth = u.mode.x != 0u ? depth20e4(depth) : uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0));
  if (packed_depth == 0u) discard;
  gl_FragDepth = depth;
}
)";

}  // namespace rex::graphics::gta4_native::dk
