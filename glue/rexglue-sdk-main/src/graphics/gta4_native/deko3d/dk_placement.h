// gta4_native deko3d back end, run 57: EDRAM placements (docs/switch-port/16-deko3d-devlog.md, section AE).
//
// On Xenos a render target is a range of EDRAM: two surface objects that name the same range in the same sample space
// share their content, and GTA IV relies on it. Run 56's anatomy captures: the scene's HDR target 40076EB0 is drawn
// (values up to 32), but the whole luminance chain that measures the exposure (640x360 402268C0, 320x180 40226900 ...
// down to 1x1) resolves to exactly 0: every one of those surfaces is created by its first resolve (the log's "render
// target" line is followed at once by its "resolved texture" line), no draw ever binds it, so this recorder resolved
// freshly cleared images. The Vulkan path (Liberty Recompiled, kabba536: graphics_system.cpp FindPlacementOwner,
// ClaimSurfaceContent, RecordSurfaceMaterialization, surface_view.h GuestPlacementKey) keeps the owner of every EDRAM
// placement and, when another surface of the same placement is used, materializes the owner's content into it: an
// image copy when the views are identical, else a conversion pass between the sample layouts. Ported here for colour
// surfaces (the Vulkan path refuses depth views too): this header plans the copy; DkRecorder::PrepareSurfaceContent
// records it. One value per pixel here (the images are single-sample): a 4x view of a 1x surface holds the average of
// its 2x2 block (what its resolve produces), a 1x view of a 4x surface the value of its block.
// No Switch headers: tests/deko3d/test_run57.py compiles it on the PC.

#pragma once

#include <cstdint>

#include <rex/graphics/gta4_native/surface_view.h>

namespace rex::graphics::gta4_native::dk {

struct PlacementCopy {
  bool valid = false;
  bool same_layout = false;          // identical pixel grid: a copy (same format) or a 1:1 conversion
  uint32_t reduce_x = 1, reduce_y = 1;  // destination pixel = average of reduce_x x reduce_y source pixels
  uint32_t expand_x = 1, expand_y = 1;  // destination pixels per source pixel
};

// The owner's view and image size, the destination's: how the destination gets the owner's content.
inline PlacementCopy PlanPlacementCopy(const GuestSurfaceView& owner, uint32_t owner_width, uint32_t owner_height,
                                       const GuestSurfaceView& destination, uint32_t destination_width,
                                       uint32_t destination_height) {
  PlacementCopy p;
  if (GetGuestPlacementKey(owner) != GetGuestPlacementKey(destination) || owner.depth || destination.depth ||
      !owner_width || !owner_height || !destination_width || !destination_height) {
    return p;
  }
  const uint32_t ox = GuestSampleScaleX(owner.msaa_samples), oy = GuestSampleScaleY(owner.msaa_samples);
  const uint32_t dx = GuestSampleScaleX(destination.msaa_samples), dy = GuestSampleScaleY(destination.msaa_samples);
  // Same sample space (the key): owner pixels x ox == destination pixels x dx, per axis.
  if (owner_width * ox != destination_width * dx || owner_height * oy != destination_height * dy) {
    return p;
  }
  if (dx >= ox) {
    p.reduce_x = dx / ox;
  } else {
    p.expand_x = ox / dx;
  }
  if (dy >= oy) {
    p.reduce_y = dy / oy;
  } else {
    p.expand_y = oy / dy;
  }
  p.same_layout = p.reduce_x == 1 && p.reduce_y == 1 && p.expand_x == 1 && p.expand_y == 1;
  p.valid = true;
  return p;
}

// The factors are 1 or 2 (sample scales): log2 of the expansion, so the pass shifts instead of dividing (uam emulates a
// division by a non-constant integer in floating point, "with resulting loss of precision").
inline uint32_t PlacementShift(uint32_t expand) {
  return expand >= 2 ? 1u : 0u;
}

// The conversion pass (with kPackedDepthAliasVs, the full-screen triangle): each destination pixel from its source
// block. u.factor = (reduce x, reduce y, PlacementShift(expand x), PlacementShift(expand y)).
inline constexpr char kPlacementPs[] = R"(#version 460
layout(std140, binding = 0) uniform Placement { ivec4 factor; } u;
layout(binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 o;
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  ivec2 reduce = max(u.factor.xy, ivec2(1));
  ivec2 first = (pixel >> u.factor.zw) * reduce;
  vec4 sum = vec4(0.0);
  for (int y = 0; y < reduce.y; ++y) {
    for (int x = 0; x < reduce.x; ++x) {
      sum += texelFetch(source, first + ivec2(x, y), 0);
    }
  }
  o = sum / float(reduce.x * reduce.y);
}
)";

}  // namespace rex::graphics::gta4_native::dk
