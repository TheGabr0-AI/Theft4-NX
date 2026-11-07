// Host probe for tests/deko3d/test_resolve_clear.py (run 41).
//   probe resolve          the resolve-clear plan for the flags the title sends and the edge cases
//   probe vertex <seed>    the vertex fix-ups: in place in a plain buffer (the run 39 algorithm, copied verbatim from
//                          commit 17f3a18's Draw(): the text between the REFERENCE markers) against the run 40 path
//                          (cached scratch, ApplyVertexFixes, one copy); prints the number of cases and of differences
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <vector>

#include "dk_vertex_convert.h"
#include "native_resolve_clear.h"

using namespace rex::graphics::gta4_native;
using rex::graphics::gta4_native::dk::ApplyVertexFixes;
using rex::graphics::gta4_native::dk::VertexElementBytes;

static int Resolve() {
  struct Case {
    const char* name;
    uint32_t flags;
    bool params;
    bool src_color, src_depth, device_depth, unrep;
  };
  const Case cases[] = {
      // The title's own resolves at the end of the deferred phase (gta4_native_hooks.cpp, SubmitNativeResolve).
      {"depth_resolve_20", 20, false, false, false, false, false},
      {"colour_resolve_16", 16, false, false, false, false, false},
      {"alias_resolve_768_clear_colour_and_depth", 768, false, true, false, true, false},
      {"second_output_273_clear_colour", 273, false, true, false, false, false},
      {"third_output_274_clear_colour", 274, false, true, false, false, false},
      // Edge cases.
      {"depth_clear_only_on_colour_resolve", 0x200, false, false, false, true, false},
      {"colour_clear_only", 0x100, false, true, false, false, false},
      {"depth_resolve_with_depth_clear", 0x204, false, false, true, false, false},
      {"depth_resolve_with_colour_clear", 0x104, false, false, false, false, true},
      {"override_refuses_the_device_depth_clear", 768, true, true, false, false, false},
      {"exponent_bits_do_not_matter", 768 | 0x04000000, false, true, false, true, false},
      {"no_clear_bits", 0, false, false, false, false, false},
  };
  int bad = 0;
  for (const Case& c : cases) {
    const ResolveClearPlan p = PlanResolveClears(c.flags, c.params);
    const bool ok = p.clear_source_color == c.src_color && p.clear_source_depth == c.src_depth &&
                    p.clear_device_depth == c.device_depth && p.unrepresentable_color == c.unrep;
    std::printf("PLAN %s %d%d%d%d %s\n", c.name, p.clear_source_color, p.clear_source_depth, p.clear_device_depth,
                p.unrepresentable_color, ok ? "ok" : "WRONG");
    bad += !ok;
  }
  return bad ? 1 : 0;
}

// BEGIN REFERENCE (commit 17f3a18, dk_recorder.cpp Draw(): the fix-ups of one stream, in place on `target`)
static void ReferenceFixes(uint8_t* target, uint64_t vertex_count, uint32_t stride,
                           const std::vector<std::pair<uint32_t, uint32_t>>& fixes) {
  struct { uint32_t stride; const std::vector<std::pair<uint32_t, uint32_t>>& fixes; } s{stride, fixes};
  auto ElementBytes = [](uint32_t type) -> uint32_t {
    switch (type) {
      case 0x2C23A5: return 8;
      case 0x2A23B9: return 12;
      case 0x1A23A6: return 16;
      case 0x1A235A:
      case 0x1A215A:
      case 0x1A205A:
      case 0x1A2360: return 8;
      default: return 4;
    }
  };
  for (const auto& [offset, fix] : s.fixes) {
    const uint32_t kind = fix & 0xFF;
    const uint32_t type = fix >> 8;
    for (uint64_t v = 0; v < vertex_count; ++v) {
      uint8_t* p = target + v * s.stride + offset;
      if (offset + ElementBytes(type) > s.stride) {
        break;
      }
      if (kind == 1) {
        // 16-bit components: after the 32-bit swap each word holds them in the wrong order.
        for (uint32_t w = 0; w < ElementBytes(type) / 4; ++w) {
          uint32_t x;
          std::memcpy(&x, p + w * 4, 4);
          x = std::rotl(x, 16);
          std::memcpy(p + w * 4, &x, 4);
        }
      } else if (kind == 2) {
        uint32_t x;
        std::memcpy(&x, p, 4);
        x = (x & 0x3FFFFFFF) | 0x40000000;
        std::memcpy(p, &x, 4);
      } else if (kind == 3) {
        std::swap(p[0], p[2]);
      }
    }
  }
}
// END REFERENCE

static uint32_t g_state;
static uint32_t Next() {  // xorshift32, deterministic
  g_state ^= g_state << 13;
  g_state ^= g_state >> 17;
  g_state ^= g_state << 5;
  return g_state;
}

static int Vertex(uint32_t seed) {
  g_state = seed ? seed : 1;
  const uint32_t types[] = {0x2C23A5, 0x2A23B9, 0x1A23A6, 0x1A235A, 0x1A215A, 0x1A205A, 0x1A2360, 0x2C235F,
                            0x182886, 0x1A2187, 0x2C2359, 0x2C2159, 0x2C2059, 0x1234};
  const uint32_t kinds[] = {1, 2, 3};
  int cases = 0, differences = 0;
  std::vector<uint8_t> scratch, ring;
  for (int round = 0; round < 3000; ++round) {
    const uint32_t stride = 4 * (1 + Next() % 16);       // 4..64 bytes, word multiples as the title's streams are
    const uint32_t count = 1 + Next() % 200;
    std::vector<std::pair<uint32_t, uint32_t>> fixes;
    const uint32_t n = Next() % 5;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t offset = 4 * (Next() % 18);          // sometimes past the stride: the break path
      fixes.emplace_back(offset, kinds[Next() % 3] | (types[Next() % 14] << 8));
    }
    std::vector<uint8_t> input(size_t(stride) * count);
    for (auto& b : input) {
      b = uint8_t(Next());
    }
    // What the ring holds after the run 39 path: the words copied (here: as they are) and fixed in place.
    std::vector<uint8_t> expected = input;
    ReferenceFixes(expected.data(), count, stride, fixes);
    // The run 40 path: into the cached scratch, fixed there, one copy into the ring.
    if (scratch.size() < input.size()) {
      scratch.resize(input.size());
    }
    std::memcpy(scratch.data(), input.data(), input.size());
    ApplyVertexFixes(scratch.data(), count, stride, fixes);
    ring.assign(input.size(), 0xCD);
    std::memcpy(ring.data(), scratch.data(), input.size());
    ++cases;
    if (std::memcmp(ring.data(), expected.data(), input.size()) != 0) {
      ++differences;
    }
    // The type table the two paths use.
    for (uint32_t t : types) {
      if (VertexElementBytes(t) != (t == 0x2C23A5 ? 8u : t == 0x2A23B9 ? 12u : t == 0x1A23A6 ? 16u
                                    : (t == 0x1A235A || t == 0x1A215A || t == 0x1A205A || t == 0x1A2360) ? 8u : 4u)) {
        ++differences;
      }
    }
  }
  std::printf("VERTEX cases %d differences %d\n", cases, differences);
  return differences ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "resolve")) {
    return Resolve();
  }
  if (argc >= 2 && !std::strcmp(argv[1], "vertex")) {
    return Vertex(argc >= 3 ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 1);
  }
  std::fprintf(stderr, "usage: probe resolve | probe vertex <seed>\n");
  return 2;
}
