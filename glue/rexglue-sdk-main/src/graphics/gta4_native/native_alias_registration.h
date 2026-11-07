// gta4_native: which virtual resource registrations of the title a recorder needs (docs/switch-port/16-deko3d-devlog.md,
// run 39). Only the title's A8R8G8B8 alias of a resolved depth (sub_828D9768): a texture object whose
// `packed_depth_source` is another texture object, the resolved depth it reinterprets. The same conditions the Vulkan
// path's RegisterVirtualResource handling insists on (texture kind, a source that is not itself).
//
// No heavy headers: tests/deko3d/test_depth_alias.py compiles this file on the PC.

#pragma once

#include <rex/graphics/gta4_native/title_commands.h>

namespace rex::graphics::gta4_native {

inline bool IsPackedDepthAliasRegistration(const RegisterVirtualResourceCommand& c) {
  return c.resource && c.packed_depth_source && c.packed_depth_source != c.resource &&
         c.kind == VirtualResourceKind::kTexture;
}

}  // namespace rex::graphics::gta4_native
