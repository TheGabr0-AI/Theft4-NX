// gta4_native deko3d back end, run 58: run 57's features and the characters' suspects compared in one session
// (docs/switch-port/16-deko3d-devlog.md, section AF).
//
// Run 57 against run 56 could not say what each new feature did: the two sessions played different scenes (run 57: 4,422
// draws a frame against 4,074; the lights' pass 684 draws against 607). Here the recorder switches one thing at a time
// in the same session, in phases of three report windows (10 s each) of gameplay (draws per frame above the trigger),
// the first window of each phase left out (the switch settles: placements, the store, the pipeline caches):
//   baseline          everything as configured
//   keep targets off  nx_dk_keep_targets off (run 57: the lights' stencil passes keep the colour targets bound)
//   placement off     nx_dk_placement off (run 57: EDRAM placements; the exposure chain depends on it)
//   store off         the geometry store bypassed: every draw's vertices and indices through the upload ring
//   copies off        the front end copies no draw at submit (run 57: 65 % of the draws, 21 % of the title's render
//                     thread in memcpy); the sampled hashes go on, so the log still counts the changed draws
//   cull off          no face culling (the characters' holes: whole triangles missing in the G-buffer)
// One capture is asked for in the last window of each phase. Cut-scenes and loading screens do not move the phases.
// Run 59 adds two phases for its copies at submit by rewritten page (native_draw_snapshot.h VolatilePages) and a mask
// of the phases to run (nx_dk_ab_phases, AbPhaseBit; 0 = run 58's six):
//   pages off         no copy for a rewritten page (run 58's copies: locked, rewritten after the first fill, promoted)
//   pages only        no copy for a promoted buffer object (run 58's promotions) but for its pages
// Run 60 adds (bit 256):
//   probe shaders     every skinned draw whose vertex shader has a probe draws with that shader (the package's GLSL with
//                     storage-buffer writes, compiled by the console's uam) instead of the package's DKSH: the
//                     characters whole in this phase and in shards in the baseline = the package's compiled vertex
//                     shader is what places their vertices wrong (run 55's probe proved the GLSL, not that binary)
// Run 61 adds (bit 512), after run 60 showed the characters in shards in both of its phases:
//   skinned depth always  every skinned draw into the G-buffer (three or more colour targets and a depth target) has
//                     its depth compare made "always" (the test and the write stay on, nothing else changes): the
//                     characters whole in this phase = their fragments are rejected by the depth test (step 8); still in
//                     shards = they never reach it (rasterization or the pixel shader's kill, steps 6-7)
// No Switch headers: tests/deko3d/test_run58.py and test_run59.py drive it on the PC.

#pragma once

#include <array>
#include <cstdint>

namespace rex::graphics::gta4_native::dk {

enum class AbPhase : uint32_t {
  kBaseline,
  kKeepTargetsOff,
  kPlacementOff,
  kStoreOff,
  kCopiesOff,
  kCullOff,
  kPagesOff,
  kPagesOnly,
  kProbeShaders,  // run 60: skinned draws with the console-compiled instrumented vertex shader (dk_shader_probe.h)
  kSkinnedDepthAlways,  // run 61: skinned G-buffer draws with the depth compare "always"
  kCount
};

constexpr uint32_t AbPhaseBit(AbPhase p) { return 1u << uint32_t(p); }
// Run 58's cycle.
constexpr uint32_t kRun58Phases = (1u << uint32_t(AbPhase::kPagesOff)) - 1;

inline const char* AbPhaseName(AbPhase p) {
  switch (p) {
    case AbPhase::kBaseline: return "baseline";
    case AbPhase::kKeepTargetsOff: return "keep targets off";
    case AbPhase::kPlacementOff: return "placement off";
    case AbPhase::kStoreOff: return "store off";
    case AbPhase::kCopiesOff: return "copies off";
    case AbPhase::kCullOff: return "cull off";
    case AbPhase::kPagesOff: return "pages off";
    case AbPhase::kPagesOnly: return "pages only";
    case AbPhase::kProbeShaders: return "probe shaders";
    case AbPhase::kSkinnedDepthAlways: return "skinned depth always";
    default: return "?";
  }
}

// One report window of the recorder.
struct AbWindow {
  double seconds = 0;
  uint64_t frames = 0, draws = 0;
  double gpu_busy_ms = 0, gpu_gap_ms = 0;
  uint64_t materializations = 0, target_binds = 0;
  uint64_t rechecked = 0, changed = 0;  // run 59: sampled draws re-checked by the recorder, changed since submit
};

// What the counted windows of one phase add up to (all cycles).
struct AbTotals {
  uint32_t windows = 0;
  double seconds = 0;
  uint64_t frames = 0, draws = 0;
  double gpu_busy_ms = 0, gpu_gap_ms = 0;
  uint64_t materializations = 0, target_binds = 0;
  uint64_t rechecked = 0, changed = 0;

  void Add(const AbWindow& w) {
    ++windows;
    seconds += w.seconds;
    frames += w.frames;
    draws += w.draws;
    gpu_busy_ms += w.gpu_busy_ms;
    gpu_gap_ms += w.gpu_gap_ms;
    materializations += w.materializations;
    target_binds += w.target_binds;
    rechecked += w.rechecked;
    changed += w.changed;
  }
  double changed_percent() const { return rechecked ? 100.0 * double(changed) / double(rechecked) : 0; }
  double fps() const { return seconds > 0 ? double(frames) / seconds : 0; }
  double per_frame(double v) const { return frames ? v / double(frames) : 0; }
};

class AbSchedule {
 public:
  static constexpr uint32_t kWindowsPerPhase = 3;  // the first one settles and is not counted
  static constexpr uint32_t kStartStreak = 2;      // gameplay windows in a row before the first phase

  // `phases`: AbPhaseBit of the phases to run, the baseline always among them.
  explicit AbSchedule(uint64_t trigger_draws = 1500, uint32_t phases = kRun58Phases)
      : trigger_(trigger_draws), phases_((phases ? phases : kRun58Phases) | AbPhaseBit(AbPhase::kBaseline)) {}

  // One report window. True when the phase changed (the caller applies phase()); ended() then names the phase that
  // just ended (none before the first one).
  bool Step(const AbWindow& w) {
    ended_ = false;
    capture_ = false;
    const bool gameplay = w.frames && w.draws >= trigger_ * w.frames;
    if (!started_) {
      streak_ = gameplay ? streak_ + 1 : 0;
      if (streak_ < kStartStreak) {
        return false;
      }
      started_ = true;
      phase_ = AbPhase::kBaseline;
      windows_ = 0;
      return true;
    }
    if (!gameplay) {
      return false;  // a cut-scene or a loading screen: the phase waits
    }
    ++windows_;
    if (windows_ >= 2) {
      totals_[uint32_t(phase_)].Add(w);
    }
    if (windows_ == kWindowsPerPhase - 1) {
      capture_ = true;  // the next window is the phase's last
    }
    if (windows_ < kWindowsPerPhase) {
      return false;
    }
    ended_ = true;
    ended_phase_ = phase_;
    windows_ = 0;
    do {
      phase_ = AbPhase((uint32_t(phase_) + 1) % uint32_t(AbPhase::kCount));
      if (phase_ == AbPhase::kBaseline) {
        ++cycle_;
      }
    } while (!(phases_ & AbPhaseBit(phase_)));
    return true;
  }

  bool started() const { return started_; }
  AbPhase phase() const { return phase_; }
  uint32_t cycle() const { return cycle_; }
  bool ended() const { return ended_; }
  AbPhase ended_phase() const { return ended_phase_; }
  bool capture_now() const { return capture_; }
  const AbTotals& totals(AbPhase p) const { return totals_[uint32_t(p)]; }

 private:
  uint64_t trigger_;
  uint32_t phases_;
  bool started_ = false;
  uint32_t streak_ = 0;
  AbPhase phase_ = AbPhase::kBaseline;
  uint32_t windows_ = 0;
  uint32_t cycle_ = 0;
  bool ended_ = false;
  AbPhase ended_phase_ = AbPhase::kBaseline;
  bool capture_ = false;
  std::array<AbTotals, size_t(AbPhase::kCount)> totals_{};
};

}  // namespace rex::graphics::gta4_native::dk
