// gta4_native deko3d back end, run 62: which stage of the GPU loses the characters' triangles
// (docs/switch-port/16-deko3d-devlog.md, section AJ).
//
// What runs 55-61 proved, link by link: the vertex shaders place every vertex where the PC does (run 55's probe, every
// vertex of four character meshes; run 60's probe files: no NaN, every vertex inside 0 <= z <= w, the meshes whole when
// the PC rasterizes them with the title's culling); the package's binary is not the cause (run 60); face culling is not
// (run 58's "cull off"); the depth test is not (run 61's "skinned depth always": still in shards). In the G-buffer whole
// triangles are missing in every target and in the depth (run 56). What is left lies between the vertex shader's output
// and the targets: primitive assembly, the clipper, the rasterizer, the pixel shader's kill and the stencil/sample tests.
//
// The GPU counts each of those stages itself. Around a sampled draw the recorder resets five counters and reports them
// after it (dkCmdBufResetCounter / dkCmdBufReportCounter, 16-byte reports in the core's "GPU reports" block):
//   input primitives       the triangles primitive assembly made (expected: indices / 3)
//   clipper input          the triangles that reached the clipper
//   clipper output         the triangles that left it (after clipping and culling; with "cull off" every one in range)
//   fragment invocations   the pixel shader's invocations
//   samples passed         the samples that passed every test and were written
// Two classes are sampled, the same way: the skinned G-buffer draws (the characters) and the other indexed G-buffer
// draws (the scenery: the control). A stage that loses the characters' triangles shows as a ratio the scenery does not
// have; a counter that does not count on this GPU shows as zero in both classes.
// No Switch headers: tests/deko3d/test_run62.py drives it on the PC.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rex::graphics::gta4_native::dk {

enum class StageCounter : uint32_t {
  kInputPrimitives,
  kClipperInput,
  kClipperOutput,
  kFragmentInvocations,
  kSamplesPassed,
  kCount
};
constexpr uint32_t kStageCounters = uint32_t(StageCounter::kCount);

enum class StageClass : uint32_t { kSkinnedGbuffer, kStaticGbuffer, kCount };
constexpr uint32_t kStageClasses = uint32_t(StageClass::kCount);

inline const char* StageClassName(StageClass c) {
  return c == StageClass::kSkinnedGbuffer ? "skinned G-buffer (characters)" : "static G-buffer (scenery, control)";
}

// One sampled draw's five values (the reports' first words) and what the CPU expected of it.
struct StageSample {
  StageClass cls = StageClass::kSkinnedGbuffer;
  uint64_t vertex_shader = 0;
  uint32_t expected_primitives = 0;  // indices / 3
  std::array<uint64_t, kStageCounters> value{};
};

struct StageTotals {
  uint64_t draws = 0;
  uint64_t expected = 0;
  std::array<uint64_t, kStageCounters> value{};
  uint64_t assembly_short = 0;  // draws whose input primitives fell short of indices / 3

  void Add(const StageSample& s) {
    ++draws;
    expected += s.expected_primitives;
    for (uint32_t i = 0; i < kStageCounters; ++i) {
      value[i] += s.value[i];
    }
    assembly_short += s.value[0] < s.expected_primitives;
  }
  static double Ratio(uint64_t a, uint64_t b) { return b ? double(a) / double(b) : 0.0; }
  // Input primitives / expected; clipper input / input; clipper output / clipper input; fragment invocations per
  // clipper-output triangle; samples passed / fragment invocations.
  double assembled() const { return Ratio(value[0], expected); }
  double reached_clipper() const { return Ratio(value[1], value[0]); }
  double left_clipper() const { return Ratio(value[2], value[1]); }
  double fragments_per_triangle() const { return Ratio(value[3], value[2]); }
  double samples_per_fragment() const { return Ratio(value[4], value[3]); }
};

// The report memory: a region of the core's "GPU reports" block after the pass stamps, one part per work slot.
struct StageCounterLayout {
  uint32_t base = 0;              // byte offset in the reports block
  uint32_t samples_per_slot = 0;  // draws a work can sample

  static constexpr uint32_t kReportBytes = 16;
  static constexpr uint32_t kSampleBytes = kStageCounters * kReportBytes;

  uint32_t Offset(uint32_t slot, uint32_t sample, uint32_t counter) const {
    return base + (slot * samples_per_slot + sample) * kSampleBytes + counter * kReportBytes;
  }
  uint32_t End(uint32_t slots) const { return base + slots * samples_per_slot * kSampleBytes; }
  // As many samples per slot as fit in `block_bytes` after `base`, at most `cap`.
  static StageCounterLayout Fit(uint32_t base, uint32_t block_bytes, uint32_t slots, uint32_t cap) {
    StageCounterLayout l;
    l.base = (base + 15u) & ~15u;
    const uint32_t room = block_bytes > l.base ? block_bytes - l.base : 0;
    l.samples_per_slot = std::min<uint32_t>(cap, room / (slots * kSampleBytes));
    return l;
  }
};

// Which draws are sampled: one in `every` of each class, while the work has room.
class StageSampler {
 public:
  explicit StageSampler(uint32_t every = 4) : every_(std::max<uint32_t>(every, 1)) {}
  bool Take(StageClass c, uint32_t used, uint32_t room) {
    if (used >= room) {
      return false;
    }
    return (seen_[uint32_t(c)]++ % every_) == 0;
  }

 private:
  uint32_t every_;
  std::array<uint64_t, kStageClasses> seen_{};
};

// The totals of one class and of its vertex shaders (the first 12 met).
class StageReport {
 public:
  void Add(const StageSample& s) {
    totals_[uint32_t(s.cls)].Add(s);
    if (s.cls != StageClass::kSkinnedGbuffer) {
      return;
    }
    for (auto& [vs, t] : shaders_) {
      if (vs == s.vertex_shader) {
        t.Add(s);
        return;
      }
    }
    if (shaders_.size() < 12) {
      shaders_.push_back({s.vertex_shader, StageTotals{}});
      shaders_.back().second.Add(s);
    }
  }
  const StageTotals& totals(StageClass c) const { return totals_[uint32_t(c)]; }
  const std::vector<std::pair<uint64_t, StageTotals>>& shaders() const { return shaders_; }

  // The verdict the two classes give, in one phrase (the log says it next to the numbers).
  static std::string Verdict(const StageTotals& skinned, const StageTotals& control) {
    if (!skinned.draws || !control.draws) {
      return "not enough sampled draws";
    }
    if (!control.value[0] && !control.value[2] && !control.value[3]) {
      return "the counters read zero in both classes: not counted on this GPU";
    }
    const auto below = [](double a, double b) { return b > 0 && a < 0.9 * b; };
    if (skinned.assembled() < 0.99) {
      return "the characters lose triangles at primitive assembly (fewer than indices / 3)";
    }
    if (below(skinned.reached_clipper(), control.reached_clipper())) {
      return "the characters lose triangles before the clipper";
    }
    if (below(skinned.left_clipper(), control.left_clipper())) {
      return "the characters lose triangles in the clipper (clipping or culling)";
    }
    if (below(skinned.samples_per_fragment(), control.samples_per_fragment())) {
      return "the characters lose fragments after the pixel shader starts (kill, stencil, depth or sample mask)";
    }
    return "no stage loses more of the characters than of the scenery: the triangles reach the targets";
  }

 private:
  std::array<StageTotals, kStageClasses> totals_{};
  std::vector<std::pair<uint64_t, StageTotals>> shaders_;
};

// The value of a 16-byte report: its first word (the second is the GPU timestamp).
inline uint64_t StageReportValue(const uint8_t* report) {
  uint64_t v = 0;
  std::memcpy(&v, report, sizeof(v));
  return v;
}

}  // namespace rex::graphics::gta4_native::dk
