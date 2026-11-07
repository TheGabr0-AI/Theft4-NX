// Probe of run 43 (devlog section N): the depth clamp decision the deko3d recorder makes for a draw. The decision is the
// shared policy header's (native_stencil_volume_policy.h, the Vulkan path's own); the recorder calls it with the volume
// state of the draw and light_setup_phase = true. Prints lines the Python test checks. Host clang, no Switch headers.
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "native_stencil_volume_policy.h"

using namespace rex::graphics::gta4_native;

// The recorder's call: the guest's own request, the phase flag fixed to true (no lighting context in this front end).
static bool RecorderClamp(bool guest_request, const NativeStencilVolumeDepthState& s) {
  return ShouldEnableNativeDepthClampForDraw(guest_request, true, s);
}

// The rule written out again from the Vulkan path's description: depth test without depth write, stencil on, two-sided,
// no culling, no colour write, and one face whose depth-fail op is not keep while its pass op is keep and its mask is not 0.
static bool Expected(bool guest, const NativeStencilVolumeDepthState& s) {
  const bool no_cull = s.cull_mode == 0 || s.cull_mode == 4;
  const bool front = s.stencil_depth_fail != 0 && s.stencil_pass == 0 && s.stencil_write_mask != 0;
  const bool back = s.ccw_stencil_depth_fail != 0 && s.ccw_stencil_pass == 0 && s.back_stencil_write_mask != 0;
  const bool volume = s.depth_enable && !s.depth_write_enable && s.stencil_enable && s.two_sided_stencil && no_cull &&
                      !s.active_color_write && (front || back);
  return guest || volume;
}

int main() {
  // 1. Named states.
  NativeStencilVolumeDepthState zfail;
  zfail.depth_enable = 1;
  zfail.stencil_enable = 1;
  zfail.two_sided_stencil = 1;
  zfail.cull_mode = 0;
  zfail.stencil_depth_fail = 6;  // increment with wrap
  zfail.stencil_write_mask = 0xFF;
  zfail.ccw_stencil_depth_fail = 7;  // decrement with wrap
  zfail.back_stencil_write_mask = 0xFF;
  std::printf("ZFAIL volume: %d\n", RecorderClamp(false, zfail));

  NativeStencilVolumeDepthState s = zfail;
  s.active_color_write = true;
  std::printf("ZFAIL with colour write: %d\n", RecorderClamp(false, s));
  s = zfail;
  s.depth_write_enable = 1;
  std::printf("ZFAIL with depth write (a G-buffer draw): %d\n", RecorderClamp(false, s));
  s = zfail;
  s.stencil_depth_fail = 0;
  s.stencil_pass = 6;
  s.ccw_stencil_depth_fail = 0;
  s.ccw_stencil_pass = 7;
  std::printf("Z-pass volume: %d\n", RecorderClamp(false, s));
  s = zfail;
  s.cull_mode = 1;
  std::printf("culled: %d\n", RecorderClamp(false, s));
  s = zfail;
  s.two_sided_stencil = 0;
  std::printf("one-sided: %d\n", RecorderClamp(false, s));
  s = zfail;
  s.stencil_write_mask = 0;
  s.back_stencil_write_mask = 0;
  std::printf("zero write mask: %d\n", RecorderClamp(false, s));
  s = zfail;
  s.stencil_depth_fail = 0;  // only the back face counts on depth fail
  std::printf("one face z-fail: %d\n", RecorderClamp(false, s));
  s = NativeStencilVolumeDepthState{};
  std::printf("plain draw: %d\n", RecorderClamp(false, s));
  std::printf("the title's own request on a plain draw: %d\n", RecorderClamp(true, s));

  // 2. Every combination of the fields the rule reads, against the rule written out again.
  uint32_t cases = 0, bad = 0, clamped = 0;
  for (uint32_t bits = 0; bits < (1u << 6); ++bits) {
    for (const uint32_t cull : {0u, 1u, 2u, 4u}) {
      for (const uint32_t op : {0u, 3u, 6u}) {
        for (const uint32_t op2 : {0u, 7u}) {
          for (const uint32_t pass : {0u, 6u}) {
            for (const uint32_t mask : {0u, 0xFFu}) {
              for (const uint32_t cpass : {0u, 5u}) {
                NativeStencilVolumeDepthState t;
                t.depth_enable = bits & 1;
                t.depth_write_enable = (bits >> 1) & 1;
                t.stencil_enable = (bits >> 2) & 1;
                t.two_sided_stencil = (bits >> 3) & 1;
                t.active_color_write = (bits >> 4) & 1;
                t.cull_mode = cull;
                t.stencil_depth_fail = op;
                t.ccw_stencil_depth_fail = op2;
                t.stencil_pass = pass;
                t.ccw_stencil_pass = cpass;
                t.stencil_write_mask = mask;
                t.back_stencil_write_mask = mask;
                const bool guest = (bits >> 5) & 1;
                const bool got = RecorderClamp(guest, t);
                ++cases;
                bad += got != Expected(guest, t);
                clamped += got;
              }
            }
          }
        }
      }
    }
  }
  std::printf("RULE cases %u bad %u clamped %u\n", cases, bad, clamped);
  return 0;
}
