// Probe of run 42 (devlog section M): the channels of a sampled view (dk_view_swizzle.h). Prints lines the Python test
// checks. Compiled on the PC with the host clang of Visual Studio; no Switch headers.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "dk_view_swizzle.h"

using namespace rex::graphics::gta4_native::dk;

// BEGIN REFERENCE (ViewSwizzle of commit 17f3a18, with DkImageSwizzle_Red + n written as the channel number n and Zero /
// One as 4 / 5, the codes of dk_view_swizzle.h)
static void ReferenceViewSwizzle(uint32_t guest, uint16_t host, uint8_t out[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t v = (guest >> (i * 3)) & 0x7;
    if (v <= 3) {
      out[i] = uint8_t((host >> (v * 3)) & 0x7);
    } else if (v == 4) {
      out[i] = 4;
    } else {
      out[i] = 5;
    }
  }
}
// END REFERENCE

static constexpr uint16_t kRGBA = (1 << 3) | (2 << 6) | (3 << 9);
static constexpr uint16_t kRRRR = 0;
static constexpr uint16_t kRGGG = (1 << 3) | (1 << 6) | (1 << 9);

static std::array<uint8_t, 4> Apply(const uint8_t code[4], const std::array<uint8_t, 4>& lanes) {
  std::array<uint8_t, 4> out{};
  for (int i = 0; i < 4; ++i) {
    out[i] = code[i] == kViewZero ? 0 : code[i] == kViewOne ? 255 : lanes[code[i]];
  }
  return out;
}

int main() {
  // 1. The composition is the one of commit 17f3a18 for every fetch swizzle and the host tables the recorder uses.
  uint32_t differences = 0, cases = 0;
  for (uint32_t guest = 0; guest < 4096; ++guest) {
    for (const uint16_t host : {kRGBA, kRRRR, kRGGG, uint16_t(0x60A)}) {
      uint8_t a[4], b[4];
      ComposeViewSwizzle(guest, host, a);
      ReferenceViewSwizzle(guest, host, b);
      differences += std::memcmp(a, b, 4) != 0;
      ++cases;
    }
  }
  std::printf("COMPOSE cases %u differences %u\n", cases, differences);

  // 2. The cases of the logs: DXT 0x688 and A8R8G8B8 0x60A of a texture read from guest memory (bytes B, G, R, A after the
  // 4-byte swap, an RGBA8 image holding them): the view must give R, G, B, A.
  {
    const std::array<uint8_t, 4> stored = {30, 120, 200, 255};  // the image's lanes, i.e. B, G, R, A of the texel
    uint8_t code[4];
    ComposeViewSwizzle(0x60A, kRGBA, code);
    const auto seen = Apply(code, stored);
    std::printf("GUESTMEM 0x60A: seen %u %u %u %u %s\n", seen[0], seen[1], seen[2], seen[3],
                (seen[0] == 200 && seen[1] == 120 && seen[2] == 30 && seen[3] == 255) ? "ok" : "BAD");
    ComposeViewSwizzle(0x688, kRGBA, code);
    const auto dxt = Apply(code, stored);
    std::printf("DXT 0x688: seen %u %u %u %u %s\n", dxt[0], dxt[1], dxt[2], dxt[3],
                (dxt == stored) ? "ok" : "BAD");
  }

  // 3. A resolved colour image: its lanes are the render target's R, G, B, A. On the console the resolve wrote the bytes in
  // memory order (B, G, R, A) and the title's fetch (0x60A) put them back; the view of the host image must give the same
  // R, G, B, A for the swizzle the title sends and for any other (it is not applied).
  uint32_t resolved_bad = 0, resolved_cases = 0;
  std::mt19937 rng(42);
  for (uint32_t n = 0; n < 2000; ++n) {
    const std::array<uint8_t, 4> rt = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};  // R, G, B, A
    // The console: memory order, then the title's swizzle.
    const std::array<uint8_t, 4> memory = {rt[2], rt[1], rt[0], rt[3]};
    uint8_t console_code[4];
    ComposeViewSwizzle(0x60A, kRGBA, console_code);
    const auto console_seen = Apply(console_code, memory);
    // The host: the image holds rt; the view is built with the guest swizzle the recorder now passes.
    for (const uint32_t fetch : {0x60Au, 0x688u, 0x000u, 0xFFFu}) {
      uint8_t host_code[4];
      ComposeViewSwizzle(ResolvedColourGuestSwizzle(fetch), kRGBA, host_code);
      const auto host_seen = Apply(host_code, rt);
      if (fetch == 0x60A) {
        resolved_bad += host_seen != console_seen;  // the title's A8R8G8B8 fetch: what the console shows
        resolved_bad += host_seen != rt;
      } else {
        // any other swizzle is the title's own and is applied as before
        uint8_t old_code[4];
        ComposeViewSwizzle(fetch, kRGBA, old_code);
        resolved_bad += host_seen != Apply(old_code, rt);
        resolved_bad += ResolvedColourGuestSwizzle(fetch) != fetch;
      }
      ++resolved_cases;
    }
  }
  std::printf("RESOLVED cases %u bad %u\n", resolved_cases, resolved_bad);

  // 4. The old behaviour on a resolved 8888 with 0x60A was the exchange of red and blue (what run 41 showed).
  {
    const std::array<uint8_t, 4> rt = {200, 120, 30, 255};
    uint8_t code[4];
    ComposeViewSwizzle(0x60A, kRGBA, code);
    const auto old_seen = Apply(code, rt);
    std::printf("OLD resolved 0x60A: seen %u %u %u %u %s\n", old_seen[0], old_seen[1], old_seen[2], old_seen[3],
                (old_seen[0] == 30 && old_seen[2] == 200) ? "swapped" : "BAD");
  }

  // 5. The host tables of single-channel formats still replicate: an R8 texture with 0x688 reads R, R, R, R.
  {
    uint8_t code[4];
    ComposeViewSwizzle(0x688, kRRRR, code);
    std::printf("R8 0x688: %u %u %u %u %s\n", code[0], code[1], code[2], code[3],
                (code[0] == 0 && code[1] == 0 && code[2] == 0 && code[3] == 0) ? "ok" : "BAD");
  }
  return 0;
}
