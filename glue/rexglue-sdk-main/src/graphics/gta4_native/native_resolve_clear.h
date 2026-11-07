// gta4_native: which surfaces the clears that ride on a Resolve (docs/switch-port/16-deko3d-devlog.md, run 41, section L)
// apply to. The Xenos copy (RB_COPY_CONTROL) can clear the source it resolved and, with bit 9, the depth/stencil
// surface the device has bound, whatever kind of surface is resolved. The title uses it: at the end of the deferred
// phase it resolves the colour target "alias" with flags 0x300 (clear colour, clear depth) and clears the deferred
// depth and stencil for the next frame (gta4_native_hooks.cpp, SubmitNativeResolve(..., 768, ...)).
//
// The Vulkan path (graphics_system.cpp, RecordResolveClears) applies bit 8 to the colour target the resolve read
// (render target flags & 7) and bit 9 to the device's depth/stencil surface, independent of the resolved aspect, and
// refuses both when the command carries a clear-format override (`parameters_valid`: the override cannot be
// represented). The deko3d recorder cleared only the resolved source and counted bit 9 on a colour resolve as "the
// other aspect" (467 times in run 40, once per deferred frame) without doing it.
//
// No heavy headers: tests/deko3d/test_resolve_clear.py compiles this file on the PC.

#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native {

inline constexpr uint32_t kResolveClearColorBit = 1u << 8;
inline constexpr uint32_t kResolveClearDepthBit = 1u << 9;
inline constexpr uint32_t kResolveSourceDepth = 4;  // (flags & 7): the depth surface is what is resolved

struct ResolveClearPlan {
  bool clear_source_color = false;    // bit 8 on a colour resolve: the resolved colour target
  bool clear_source_depth = false;    // bit 9 on a depth resolve: the resolved depth surface
  bool clear_device_depth = false;    // bit 9 on a colour resolve: the device's depth/stencil surface (not the source)
  bool unrepresentable_color = false; // bit 8 on a depth resolve: no colour surface is named (Vulkan ignores it too)
};

constexpr ResolveClearPlan PlanResolveClears(uint32_t flags, bool parameters_valid) {
  ResolveClearPlan plan;
  const bool color = (flags & kResolveClearColorBit) != 0;
  const bool depth = (flags & kResolveClearDepthBit) != 0;
  const bool source_is_depth = (flags & 7u) == kResolveSourceDepth;
  plan.clear_source_color = color && !source_is_depth;
  plan.clear_source_depth = depth && source_is_depth;
  plan.unrepresentable_color = color && source_is_depth;
  // The new part: the depth/stencil clear of a colour resolve. A clear-format override cannot be represented.
  plan.clear_device_depth = depth && !source_is_depth && !parameters_valid;
  return plan;
}

}  // namespace rex::graphics::gta4_native
