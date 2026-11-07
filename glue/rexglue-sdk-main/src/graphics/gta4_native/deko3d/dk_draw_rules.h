// deko3d recorder: two per-draw rules of run 55 (docs/switch-port/16-deko3d-devlog.md section AC), kept apart so that
// tests/deko3d/test_run55.py checks them on the PC. No deko3d or SDK headers.

#pragma once

#include <bit>
#include <cstdint>

namespace rex::graphics::gta4_native::dk {

// Depth-only draws (banjo-nx / sa2-nx, Renut's renderer, package 28): a draw that writes no colour, with a depth/stencil
// target, without the alpha test or alpha to coverage, whose pixel shader (its variant without the alpha test) neither
// kills pixels nor writes depth, gives the same depth and stencil without its pixel shader. `colour_write_masks` is
// the write mask of each colour target the pixel shader writes (0: that target's colour is not written).
struct DepthOnlyInputs {
  bool enabled = true;
  bool has_pixel_shader = false;
  bool coverage = false;  // alpha test or alpha to coverage
  bool depth_target = false;
  bool flags_valid = false;
  bool kills = false;
  bool writes_depth = false;
  uint32_t colour_write_masks = 0;  // 4 bits per target, the targets the shader writes
};

inline bool IsDepthOnlyDraw(const DepthOnlyInputs& in) {
  return in.enabled && in.has_pixel_shader && !in.coverage && in.depth_target && in.flags_valid && !in.kills &&
         !in.writes_depth && in.colour_write_masks == 0;
}

// The title's depth bias as the Vulkan path gives it to vkCmdSetDepthBias (graphics_system.cpp): the constant times
// kD3D10PolygonOffsetFactorFloat24 (2^24), the slope times xenos::kPolygonOffsetScaleSubpixelUnit (1/16), at 1x
// resolution. Through run 54 the slope lacked the 1/16.
struct DepthBiasFactors {
  float constant = 0.0f;
  float slope = 0.0f;
};

inline DepthBiasFactors DepthBiasFor(uint32_t depth_bias_bits, uint32_t slope_scaled_depth_bias_bits) {
  return {std::bit_cast<float>(depth_bias_bits) * float(1u << 24),
          std::bit_cast<float>(slope_scaled_depth_bias_bits) * (1.0f / 16.0f)};
}

}  // namespace rex::graphics::gta4_native::dk
