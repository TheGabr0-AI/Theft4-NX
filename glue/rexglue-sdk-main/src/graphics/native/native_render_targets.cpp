// Native renderer, step N2: render targets without EDRAM emulation
// (native_render_targets.h).

#include "native_render_targets.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/nx_gpu_counters.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_BOOL(native_render_targets, false, "GPU/Native",
                    "N2: render targets without EDRAM emulation (no ownership transfers but the "
                    "kinds of native_transfer_mask, resolves straight from the render target); "
                    "read at the first frame");
REXCVAR_DEFINE_INT32(native_transfer_mask, 32, "GPU/Native",
                     "N2: ownership transfers still made with native render targets, 4 bits: 1 "
                     "colour->colour, 2 colour->depth, 4 depth->colour, 8 depth->depth; 0 = none; "
                     "+32 = also every (source, destination) pair seen read by the next draw in "
                     "an earlier frame (learned)");
REXCVAR_DEFINE_INT32(native_capture_every_s, 0, "GPU/Native",
                     "N2: every N seconds, one frame drawn emulated and the next drawn native, "
                     "both written as PPM to native_capture_dir; 0 = off");
REXCVAR_DEFINE_STRING(native_capture_dir, ".", "GPU/Native",
                      "N2: folder of the captures and of native-sweep.csv");
REXCVAR_DEFINE_STRING(native_sweep, "", "GPU/Native",
                      "N2: \"single\" = pairs (emulated, emulated with only the i-th transfer not "
                      "made); \"mask\" = pairs (emulated, native with the pair's mask); compared "
                      "in the process, rows in native-sweep.csv; empty = off");
REXCVAR_DEFINE_STRING(native_sweep_masks, "", "GPU/Native",
                      "N2: masks of the \"mask\" sweep, comma-separated (empty = 0 to 15)");
REXCVAR_DEFINE_DOUBLE(native_sweep_save_over, 101.0, "GPU/Native",
                      "N2: sweep pairs whose difference (percent of pixels) is at least this are "
                      "also written as PPM");

namespace rex::graphics::native {

namespace {

using Clock = std::chrono::steady_clock;

// Sweeps use triplets: emulated, test, emulated again; the two emulated frames
// tell whether the scene changed in between (only still moments compare well).
enum class CapturePhase { kIdle, kEmulatedFrame, kTestFrame, kAfterFrame };

struct State {
  bool initialized = false;
  bool base_mode = false;
  uint64_t frame = 0;
  Clock::time_point start, next_capture, last_log_time;
  CapturePhase phase = CapturePhase::kIdle;
  bool sweep_pair = false;  // the current pair is a sweep pair (not a timed capture)
  uint64_t pair = 0;
  uint64_t sweep_pairs = 0;
  std::string sweep;
  std::vector<int> sweep_masks;
  // The frame under test.
  int test_mask = 0;
  int single_target = -1;
  ui::RawImage emulated_image, test_image;
  uint64_t emulated_frame = 0, test_frame = 0;
  double test_difference = 0.0;
  std::string pending_row;  // the CSV row of the test frame, completed after the third frame
  // Per frame.
  uint32_t frame_transfer_index = 0;
  uint64_t frame_made = 0, frame_skipped = 0;
  bool single_hit = false;
  char single_detail[320] = {};
  NativeDrawContext draw;
  // Destination key -> the (source, destination) pair of the transfer not made.
  std::unordered_map<uint32_t, uint64_t> pending;
  // (source, destination) -> times the destination was then read / written
  // over by the next draw binding it (for the learned mask, +32).
  struct PairUse {
    uint32_t reads = 0, writes = 0;
  };
  std::unordered_map<uint64_t, PairUse> pair_use;
  uint64_t last_draws = 0, frame_draws = 0;
  std::FILE* csv = nullptr;
  uint64_t last_logged[17] = {};
};

State& GetState() {
  static State state;
  return state;
}

std::atomic<int>& Mask() {
  static std::atomic<int> value{REXCVAR_GET(native_transfer_mask) & 0x2F};
  return value;
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

// Share of pixels whose |dR| + |dG| + |dB| is over 24, in percent.
double DifferencePercent(const ui::RawImage& a, const ui::RawImage& b) {
  if (a.width != b.width || a.height != b.height || !a.width) return -1.0;
  uint64_t differ = 0;
  for (uint32_t y = 0; y < a.height; ++y) {
    const uint8_t* pa = a.data.data() + size_t(y) * a.stride;
    const uint8_t* pb = b.data.data() + size_t(y) * b.stride;
    for (uint32_t x = 0; x < a.width; ++x) {
      const int d = std::abs(int(pa[x * 4]) - int(pb[x * 4])) +
                    std::abs(int(pa[x * 4 + 1]) - int(pb[x * 4 + 1])) +
                    std::abs(int(pa[x * 4 + 2]) - int(pb[x * 4 + 2]));
      differ += d > 24;
    }
  }
  return 100.0 * double(differ) / (double(a.width) * double(a.height));
}

bool Capture(ui::Presenter* presenter, ui::RawImage* image) {
  return presenter && presenter->CaptureGuestOutput(*image) && image->width;
}

std::string SavePath(const char* kind, uint64_t pair, uint64_t frame, const char* mode) {
  char name[128];
  std::snprintf(name, sizeof(name), "/n2-%s%02" PRIu64 "-frame%05" PRIu64 "-%s.ppm", kind, pair,
                frame, mode);
  return REXCVAR_GET(native_capture_dir) + name;
}

const char* KindName(int kind) {
  static const char* const kNames[] = {"colour->colour", "colour->depth", "depth->colour",
                                       "depth->depth"};
  return kind >= 0 && kind < kNativeTransferKindCount ? kNames[kind] : "?";
}

void SettlePending(State& s) {
  // Destinations not bound again before the end of the frame.
  NativeCounters().skipped_then_unused.fetch_add(s.pending.size(), std::memory_order_relaxed);
  s.pending.clear();
}

void ParseMasks(State& s) {
  const std::string& list = REXCVAR_GET(native_sweep_masks);
  size_t at = 0;
  while (at < list.size()) {
    const size_t comma = list.find(',', at);
    const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos
                                                                         : comma - at);
    // 16 = control: the test frame drawn emulated too (the noise between two
    // consecutive frames of the same renderer).
    if (!item.empty()) s.sweep_masks.push_back(std::atoi(item.c_str()) & 0x3F);
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  if (s.sweep_masks.empty()) {
    for (int m = 0; m < 16; ++m) s.sweep_masks.push_back(m);
  }
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

void SetDrawContext(const NativeDrawContext& context) { GetState().draw = context; }

bool DecideTransfer(uint32_t source_key, bool source_is_depth, uint32_t dest_key,
                    bool dest_is_depth, uint32_t start_tiles, uint32_t end_tiles) {
  State& s = GetState();
  const int kind = source_is_depth ? (dest_is_depth ? kDepthToDepth : kDepthToColor)
                                   : (dest_is_depth ? kColorToDepth : kColorToColor);
  const uint32_t index = s.frame_transfer_index++;
  bool skip = false;
  const uint64_t pair = (uint64_t(source_key) << 32) | dest_key;
  if (NativeRenderTargets().load(std::memory_order_relaxed)) {
    const int mask = Mask().load(std::memory_order_relaxed);
    skip = !((mask >> kind) & 1);
    if (skip && (mask & 32)) {
      auto it = s.pair_use.find(pair);
      skip = it == s.pair_use.end() || it->second.reads == 0;
    }
  } else if (s.single_target >= 0 && index == uint32_t(s.single_target)) {
    skip = true;
    s.single_hit = true;
    const NativeDrawContext& d = s.draw;
    std::snprintf(s.single_detail, sizeof(s.single_detail),
                  "%s src %08X dst %08X tiles %u..%u; %s: depthcontrol %08X (z %u, stencil %u, "
                  "stencilfunc %u, zfunc %u) stencilrefmask %08X colormask %04X blend %X prim %u "
                  "vs %016" PRIX64 " surface %08X",
                  KindName(kind), source_key, dest_key, start_tiles, end_tiles,
                  d.clear ? "resolve clear" : "draw", d.depth_control, (d.depth_control >> 1) & 1,
                  d.depth_control & 1, (d.depth_control >> 8) & 7, (d.depth_control >> 4) & 7,
                  d.stencil_ref_mask, d.color_mask, d.blend_mask, d.primitive_type,
                  d.vertex_shader_hash, d.surface_info);
  }
  auto& c = NativeCounters();
  if (skip) {
    c.transfers_skipped[kind].fetch_add(1, std::memory_order_relaxed);
    nx_gpu_counters().native_transfers_not_made.fetch_add(1, std::memory_order_relaxed);
    ++s.frame_skipped;
    if (s.draw.clear) {
      // The clear covers the destination's range: nothing will read the old contents.
      c.skipped_then_cleared.fetch_add(1, std::memory_order_relaxed);
    } else {
      s.pending[dest_key] = pair;
    }
  } else {
    c.transfers_made[kind].fetch_add(1, std::memory_order_relaxed);
    ++s.frame_made;
    if (NativeRenderTargets().load(std::memory_order_relaxed)) {
      nx_gpu_counters().native_transfers_made.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return skip;
}

void OnDrawBound(const uint32_t keys[5]) {
  State& s = GetState();
  if (s.pending.empty()) return;
  const NativeDrawContext& d = s.draw;
  auto& c = NativeCounters();
  for (int slot = 0; slot < 5; ++slot) {
    if (!keys[slot]) continue;
    auto it = s.pending.find(keys[slot]);
    if (it == s.pending.end()) continue;
    bool read;
    if (slot == 0) {
      read = (d.depth_control & 0x3) != 0;  // stencil or depth test
    } else {
      const uint32_t mask = (d.color_mask >> ((slot - 1) * 4)) & 0xF;
      read = ((d.blend_mask >> (slot - 1)) & 1) || (mask != 0 && mask != 0xF);
    }
    (read ? c.skipped_then_read : c.skipped_then_written).fetch_add(1, std::memory_order_relaxed);
    auto& use = s.pair_use[it->second];
    (read ? use.reads : use.writes) += 1;
    s.pending.erase(it);
  }
}

void OnNativeSwap(ui::Presenter* presenter) {
  State& s = GetState();
  auto& c = NativeCounters();
  if (!s.initialized) {
    s.initialized = true;
    s.base_mode = REXCVAR_GET(native_render_targets);
    s.start = s.last_log_time = Clock::now();
    s.next_capture = s.start + std::chrono::seconds(REXCVAR_GET(native_capture_every_s));
    s.sweep = REXCVAR_GET(native_sweep);
    ParseMasks(s);
    if (!s.sweep.empty()) {
      const std::string path = REXCVAR_GET(native_capture_dir) + "/native-sweep.csv";
      s.csv = std::fopen(path.c_str(), "w");
      if (s.csv) {
        std::fputs("pair,frame,draws,mode,param,difference_percent,transfers_made,"
                   "transfers_not_made,skipped_transfer,control_percent\n",
                   s.csv);
      }
    }
    REXGPU_INFO("[NativeRT] render targets: {} (cvar native_render_targets), transfer mask {}; "
                "captures every {} s; sweep '{}'; into {}",
                s.base_mode ? "NATIVE (no EDRAM emulation)" : "emulated", Mask().load(),
                REXCVAR_GET(native_capture_every_s), s.sweep, REXCVAR_GET(native_capture_dir));
  }
  // The frame that just ended.
  const uint64_t draws = nx_gpu_counters().draws.load(std::memory_order_relaxed);
  s.frame_draws = draws - s.last_draws;
  s.last_draws = draws;
  SettlePending(s);
  const bool was_native = NativeRenderTargets().load(std::memory_order_relaxed);
  (was_native ? c.native_frames : c.emulated_frames).fetch_add(1, std::memory_order_relaxed);
  if (was_native) {
    nx_gpu_counters().native_frames.fetch_add(1, std::memory_order_relaxed);
  }
  if (s.phase == CapturePhase::kEmulatedFrame) {
    s.emulated_image = ui::RawImage();
    if (!Capture(presenter, &s.emulated_image)) {
      REXGPU_WARN("[NativeRT] capture of frame {}: no guest output", s.frame);
      s.phase = CapturePhase::kIdle;
    } else {
      s.emulated_frame = s.frame;
      if (!s.sweep_pair) {
        WritePpm(s.emulated_image, SavePath("pair", s.pair, s.frame, "emulated"));
      }
      s.phase = CapturePhase::kTestFrame;
    }
  } else if (s.phase == CapturePhase::kTestFrame) {
    ui::RawImage test;
    const bool captured = Capture(presenter, &test);
    const double difference = captured ? DifferencePercent(s.emulated_image, test) : -1.0;
    if (!s.sweep_pair) {
      if (captured) WritePpm(test, SavePath("pair", s.pair, s.frame, "native"));
      REXGPU_INFO("[NativeRT] capture pair {}: frames {} (emulated) / {} (native), {:.1f}% of "
                  "pixels differ",
                  s.pair, s.emulated_frame, s.frame, difference);
      ++s.pair;
      s.phase = CapturePhase::kIdle;
      s.emulated_image = ui::RawImage();
    } else {
      const bool single = s.sweep == "single";
      const int param = single ? s.single_target : s.test_mask;
      char row[512];
      std::snprintf(row, sizeof(row),
                    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s,%d,%.3f,%" PRIu64 ",%" PRIu64 ",\"%s\"",
                    s.sweep_pairs, s.frame, s.frame_draws, single ? "single" : "mask", param,
                    difference, s.frame_made, s.frame_skipped,
                    single && s.single_hit ? s.single_detail : "");
      s.pending_row = row;
      s.test_image = std::move(test);
      s.test_frame = s.frame;
      s.test_difference = difference;
      s.phase = CapturePhase::kAfterFrame;
    }
  } else if (s.phase == CapturePhase::kAfterFrame) {
    ui::RawImage after;
    const double control =
        Capture(presenter, &after) ? DifferencePercent(s.emulated_image, after) : -1.0;
    if (s.csv) {
      std::fprintf(s.csv, "%s,%.3f\n", s.pending_row.c_str(), control);
      std::fflush(s.csv);
    }
    // Images only of still moments (the two emulated frames almost equal).
    if (control >= 0.0 && control < 0.3 &&
        s.test_difference >= REXCVAR_GET(native_sweep_save_over)) {
      const bool single = s.sweep == "single";
      char kind[32];
      std::snprintf(kind, sizeof(kind), "%s%d-pair", single ? "single" : "mask",
                    single ? -1 : s.test_mask);
      WritePpm(s.emulated_image, SavePath(kind, s.sweep_pairs, s.emulated_frame, "emulated"));
      WritePpm(s.test_image, SavePath(kind, s.sweep_pairs, s.test_frame, "test"));
    }
    ++s.sweep_pairs;
    s.phase = CapturePhase::kIdle;
    s.emulated_image = ui::RawImage();
    s.test_image = ui::RawImage();
  }
  ++s.frame;
  // Every 300 frames, and every 10 s at most: the counters.
  const auto now = Clock::now();
  if (s.frame % 300 == 0 || now - s.last_log_time > std::chrono::seconds(10)) {
    const double window_s = std::chrono::duration<double>(now - s.last_log_time).count();
    s.last_log_time = now;
    auto& g = nx_gpu_counters();
    uint64_t v[17] = {
        c.transfers_skipped[0].load(), c.transfers_skipped[1].load(), c.transfers_skipped[2].load(),
        c.transfers_skipped[3].load(), c.transfers_made[0].load(),    c.transfers_made[1].load(),
        c.transfers_made[2].load(),    c.transfers_made[3].load(),    c.resolves_direct.load(),
        c.resolves_through_edram.load(), c.skipped_then_read.load(),  c.skipped_then_written.load(),
        c.skipped_then_cleared.load(), c.skipped_then_unused.load(),  g.cp_idle_ns.load(),
        g.cp_wait_reg_ns.load(), g.cp_refresh_ns.load()};
    uint64_t d[17];
    for (int i = 0; i < 17; ++i) {
      d[i] = v[i] - s.last_logged[i];
      s.last_logged[i] = v[i];
    }
    static uint64_t last_frame = 0;
    const uint64_t frames = s.frame - last_frame;
    last_frame = s.frame;
    const double per = frames ? 1.0 / double(frames) : 0.0;
    REXGPU_INFO(
        "[NativeRT] frames {}: {} frames in {:.1f} s ({:.1f} ms per frame, {} draws in the last); "
        "per frame: transfers not made {:.1f}/{:.1f}/{:.1f}/{:.1f} (cc/cd/dc/dd), made "
        "{:.1f}/{:.1f}/{:.1f}/{:.1f}; not made then read {:.1f}, written {:.1f}, cleared {:.1f}, "
        "unused {:.1f}; resolves direct {:.1f}, through EDRAM {:.1f}; command processor idle "
        "(empty ring) {:.1f} ms, in WAIT_REG_MEM {:.1f} ms, in RefreshGuestOutput {:.1f} ms",
        s.frame, frames, window_s, frames ? window_s * 1000.0 / double(frames) : 0.0,
        s.frame_draws, d[0] * per, d[1] * per, d[2] * per, d[3] * per, d[4] * per, d[5] * per,
        d[6] * per, d[7] * per, d[10] * per, d[11] * per, d[12] * per, d[13] * per, d[8] * per,
        d[9] * per, double(d[14]) / 1e6 * per, double(d[15]) / 1e6 * per,
        double(d[16]) / 1e6 * per);
  }
  // The next frame.
  s.frame_transfer_index = 0;
  s.frame_made = s.frame_skipped = 0;
  s.single_hit = false;
  s.single_target = -1;
  bool next_native = s.base_mode;
  if (s.phase == CapturePhase::kIdle) {
    if (!s.sweep.empty()) {
      s.phase = CapturePhase::kEmulatedFrame;
      s.sweep_pair = true;
    } else if (REXCVAR_GET(native_capture_every_s) > 0 && now >= s.next_capture) {
      s.next_capture += std::chrono::seconds(REXCVAR_GET(native_capture_every_s));
      s.phase = CapturePhase::kEmulatedFrame;
      s.sweep_pair = false;
    }
  }
  if (s.phase == CapturePhase::kEmulatedFrame || s.phase == CapturePhase::kAfterFrame) {
    next_native = false;
  } else if (s.phase == CapturePhase::kTestFrame) {
    if (s.sweep_pair && s.sweep == "single") {
      next_native = false;
      s.single_target = int(s.sweep_pairs % 96);
    } else if (s.sweep_pair) {
      s.test_mask = s.sweep_masks[s.sweep_pairs % s.sweep_masks.size()];
      next_native = s.test_mask != 16;
      Mask().store(s.test_mask & 0x2F, std::memory_order_relaxed);
    } else {
      next_native = true;
    }
  }
  if (!s.sweep_pair || s.phase != CapturePhase::kTestFrame) {
    Mask().store(REXCVAR_GET(native_transfer_mask) & 0x2F, std::memory_order_relaxed);
  }
  NativeRenderTargets().store(next_native, std::memory_order_relaxed);
}

}  // namespace rex::graphics::native
