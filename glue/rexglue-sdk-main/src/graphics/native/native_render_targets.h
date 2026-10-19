// Native renderer, step N2: render targets without EDRAM emulation.
//
// In native mode the host render targets of the Vulkan backend are plain
// images, one per guest render target (base, pitch, format, samples), and
// nothing is ever copied between two of them when they alias the same EDRAM
// range: no ownership transfers. A resolve copies straight from the render
// target into guest memory or the destination texture (the direct host resolve
// shaders), never through the EDRAM buffer. The shader translator, the
// pipeline cache, the texture cache, the shared memory and the PM4 decoding
// are the backend's, unchanged.
//
// What would have been transferred is counted by kind: those are the places
// where the game reads a render target's contents through another render
// target (depth as colour, colour as depth, a different format or pitch), the
// cases a native renderer must handle on purpose.
//
// The mode is an atomic so that one run can alternate it frame by frame for
// the side-by-side captures (cvar native_capture_every_s).

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
  std::atomic<uint64_t> resolves_direct{0};
  std::atomic<uint64_t> resolves_through_edram{0};  // direct not possible: EDRAM buffer path
  std::atomic<uint64_t> native_frames{0}, emulated_frames{0};
};

// Whether the frame being recorded uses native render targets (initially the
// cvar native_render_targets).
std::atomic<bool>& NativeRenderTargets();
NativeRenderTargetCounters& NativeCounters();

// Called once per guest swap by the Vulkan command processor, after the guest
// output was refreshed: logs the counters every 300 frames, captures the
// side-by-side pairs, and sets the mode of the next frame.
void OnNativeSwap(ui::Presenter* presenter);

}  // namespace rex::graphics::native
