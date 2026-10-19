// Native renderer, step N2: render targets without EDRAM emulation
// (native_render_targets.h).

#include "native_render_targets.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/nx_gpu_counters.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_BOOL(native_render_targets, false, "GPU/Native",
                    "N2: render targets without EDRAM emulation (no ownership transfers, "
                    "resolves straight from the render target); read at the first frame");
REXCVAR_DEFINE_INT32(native_capture_every_s, 0, "GPU/Native",
                     "N2: every N seconds, one frame drawn emulated and the next drawn native, "
                     "both written as PPM to native_capture_dir; 0 = off");
REXCVAR_DEFINE_STRING(native_capture_dir, ".", "GPU/Native",
                      "N2: folder of the side-by-side captures (native_capture_every_s)");

namespace rex::graphics::native {

namespace {

using Clock = std::chrono::steady_clock;

enum class CapturePhase { kIdle, kEmulatedFrame, kNativeFrame };

struct State {
  bool initialized = false;
  bool base_mode = false;
  uint64_t frame = 0;
  Clock::time_point start, next_capture;
  CapturePhase phase = CapturePhase::kIdle;
  uint64_t pair = 0;
  uint64_t last_logged[kNativeTransferKindCount + 2] = {};
};

State& GetState() {
  static State state;
  return state;
}

bool WritePpm(const ui::RawImage& image, const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
  std::string row(size_t(image.width) * 3, '\0');
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3] = char(src[x * 4]);
      row[x * 3 + 1] = char(src[x * 4 + 1]);
      row[x * 3 + 2] = char(src[x * 4 + 2]);
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

void Capture(ui::Presenter* presenter, const State& s, const char* mode) {
  ui::RawImage image;
  if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) {
    REXGPU_WARN("[NativeRT] capture {} frame {}: no guest output", mode, s.frame);
    return;
  }
  char name[96];
  const double seconds = std::chrono::duration<double>(Clock::now() - s.start).count();
  std::snprintf(name, sizeof(name), "/n2-pair%02" PRIu64 "-frame%05" PRIu64 "-%s.ppm", s.pair,
                s.frame, mode);
  const std::string path = REXCVAR_GET(native_capture_dir) + name;
  const bool ok = WritePpm(image, path);
  REXGPU_INFO("[NativeRT] capture pair {} at {:.1f} s: frame {} drawn {} -> {} ({})", s.pair,
              seconds, s.frame, mode, path, ok ? "written" : "NOT written");
}

}  // namespace

std::atomic<bool>& NativeRenderTargets() {
  static std::atomic<bool> value{REXCVAR_GET(native_render_targets)};
  return value;
}

NativeRenderTargetCounters& NativeCounters() {
  static NativeRenderTargetCounters counters;
  return counters;
}

void OnNativeSwap(ui::Presenter* presenter) {
  State& s = GetState();
  auto& c = NativeCounters();
  if (!s.initialized) {
    s.initialized = true;
    s.base_mode = REXCVAR_GET(native_render_targets);
    s.start = Clock::now();
    s.next_capture = s.start + std::chrono::seconds(REXCVAR_GET(native_capture_every_s));
    REXGPU_INFO("[NativeRT] render targets: {} (cvar native_render_targets); captures every {} s "
                "into {}",
                s.base_mode ? "NATIVE (no EDRAM emulation)" : "emulated",
                REXCVAR_GET(native_capture_every_s), REXCVAR_GET(native_capture_dir));
  }
  // The frame that just ended.
  const bool was_native = NativeRenderTargets().load(std::memory_order_relaxed);
  (was_native ? c.native_frames : c.emulated_frames).fetch_add(1, std::memory_order_relaxed);
  if (was_native) {
    nx_gpu_counters().native_frames.fetch_add(1, std::memory_order_relaxed);
  }
  if (s.phase == CapturePhase::kEmulatedFrame) {
    Capture(presenter, s, "emulated");
    s.phase = CapturePhase::kNativeFrame;
  } else if (s.phase == CapturePhase::kNativeFrame) {
    Capture(presenter, s, "native");
    s.phase = CapturePhase::kIdle;
    ++s.pair;
  }
  ++s.frame;
  if (s.frame % 300 == 0) {
    uint64_t now[kNativeTransferKindCount + 2];
    for (int i = 0; i < kNativeTransferKindCount; ++i) {
      now[i] = c.transfers_skipped[i].load(std::memory_order_relaxed);
    }
    now[kNativeTransferKindCount] = c.resolves_direct.load(std::memory_order_relaxed);
    now[kNativeTransferKindCount + 1] = c.resolves_through_edram.load(std::memory_order_relaxed);
    auto d = [&](int i) { return now[i] - s.last_logged[i]; };
    REXGPU_INFO("[NativeRT] frames {}: last 300 frames ({} native so far, {} emulated): transfers "
                "not made colour->colour {}, colour->depth {}, depth->colour {}, depth->depth {}; "
                "resolves straight from the render target {}, through the EDRAM buffer {}",
                s.frame, c.native_frames.load(), c.emulated_frames.load(), d(kColorToColor),
                d(kColorToDepth), d(kDepthToColor), d(kDepthToDepth), d(kNativeTransferKindCount),
                d(kNativeTransferKindCount + 1));
    for (int i = 0; i < kNativeTransferKindCount + 2; ++i) s.last_logged[i] = now[i];
  }
  // The next frame's mode: the pair (emulated, then native) when a capture is due.
  bool next = s.base_mode;
  if (s.phase == CapturePhase::kIdle && REXCVAR_GET(native_capture_every_s) > 0 &&
      Clock::now() >= s.next_capture) {
    s.next_capture += std::chrono::seconds(REXCVAR_GET(native_capture_every_s));
    s.phase = CapturePhase::kEmulatedFrame;
  }
  if (s.phase == CapturePhase::kEmulatedFrame) next = false;
  if (s.phase == CapturePhase::kNativeFrame) next = true;
  NativeRenderTargets().store(next, std::memory_order_relaxed);
}

}  // namespace rex::graphics::native
