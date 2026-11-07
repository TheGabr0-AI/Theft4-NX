// Probe of run 44 (devlog section O): dk_depth_handoff.h, the scene depth hand-off. Prints lines the Python test checks.
// Host clang, no Switch headers.
//   dk_run44_probe plan          the verdicts of PlanDepthHandoff
//   dk_run44_probe pixel         reads "depth_bits float24" lines, prints what the pass leaves in a pixel
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "dk_depth_handoff.h"

using namespace rex::graphics::gta4_native;
using namespace rex::graphics::gta4_native::dk;

static HandoffInputs Good() {
  HandoffInputs in;
  in.policy = ForwardStencilHandoffPolicy::kRebuildSceneCoverage;
  in.pass_available = true;
  in.source_surface = in.destination_surface = true;
  in.same_image = false;
  in.source_written = true;
  in.source_width = in.destination_width = 1280;
  in.source_height = in.destination_height = 720;
  in.source_guest_format = kFetchFormat24_8Float;
  return in;
}

static void Plan(const char* name, const HandoffInputs& in) {
  std::printf("PLAN %s %u\n", name, unsigned(PlanDepthHandoff(in)));
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "plan";
  if (std::strcmp(mode, "plan") == 0) {
    Plan("good", Good());
    HandoffInputs in = Good();
    in.source_guest_format = kFetchFormat24_8;
    Plan("good_format_22", in);
    in = Good();
    in.policy = ForwardStencilHandoffPolicy::kPreserve;
    Plan("preserve", in);
    in = Good();
    in.pass_available = false;
    Plan("no_pass", in);
    in = Good();
    in.source_surface = false;
    Plan("no_source_surface", in);
    in = Good();
    in.destination_surface = false;
    Plan("no_destination_surface", in);
    in = Good();
    in.same_image = true;
    Plan("same_image", in);
    in = Good();
    in.source_written = false;
    Plan("source_unwritten", in);
    in = Good();
    in.destination_width = 640;
    Plan("width_differs", in);
    in = Good();
    in.destination_height = 360;
    Plan("height_differs", in);
    in = Good();
    in.source_guest_format = 6;
    Plan("colour_format", in);
    // the order of the rejections when several hold: the policy, the pass, the surfaces, the same image, the unwritten
    // source, the extent, the encoding
    in = Good();
    in.source_written = false;
    in.destination_width = 640;
    in.source_guest_format = 6;
    Plan("unwritten_and_extent_and_format", in);
    in = Good();
    in.destination_width = 640;
    in.source_guest_format = 6;
    Plan("extent_and_format", in);
    in = Good();
    in.same_image = true;
    in.source_written = false;
    Plan("same_and_unwritten", in);
    std::printf("CONSTANTS empty %u covered %u\n", kForwardEmptySceneStencil, kForwardCoveredSceneStencil);
    return 0;
  }
  if (std::strcmp(mode, "pixel") == 0) {
    unsigned bits, flt;
    while (std::scanf("%u %u", &bits, &flt) == 2) {
      const float depth = std::bit_cast<float>(bits);
      // rounded_24: the round-to-even of clamp(depth) * 16777215 in single precision, as the shader computes it
      float c = depth;
      if (!(c > 0.0f)) c = 0.0f;
      if (c > 1.0f) c = 1.0f;
      const uint32_t rounded = uint32_t(std::nearbyintf(c * 16777215.0f));
      const HandoffPixel p = SceneHandoffPixel(flt != 0, depth, bits, rounded);
      std::printf("PIXEL %u %u %u %u\n", bits, flt, std::bit_cast<uint32_t>(p.depth), p.stencil);
    }
    return 0;
  }
  return 1;
}
