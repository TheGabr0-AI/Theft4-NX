// Offline probe of run 52 (docs/switch-port/16-deko3d-devlog.md section Z), compiled by tests/deko3d/test_run52.py.
//
// STORE: the geometry store (deko3d/dk_geometry_store.cpp, nfsmw-nx's) over a simulated recorder: 64 guest vertex
// buffers, some static, some rewritten now and then, some rewritten every frame; 3 work slots, a GPU that finishes a
// work two works later. For every draw:
//   - a range bound from the store must hold exactly the host form (byte-swapped words) of the guest bytes now;
//   - a block bound by a work must not change until that work is done (checked when the work finishes);
// and the store must hit on the static data (the point of it).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <random>
#include <vector>

#include "dk_geometry_store.h"
#include "dk_rect_list.h"
#include <cmath>
#include <span>
#include <utility>

using rex::graphics::gta4_native::dk::ExpandRectangle;
using rex::graphics::gta4_native::dk::GeometryStore;

namespace {

void SwapWords(uint8_t* out, const uint8_t* in, uint32_t bytes) {
  for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
    out[i] = in[i + 3];
    out[i + 1] = in[i + 2];
    out[i + 2] = in[i + 1];
    out[i + 3] = in[i];
  }
}

// RECT: dk_rect_list.h. Random rectangles (rotated, scaled), three of their corners given in a random order as Xenos
// does; vertex = position float2 @0, colour u32 @8 (a different value per vertex), uv float2 @12. The quad must have
// the four true corners in perimeter order, uv affine with the position, and the colours of real vertices, the fourth
// taking c2's; the old rule (v0 is always the corner, every finite word extrapolated) is counted for comparison.
int RectTest() {
  std::mt19937 rng(5252);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  constexpr uint32_t kStride = 20;
  const std::pair<uint32_t, uint32_t> floats[] = {{0u, 2u}, {12u, 2u}};
  int rounds = 0, errors = 0, old_wrong = 0;
  for (int round = 0; round < 20000; ++round) {
    const float cx = u(rng), cy = u(rng), a = u(rng) * 3.14159f, w = 0.01f + std::fabs(u(rng)), h = 0.01f + std::fabs(u(rng));
    const float ex[2] = {std::cos(a) * w, std::sin(a) * w}, ey[2] = {-std::sin(a) * h, std::cos(a) * h};
    float corners[4][2], uvs[4][2];
    for (int k = 0; k < 4; ++k) {
      const float sx = (k == 1 || k == 2) ? 1.0f : 0.0f, sy = (k >= 2) ? 1.0f : 0.0f;  // perimeter order 0 1 2 3
      corners[k][0] = cx + sx * ex[0] + sy * ey[0];
      corners[k][1] = cy + sx * ex[1] + sy * ey[1];
      uvs[k][0] = sx;
      uvs[k][1] = sy;
    }
    // Xenos gives three corners: a right-angle corner and its two neighbours, in some order.
    const int right = int(rng() % 4);
    int given[3] = {right, (right + 1) % 4, (right + 3) % 4};
    std::shuffle(given, given + 3, rng);
    uint8_t v[3][kStride];
    for (int k = 0; k < 3; ++k) {
      std::memcpy(v[k], corners[given[k]], 8);
      const uint32_t colour = 0x11223300u + uint32_t(given[k]);
      std::memcpy(v[k] + 8, &colour, 4);
      std::memcpy(v[k] + 12, uvs[given[k]], 8);
    }
    uint8_t out[4 * kStride], fourth[kStride];
    ExpandRectangle(v[0], v[1], v[2], kStride, 0, std::span<const std::pair<uint32_t, uint32_t>>(floats), fourth, out);
    bool ok = true;
    int seen[4] = {-1, -1, -1, -1};
    for (int q = 0; q < 4 && ok; ++q) {
      float pq[2], uvq[2];
      std::memcpy(pq, out + q * kStride, 8);
      std::memcpy(uvq, out + q * kStride + 12, 8);
      int match = -1;
      for (int k = 0; k < 4; ++k) {
        if (std::fabs(pq[0] - corners[k][0]) < 1e-4f && std::fabs(pq[1] - corners[k][1]) < 1e-4f) match = k;
      }
      if (match < 0 || std::fabs(uvq[0] - uvs[match][0]) > 1e-4f || std::fabs(uvq[1] - uvs[match][1]) > 1e-4f) {
        ok = false;
        break;
      }
      seen[q] = match;
      uint32_t colour;
      std::memcpy(&colour, out + q * kStride + 8, 4);
      if (q != 2 && colour != 0x11223300u + uint32_t(match)) ok = false;
    }
    // Perimeter order: consecutive output vertices are neighbours on the rectangle (differ by 1 mod 4).
    for (int q = 0; q < 4 && ok; ++q) {
      const int d = (seen[q] - seen[(q + 1) % 4] + 4) % 4;
      if (d != 1 && d != 3) ok = false;
    }
    errors += !ok;
    old_wrong += given[0] != right;  // the old rule took v0 as the corner
    ++rounds;
  }
  std::printf("RECT rounds %d errors %d old_rule_wrong %d\n", rounds, errors, old_wrong);
  return errors;
}

}  // namespace

int main() {
  std::mt19937 rng(52);
  constexpr uint32_t kSlots = 3;
  std::vector<uint8_t> store_memory(8u << 20);
  constexpr uint64_t kGpuBase = 0x100000000ull;
  uint64_t done_up_to = 0;  // the simulated GPU
  GeometryStore store;
  GeometryStore::Options options;
  options.mb = 1;  // small: evictions happen
  store.Create(options, kSlots, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), kGpuBase}; },
               [&](uint64_t work) { return work <= done_up_to; });

  constexpr int kBuffers = 64;
  std::vector<std::vector<uint8_t>> guest(kBuffers);
  std::vector<int> kind(kBuffers);  // 0 static, 1 sometimes rewritten, 2 rewritten every frame
  for (int b = 0; b < kBuffers; ++b) {
    guest[b].resize(4096 + 64 * (rng() % 64));
    for (auto& x : guest[b]) x = uint8_t(rng());
    kind[b] = b % 8 == 7 ? 2 : (b % 4 == 3 ? 1 : 0);
  }
  // Blocks bound per work, with the bytes they held when bound.
  std::map<uint64_t, std::vector<std::pair<uint64_t, std::vector<uint8_t>>>> in_flight;
  uint64_t draws = 0, bound = 0, wrong_content = 0, overwritten = 0, static_lookups = 0, static_hits = 0;
  uint64_t work = 0, frame = 1;
  std::vector<uint8_t> expect;
  for (int f = 0; f < 600; ++f, ++frame) {
    store.NewFrame(frame);
    for (int w = 0; w < 3; ++w) {
      ++work;
      // Opening a work waits for its slot's previous work (as Core::BeginWork): everything up to work - kSlots is done.
      if (work > kSlots) done_up_to = std::max(done_up_to, work - kSlots);
      // The GPU finishes one more work on its own now and then.
      if (rng() % 2 && done_up_to + 1 < work) ++done_up_to;
      for (auto it = in_flight.begin(); it != in_flight.end();) {
        if (it->first <= done_up_to) {
          for (const auto& [gpu, bytes] : it->second) {
            if (std::memcmp(store_memory.data() + (gpu - kGpuBase), bytes.data(), bytes.size()) != 0) ++overwritten;
          }
          it = in_flight.erase(it);
        } else {
          ++it;
        }
      }
      store.NewWork(work);
      for (int d = 0; d < 40; ++d) {
        const int b = int(rng() % kBuffers);
        if (kind[b] == 2 && d == 0) {
          for (auto& x : guest[b]) x = uint8_t(rng());
        }
        if (kind[b] == 1 && rng() % 200 == 0) {
          guest[b][rng() % guest[b].size()] ^= 0x5A;
        }
        // A range of whole 16-byte vertices.
        const uint32_t stride = 16;
        const uint32_t vertices = uint32_t(guest[b].size() / stride);
        const uint32_t first = uint32_t(rng() % 4) * 8;
        const uint32_t count = std::min<uint32_t>(vertices - first, 64 + uint32_t(rng() % 3) * 64);
        const uint8_t* range = guest[b].data() + first * stride;
        const uint32_t bytes = count * stride;
        const auto r = store.Find(range, bytes, stride, bytes, [&](uint8_t* target) { SwapWords(target, range, bytes); });
        ++draws;
        expect.resize(bytes);
        SwapWords(expect.data(), range, bytes);
        if (kind[b] == 0 && f > 4) {
          ++static_lookups;
          static_hits += r.gpu != 0;
        }
        if (r.gpu) {
          store.Use(r.entry, work);
          ++bound;
          const uint8_t* held = store_memory.data() + (r.gpu - kGpuBase);
          if (std::memcmp(held, expect.data(), bytes) != 0) ++wrong_content;
          in_flight[work].push_back({r.gpu, std::vector<uint8_t>(held, held + bytes)});
        }
      }
    }
  }
  const auto& s = store.stats();
  std::printf("STORE draws %llu bound %llu wrong_content %llu overwritten_in_flight %llu static_hit_rate %.3f "
              "admitted %llu changes %llu evictions %llu\n",
              (unsigned long long)draws, (unsigned long long)bound, (unsigned long long)wrong_content,
              (unsigned long long)overwritten, static_lookups ? double(static_hits) / double(static_lookups) : 0.0,
              (unsigned long long)s.admitted, (unsigned long long)s.changes, (unsigned long long)s.evictions);
  const int rect_errors = RectTest();
  return (wrong_content || overwritten || rect_errors) ? 1 : 0;
}
