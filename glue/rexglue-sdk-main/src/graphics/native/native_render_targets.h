// Native renderer, step N2: render targets without EDRAM emulation.
//
// In native mode the host render targets of the Vulkan backend are plain
// images, one per guest render target (base, pitch, format, samples), and
// nothing is copied between two of them when they alias the same EDRAM range
// (no ownership transfers), except the kinds kept by cvar
// native_transfer_mask. A resolve copies straight from the render target into
// guest memory or the destination texture (the direct host resolve shaders),
// never through the EDRAM buffer when a direct variant exists. The shader
// translator, the pipeline cache, the texture cache, the shared memory and the
// PM4 decoding are the backend's, unchanged.
//
// Tools for the image (all on the command processor thread):
// - side-by-side captures (cvar native_capture_every_s): one frame drawn
//   emulated, the next drawn native, written as PPM;
// - sweeps (cvar native_sweep): pairs of frames back to back, the first
//   emulated, the second with one configuration under test, compared in the
//   process (share of pixels that differ), one CSV row per pair:
//     "single": emulated, but only the i-th transfer of the frame not made
//               (i = pair % 96): which single transfer changes the image, and
//               the state of the draw that caused it;
//     "mask":   native with native_transfer_mask = the pair's mask
//               (native_sweep_masks, default 0-15);
// - for every transfer not made, whether the destination is then read: by
//   the next draw that binds it (depth/stencil test, blending, partial colour
//   mask), or overwritten by a clear first, or not used again in the frame.
//
// The modes are atomics so that one run can change them frame by frame.

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::ui {
class Presenter;
}

namespace rex::graphics::native {

enum NativeTransferKind : int {
  kColorToColor = 0,
  kColorToDepth,
  kDepthToColor,
  kDepthToDepth,
  kNativeTransferKindCount
};

struct NativeRenderTargetCounters {
  std::atomic<uint64_t> transfers_skipped[kNativeTransferKindCount] = {};
  std::atomic<uint64_t> transfers_made[kNativeTransferKindCount] = {};
  std::atomic<uint64_t> resolves_direct{0};
  std::atomic<uint64_t> resolves_through_edram{0};  // direct not possible: EDRAM buffer path
  std::atomic<uint64_t> native_frames{0}, emulated_frames{0};
  // A transfer not made, then: read by the next draw binding its
  // destination, written over without reading by it, cleared, or unused.
  std::atomic<uint64_t> skipped_then_read{0}, skipped_then_written{0};
  std::atomic<uint64_t> skipped_then_cleared{0}, skipped_then_unused{0};
};

// The draw (or resolve clear) whose render target update causes the next
// transfers; set by RenderTargetCache::Update before its ownership changes.
struct NativeDrawContext {
  bool clear = false;              // a resolve clear, not a draw
  uint32_t depth_control = 0;      // normalized RB_DEPTHCONTROL
  uint32_t color_mask = 0;         // normalized, 4 bits per colour render target
  uint32_t blend_mask = 0;         // bit i: colour render target i blends
  uint32_t stencil_ref_mask = 0;   // RB_STENCILREFMASK
  uint32_t primitive_type = 0;     // VGT_DRAW_INITIATOR prim_type
  uint64_t vertex_shader_hash = 0;
  uint32_t surface_info = 0;       // RB_SURFACE_INFO
};

std::atomic<bool>& NativeRenderTargets();
NativeRenderTargetCounters& NativeCounters();

void SetDrawContext(const NativeDrawContext& context);

// Called for every transfer the emulation would make (source != destination);
// true = do not make it. Counts it, by kind, either way.
bool DecideTransfer(uint32_t source_key, bool source_is_depth, uint32_t dest_key,
                    bool dest_is_depth, uint32_t start_tiles, uint32_t end_tiles);

// After a draw's ownership changes: the render targets it binds (slot 0
// depth, 1-4 colour; key 0 = none) and the draw's state.
void OnDrawBound(const uint32_t keys[5]);

// Called once per guest swap by the Vulkan command processor, after the guest
// output was refreshed: counters every 300 frames, captures, sweeps, and the
// mode of the next frame.
void OnNativeSwap(ui::Presenter* presenter);

}  // namespace rex::graphics::native
