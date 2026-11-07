// Probe of run 48's dk_blend_alpha.h (tests/deko3d/test_blend_alpha.py).
//
// The six IndependentBlend registers of a target (methods 0x781 + 8 * target .. +5) as three writers leave them:
//   library: what deko3d 0.5.0's dkCmdBufBindBlendStates writes, transcribed from its disassembly (the test checks the
//            disassembly reads no bit of dstAlphaBlendFactor);
//   plume:   banjo-nx's plume deko3d back end (thirdparty/plume-deko3d/plume_deko3d.cpp blendWords), the six registers
//            written right;
//   ours:    the library, then the two words of DkBlendAlphaDstWords executed on the register file.
// For every target 0-7 and every pair of colour/alpha destination factors (and the other fields varied), ours must equal
// plume, and the library must differ from plume exactly when the two destination factors differ.
#include <cstdint>
#include <cstdio>
#include <map>

#include "dk_blend_alpha.h"

using namespace rex::graphics::gta4_native::dk;

namespace {

const uint32_t kFactors[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 18, 19, 0x21, 0x22, 0x23, 0x24};

struct State {
  uint32_t color_op, src_color, dst_color, alpha_op, src_alpha, dst_alpha;
};

uint32_t Pack(const State& s) {  // DkBlendState's bitfields (deko3d.h)
  return s.color_op | (s.src_color << 3) | (s.dst_color << 9) | (s.alpha_op << 15) | (s.src_alpha << 18) |
         (s.dst_alpha << 24);
}

// The disassembly of dkCmdBufBindBlendStates (libdeko3d.a and libdeko3dd.a): header, then w8 = bits 0-2, srcColor from
// bits 3-8, dstColor from bits 9-14, alphaOp bits 15-17, srcAlpha bits 18-23, and dstColor again as the last word.
void Library(uint32_t target, uint32_t packed, uint32_t w[7]) {
  auto enc = [](uint32_t six) { return six > 0x1F ? ((six & 0x1F) | 0xC000) : (six & 0x1F); };
  w[0] = 0x20060000u | ((0x781 + 8 * target) & 0xFFFF & 0x1FFF);
  w[1] = packed & 0x7;
  w[2] = enc((packed >> 3) & 0x3F);
  w[3] = enc((packed >> 9) & 0x3F);
  w[4] = (packed >> 15) & 0x7;
  w[5] = enc((packed >> 18) & 0x3F);
  w[6] = enc((packed >> 9) & 0x3F);
}

// plume_deko3d.cpp blendWords / blendFactorWord, verbatim but for the types.
uint32_t PlumeFactor(uint32_t f) { return (f & 0x1Fu) | ((f & 0x20u) ? 0xC000u : 0u); }
void Plume(uint32_t target, const State& s, uint32_t w[7]) {
  w[0] = (1u << 29) | (6u << 16) | (0u << 13) | (0x781u + 8u * target);
  w[1] = s.color_op;
  w[2] = PlumeFactor(s.src_color);
  w[3] = PlumeFactor(s.dst_color);
  w[4] = s.alpha_op;
  w[5] = PlumeFactor(s.src_alpha);
  w[6] = PlumeFactor(s.dst_alpha);
}

// A minimal Maxwell pushbuffer executor: increasing-mode headers (secop 1) on subchannel 0 only.
bool Execute(const uint32_t* w, uint32_t n, std::map<uint32_t, uint32_t>& regs) {
  uint32_t i = 0;
  while (i < n) {
    const uint32_t h = w[i++];
    if ((h >> 29) != 1 || ((h >> 13) & 7) != 0) return false;
    const uint32_t count = (h >> 16) & 0x1FFF, method = h & 0x1FFF;
    for (uint32_t k = 0; k < count; ++k) {
      if (i >= n) return false;
      regs[method + k] = w[i++];
    }
  }
  return true;
}

}  // namespace

int main() {
  uint64_t cases = 0, ours_wrong = 0, library_wrong = 0, library_wrong_expected = 0, malformed = 0;
  for (uint32_t target = 0; target < 8; ++target) {
    for (uint32_t dc : kFactors) {
      for (uint32_t da : kFactors) {
        for (uint32_t v = 0; v < 4; ++v) {
          const State s{1 + v % 5, kFactors[(v * 7 + dc) % 19], dc, 1 + (v + 2) % 5, kFactors[(v * 5 + da) % 19], da};
          uint32_t lib[7], plume[7], fix[2];
          Library(target, Pack(s), lib);
          Plume(target, s, plume);
          DkBlendAlphaDstWords(target, s.dst_alpha, fix);
          std::map<uint32_t, uint32_t> r_lib, r_plume, r_ours;
          bool ok = Execute(lib, 7, r_lib) && Execute(plume, 7, r_plume);
          r_ours = r_lib;
          ok = ok && Execute(fix, 2, r_ours);
          malformed += !ok;
          ++cases;
          ours_wrong += r_ours != r_plume;
          library_wrong += r_lib != r_plume;
          library_wrong_expected += dc != da;
        }
      }
    }
  }
  std::printf("BLEND cases %llu ours_wrong %llu library_wrong %llu expected %llu malformed %llu\n",
              (unsigned long long)cases, (unsigned long long)ours_wrong, (unsigned long long)library_wrong,
              (unsigned long long)library_wrong_expected, (unsigned long long)malformed);
  uint32_t w[2];
  DkBlendAlphaDstWords(3, 0x23, w);
  std::printf("WORDS target 3 ConstAlpha %08X %08X\n", w[0], w[1]);
  return 0;
}
