// Run 57's offline probe (tests/deko3d/test_run57.py): dk_placement.h's plan and the conversion pass's arithmetic.
//   dk_run57_probe placement -> "PLACEMENT ... errors N"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dk_placement.h"

using namespace rex::graphics;
using namespace rex::graphics::gta4_native;
using namespace rex::graphics::gta4_native::dk;

static SurfaceDescriptor Surface(uint32_t handle, uint32_t width, uint32_t height, uint32_t samples, uint32_t pitch,
                                 uint32_t base_tiles) {
  SurfaceDescriptor d{};
  d.handle = handle;
  d.width = width;
  d.height = height;
  d.base = (samples << 16) | pitch;  // surface info: pitch in the low bits, sample type at bit 16
  d.address = base_tiles;
  return d;
}

// The conversion pass as kPlacementPs computes it, on the CPU (one channel).
static std::vector<float> Convert(const std::vector<float>& src, uint32_t sw, const PlacementCopy& p, uint32_t dw,
                                  uint32_t dh) {
  std::vector<float> out(size_t(dw) * dh);
  for (uint32_t y = 0; y < dh; ++y) {
    for (uint32_t x = 0; x < dw; ++x) {
      // as the shader: a shift by PlacementShift(expand), then the reduce factor
      const uint32_t fx = (x >> PlacementShift(p.expand_x)) * p.reduce_x, fy = (y >> PlacementShift(p.expand_y)) * p.reduce_y;
      float sum = 0;
      for (uint32_t j = 0; j < p.reduce_y; ++j) {
        for (uint32_t i = 0; i < p.reduce_x; ++i) {
          sum += src[size_t(fy + j) * sw + fx + i];
        }
      }
      out[size_t(y) * dw + x] = sum / float(p.reduce_x * p.reduce_y);
    }
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 2 || std::strcmp(argv[1], "placement") != 0) return 2;
  int errors = 0;
  auto view = [](const SurfaceDescriptor& d, bool depth = false) {
    GuestSurfaceView v{};
    if (!DecodeGuestSurfaceView(d, depth, v)) std::printf("  decode failed for %08X\n", d.handle);
    return v;
  };
  // GTA IV's exposure chain: the HDR scene 1280x720 1X and a 640x360 4X view of the same EDRAM.
  const SurfaceDescriptor hdr = Surface(0x40076EB0, 1280, 720, 0, 1280, 0x200);
  const SurfaceDescriptor quarter4x = Surface(0x402268C0, 640, 360, 2, 640, 0x200);
  const SurfaceDescriptor half2x = Surface(0x11111111, 1280, 360, 1, 1280, 0x200);
  const SurfaceDescriptor other_base = Surface(0x22222222, 640, 360, 2, 640, 0x300);
  const SurfaceDescriptor same = Surface(0x33333333, 1280, 720, 0, 1280, 0x200);
  struct Case {
    const char* name;
    SurfaceDescriptor owner, dest;
    bool valid, same_layout;
    uint32_t rx, ry, ex, ey;
  } cases[] = {
      {"1X 1280x720 -> 4X 640x360 (2x2 average)", hdr, quarter4x, true, false, 2, 2, 1, 1},
      {"1X 1280x720 -> 2X 1280x360 (1x2 average)", hdr, half2x, true, false, 1, 2, 1, 1},
      {"4X 640x360 -> 1X 1280x720 (2x2 copies)", quarter4x, hdr, true, false, 1, 1, 2, 2},
      {"2X 1280x360 -> 4X 640x360", half2x, quarter4x, true, false, 2, 1, 1, 1},
      {"identical views (copy)", hdr, same, true, true, 1, 1, 1, 1},
      {"another EDRAM base (refused)", hdr, other_base, false, false, 1, 1, 1, 1},
  };
  for (const Case& c : cases) {
    const PlacementCopy p =
        PlanPlacementCopy(view(c.owner), c.owner.width, c.owner.height, view(c.dest), c.dest.width, c.dest.height);
    const bool ok = p.valid == c.valid &&
                    (!c.valid || (p.same_layout == c.same_layout && p.reduce_x == c.rx && p.reduce_y == c.ry &&
                                  p.expand_x == c.ex && p.expand_y == c.ey));
    errors += !ok;
    std::printf("PLACEMENT plan %-45s valid %d same %d reduce %ux%u expand %ux%u: %s\n", c.name, p.valid, p.same_layout,
                p.reduce_x, p.reduce_y, p.expand_x, p.expand_y, ok ? "ok" : "WRONG");
  }
  // Depth views are not materialized (the Vulkan path refuses them too).
  {
    const PlacementCopy p = PlanPlacementCopy(view(hdr, true), 1280, 720, view(same, true), 1280, 720);
    errors += p.valid;
    std::printf("PLACEMENT plan depth views: valid %d (expected 0)\n", p.valid);
  }
  // An image size that does not fill the sample space: refused.
  {
    const PlacementCopy p = PlanPlacementCopy(view(hdr), 1280, 720, view(quarter4x), 600, 360);
    errors += p.valid;
    std::printf("PLACEMENT plan wrong destination size: valid %d (expected 0)\n", p.valid);
  }
  // The pass's arithmetic: 1280x720 ramp -> 640x360 is the 2x2 box average; back to 1280x720 replicates it.
  {
    std::vector<float> src(1280 * 720);
    for (uint32_t y = 0; y < 720; ++y)
      for (uint32_t x = 0; x < 1280; ++x) src[y * 1280 + x] = float((x * 7 + y * 13) % 97) / 8.0f;
    const PlacementCopy down = PlanPlacementCopy(view(hdr), 1280, 720, view(quarter4x), 640, 360);
    const std::vector<float> small = Convert(src, 1280, down, 640, 360);
    double worst = 0;
    for (uint32_t y = 0; y < 360; ++y)
      for (uint32_t x = 0; x < 640; ++x) {
        const float want = (src[(2 * y) * 1280 + 2 * x] + src[(2 * y) * 1280 + 2 * x + 1] +
                            src[(2 * y + 1) * 1280 + 2 * x] + src[(2 * y + 1) * 1280 + 2 * x + 1]) / 4.0f;
        worst = std::max(worst, double(std::fabs(small[y * 640 + x] - want)));
      }
    const PlacementCopy up = PlanPlacementCopy(view(quarter4x), 640, 360, view(hdr), 1280, 720);
    const std::vector<float> big = Convert(small, 640, up, 1280, 720);
    double worst_up = 0;
    for (uint32_t y = 0; y < 720; ++y)
      for (uint32_t x = 0; x < 1280; ++x)
        worst_up = std::max(worst_up, double(std::fabs(big[y * 1280 + x] - small[(y / 2) * 640 + x / 2])));
    errors += worst > 1e-5 || worst_up > 0;
    std::printf("PLACEMENT pass arithmetic: 2x2 average worst error %.2g, replication worst error %.2g\n", worst, worst_up);
  }
  std::printf("PLACEMENT errors %d\n", errors);
  return errors ? 1 : 0;
}
