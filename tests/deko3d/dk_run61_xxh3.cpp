// Run 61 (tests/deko3d/test_run61.py):
// - XXH3-64 of deterministic buffers by the reference implementation (thirdparty/xxHash, the one the runtime hashes
//   shader containers with), to check tools/deko3d/gta4_xbox_data.py;
// - dk_ab.h's mask 513: baseline then "skinned depth always", and back.
#include <cinttypes>
#include <cstdio>
#include <string_view>
#include <vector>

#include "dk_ab.h"
#include "xxhash.h"

using namespace rex::graphics::gta4_native::dk;

static int failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("AB %s: %s\n", ok ? "ok" : "FAIL", what);
  failures += !ok;
}

int main() {
  const unsigned lengths[] = {0,   1,   2,   3,   4,   7,   8,   9,    15,   16,   17,   31,   32,   33,   63,
                              64,  65,  96,  97,  127, 128, 129, 200,  239,  240,  241,  255,  256,  511,  1023,
                              1024, 1025, 1088, 2047, 2048, 2049, 4000, 4464, 5016, 9999, 65537};
  for (unsigned n : lengths) {
    std::vector<uint8_t> b(n);
    uint32_t s = 0x12345678u ^ n;
    for (unsigned i = 0; i < n; ++i) {
      s = s * 1103515245u + 12345u;
      b[i] = uint8_t(s >> 16);
    }
    std::printf("XXH3 %u %016" PRIX64 "\n", n, uint64_t(XXH3_64bits(b.data(), n)));
  }

  Check(AbPhaseBit(AbPhase::kSkinnedDepthAlways) == 512, "skinned depth always is bit 512");
  Check(std::string_view(AbPhaseName(AbPhase::kSkinnedDepthAlways)) == "skinned depth always", "its name");
  AbSchedule ab(1500, 513);
  AbWindow w;
  w.seconds = 10;
  w.frames = 40;
  w.draws = 4000 * 40;
  ab.Step(w);
  Check(ab.Step(w) && ab.phase() == AbPhase::kBaseline, "two gameplay windows start the baseline");
  bool ok = true;
  const AbPhase next[] = {AbPhase::kSkinnedDepthAlways, AbPhase::kBaseline, AbPhase::kSkinnedDepthAlways};
  for (AbPhase want : next) {
    ab.Step(w);
    ab.Step(w);
    ok = ok && ab.Step(w) && ab.phase() == want;
  }
  Check(ok, "mask 513 alternates baseline and skinned depth always");
  Check(ab.cycle() == 1, "one cycle after the first depth-always phase");
  return failures ? 1 : 0;
}
