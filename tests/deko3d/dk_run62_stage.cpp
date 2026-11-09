// Run 62 offline test (tests/deko3d/test_run62.py): dk_stage_counters.h's layout, sampler and verdicts, and dk_ab.h's
// schedule for run 62's mask (baseline, cull off, skinned depth always). Prints one line per check: "STAGE <name> ok|FAIL".
#include <cstdio>
#include <set>

#include "dk_ab.h"
#include "dk_stage_counters.h"

using namespace rex::graphics::gta4_native::dk;

static int failures = 0;
static void Check(const char* name, bool ok) {
  std::printf("STAGE %s %s\n", name, ok ? "ok" : "FAIL");
  failures += !ok;
}

static StageSample Sample(StageClass c, uint32_t expected, uint64_t in, uint64_t clip_in, uint64_t clip_out,
                          uint64_t frags, uint64_t samples) {
  StageSample s;
  s.cls = c;
  s.vertex_shader = c == StageClass::kSkinnedGbuffer ? 0x9A16E1CEAD4ABA0Dull : 0x1234;
  s.expected_primitives = expected;
  s.value = {in, clip_in, clip_out, frags, samples};
  return s;
}

int main() {
  // The recorder's numbers: 3 slots of 1,000 16-byte stamps, a 64 KiB block, 32 samples a slot at most.
  const StageCounterLayout l = StageCounterLayout::Fit(3 * 1000 * 16, 64 << 10, 3, 32);
  Check("layout-fits", l.samples_per_slot == 32 && l.End(3) <= (64u << 10) && l.base == 48000);
  std::set<uint32_t> offsets;
  bool aligned = true;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    for (uint32_t i = 0; i < l.samples_per_slot; ++i) {
      for (uint32_t c = 0; c < kStageCounters; ++c) {
        const uint32_t o = l.Offset(slot, i, c);
        aligned = aligned && (o % 16) == 0 && o >= 48000 && o + 16 <= l.End(3);
        offsets.insert(o);
      }
    }
  }
  Check("layout-distinct-aligned", aligned && offsets.size() == 3u * 32u * kStageCounters);
  Check("layout-small-block", StageCounterLayout::Fit(48000, 48000, 3, 32).samples_per_slot == 0);

  StageSampler sampler(4);
  uint32_t taken = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    taken += sampler.Take(StageClass::kSkinnedGbuffer, 0, 32);
  }
  Check("sampler-one-in-four", taken == 4);
  StageSampler full(1);
  Check("sampler-room", !full.Take(StageClass::kStaticGbuffer, 32, 32) && full.Take(StageClass::kStaticGbuffer, 31, 32));

  // Verdicts. Scenery (control): every triangle assembled and clipped in, half culled, 20 fragments each, all written.
  const StageTotals control = [] {
    StageTotals t;
    t.Add(Sample(StageClass::kStaticGbuffer, 1000, 1000, 1000, 500, 10000, 10000));
    return t;
  }();
  auto skinned = [](uint32_t expected, uint64_t in, uint64_t ci, uint64_t co, uint64_t f, uint64_t s) {
    StageTotals t;
    t.Add(Sample(StageClass::kSkinnedGbuffer, expected, in, ci, co, f, s));
    return t;
  };
  const std::string zero = StageReport::Verdict(skinned(1000, 0, 0, 0, 0, 0), [] {
    StageTotals t;
    t.Add(Sample(StageClass::kStaticGbuffer, 1000, 0, 0, 0, 0, 0));
    return t;
  }());
  Check("verdict-not-counted", zero.find("not counted") != std::string::npos);
  Check("verdict-assembly",
        StageReport::Verdict(skinned(1000, 700, 700, 350, 7000, 7000), control).find("assembly") != std::string::npos);
  Check("verdict-clipper",
        StageReport::Verdict(skinned(1000, 1000, 1000, 200, 4000, 4000), control).find("in the clipper") != std::string::npos);
  Check("verdict-fragments",
        StageReport::Verdict(skinned(1000, 1000, 1000, 500, 10000, 6000), control).find("after the pixel shader") !=
            std::string::npos);
  Check("verdict-reach-targets",
        StageReport::Verdict(skinned(1000, 1000, 1000, 510, 9000, 9000), control).find("reach the targets") !=
            std::string::npos);
  Check("verdict-empty", StageReport::Verdict(StageTotals{}, control).find("not enough") != std::string::npos);

  StageReport r;
  r.Add(Sample(StageClass::kSkinnedGbuffer, 100, 100, 100, 40, 800, 800));
  r.Add(Sample(StageClass::kSkinnedGbuffer, 100, 90, 90, 40, 800, 800));
  r.Add(Sample(StageClass::kStaticGbuffer, 100, 100, 100, 50, 900, 900));
  const StageTotals& t = r.totals(StageClass::kSkinnedGbuffer);
  Check("report-totals", t.draws == 2 && t.expected == 200 && t.value[0] == 190 && t.assembly_short == 1 &&
                             r.shaders().size() == 1 && r.totals(StageClass::kStaticGbuffer).draws == 1);
  uint8_t report[16] = {0x39, 0x30, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  Check("report-value", StageReportValue(report) == 12345);

  // Run 62's A/B mask: baseline (1), cull off (32), skinned depth always (512) = 545, in that order, cycling.
  AbSchedule ab(1500, 545);
  AbWindow w;
  w.seconds = 10;
  w.frames = 50;
  w.draws = 50 * 2000;
  std::vector<AbPhase> seen;
  for (int i = 0; i < 2 + 3 * 7; ++i) {
    if (ab.Step(w)) {
      seen.push_back(ab.phase());
    }
  }
  const std::vector<AbPhase> want = {AbPhase::kBaseline, AbPhase::kCullOff, AbPhase::kSkinnedDepthAlways,
                                     AbPhase::kBaseline, AbPhase::kCullOff, AbPhase::kSkinnedDepthAlways,
                                     AbPhase::kBaseline, AbPhase::kCullOff};
  Check("ab-545", seen == want);
  std::printf("STAGE failures %d\n", failures);
  return failures ? 1 : 0;
}
