// Host probe of dk_depth_alias.h for tests/deko3d/test_depth_alias.py.
//   probe decide            the fetch decision on synthetic cases ("DECIDE name result")
//   probe pack < lines      each line "depth_bits stencil float(0|1)": prints "PACK depth_bits stencil float packed"
// The pixel shader's own arithmetic (round to even of clamp(depth) * 16777215) is done here in single precision, as the
// GPU does it.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dk_depth_alias.h"
#include "native_alias_registration.h"

using namespace rex::graphics::gta4_native::dk;

// The registrations the front end passes on: the title's real command struct, the same predicate.
static int Register() {
  using namespace rex::graphics::gta4_native;
  struct Case {
    const char* name;
    uint32_t resource, source;
    VirtualResourceKind kind;
    bool want;
  };
  const Case cases[] = {
      {"alias_of_depth", 0x40001000, 0x40002000, VirtualResourceKind::kTexture, true},
      {"no_source", 0x40001000, 0, VirtualResourceKind::kTexture, false},
      {"source_is_itself", 0x40001000, 0x40001000, VirtualResourceKind::kTexture, false},
      {"no_resource", 0, 0x40002000, VirtualResourceKind::kTexture, false},
      {"surface_kind", 0x40001000, 0x40002000, VirtualResourceKind::kSurface, false},
  };
  int bad = 0;
  for (const Case& c : cases) {
    RegisterVirtualResourceCommand command{};
    command.resource = c.resource;
    command.packed_depth_source = c.source;
    command.kind = c.kind;
    const bool got = IsPackedDepthAliasRegistration(command);
    std::printf("REGISTER %s %d\n", c.name, got ? 1 : 0);
    bad += got != c.want;
  }
  // A registration that carries no source (the title's other virtual resources) is the default of the struct.
  std::printf("REGISTER default_command %d\n", IsPackedDepthAliasRegistration(RegisterVirtualResourceCommand{}) ? 1 : 0);
  return bad ? 1 : 0;
}

static int Decide() {
  struct Case {
    const char* name;
    bool depth;
    uint32_t format, fw, fh, iw, ih;
    bool want;
  };
  const Case cases[] = {
      {"colour_fetch_of_depth", true, 6, 1280, 720, 1280, 720, true},
      {"as16_variant", true, 50, 1280, 720, 1280, 720, true},
      {"depth_fetch_of_depth", true, 23, 1280, 720, 1280, 720, false},
      {"depth24_fetch_of_depth", true, 22, 1280, 720, 1280, 720, false},
      {"colour_fetch_of_colour", false, 6, 1280, 720, 1280, 720, false},
      {"other_colour_format", true, 36, 1280, 720, 1280, 720, false},
      {"width_differs", true, 6, 640, 720, 1280, 720, false},
      {"height_differs", true, 6, 1280, 360, 1280, 720, false},
      {"zero_size", true, 6, 0, 0, 0, 0, false},
  };
  int bad = 0;
  for (const Case& c : cases) {
    const bool got = IsPackedDepthFetch(c.depth, c.format, c.fw, c.fh, c.iw, c.ih);
    std::printf("DECIDE %s %d\n", c.name, got ? 1 : 0);
    bad += got != c.want;
  }
  std::printf("DECIDE_DEPTHFORMATS %d %d %d %d\n", IsDepthFetchFormat(22), IsDepthFetchFormat(23), IsDepthFetchFormat(6),
              IsDepthFetchFormat(0));
  return bad ? 1 : 0;
}

static int Pack() {
  unsigned bits, stencil, flt;
  while (std::scanf("%u %u %u", &bits, &stencil, &flt) == 3) {
    const float depth = std::bit_cast<float>(bits);
    const float scaled = std::fmin(std::fmax(depth, 0.0f), 1.0f) * 16777215.0f;
    const uint32_t rounded = uint32_t(std::nearbyint(scaled));  // round to nearest even (the default mode)
    const uint32_t packed = PackDepthStencil(flt != 0, depth, bits, rounded, stencil);
    std::printf("PACK %u %u %u %u\n", bits, stencil, flt, packed);
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "decide")) {
    return Decide();
  }
  if (argc >= 2 && !std::strcmp(argv[1], "register")) {
    return Register();
  }
  if (argc >= 2 && !std::strcmp(argv[1], "pack")) {
    return Pack();
  }
  std::fprintf(stderr, "usage: probe decide | probe pack\n");
  return 2;
}
