// Offline probe of run 53 (docs/switch-port/16-deko3d-devlog.md section AA), compiled by tests/deko3d/test_run53.py.
//
// STORE: the geometry store as run 53's recorder uses it: trust per epoch on, one XXH3 pass per range, the front end's
// submit copies keyed by their guest address (a new host buffer every draw, never trusted: mode bit 63), guest memory
// in 1 MiB slices placed out of order (as the Switch's guest table). The title rewrites buffers only between epochs for
// the trusted keys (a work, a frame or a non-draw command, as the recorder's Forget calls), and copy-keyed buffers at
// any time. Every range bound from the store must hold the host form of the bytes the draw is given.
// INDEX: indices through the store: the bound bytes equal ConvertIndices' output and the stored lo/hi equal its range.
// SKIN: dk_skin_check.h on palettes of random rigid bones: the skinned vertex equals a direct computation and no bone is
// flagged; scaled, zeroed or index-shifted palettes are flagged.
// LODBIAS: the fetch constant's signed 10 bits at bit 12 of word 4, through NativeSamplerLodBias.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "dk_geometry_store.h"
#include "dk_skin_check.h"
#include "native_index_policy.h"
#include "native_sampler_lod_bias.h"

using namespace rex::graphics::gta4_native;
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

// Guest memory as the Switch has it: 1 MiB slices, each somewhere else on the host (reverse order).
struct GuestMemory {
  static constexpr uint32_t kSlice = 1u << 20, kSlices = 16;
  std::vector<uint8_t> host = std::vector<uint8_t>(size_t(kSlice) * kSlices);
  uint8_t* At(uint32_t guest) { return host.data() + size_t(kSlices - 1 - guest / kSlice) * kSlice + guest % kSlice; }
};

int StoreTest() {
  std::mt19937 rng(53);
  GuestMemory guest;
  std::vector<uint8_t> store_memory(4u << 20);
  constexpr uint64_t kGpu = 0x100000000ull;
  constexpr uint32_t kSlots = 3;
  uint64_t done_up_to = 0;
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 4;
  o.trust_epoch = true;
  store.Create(o, kSlots, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), kGpu}; },
               [&](uint64_t w) { return w <= done_up_to; });
  // 48 buffers of whole vertices, each inside one slice; kind 0 static, 1 rewritten between epochs, 2 a dynamic
  // buffer the front end copies at submit (rewritten at any moment).
  struct Buffer {
    uint32_t guest = 0, bytes = 0;
    int kind = 0;
  };
  std::vector<Buffer> buffers;
  for (int b = 0; b < 48; ++b) {
    Buffer x;
    x.bytes = 2048 + 64 * uint32_t(rng() % 48);
    x.guest = uint32_t(b % 16) * GuestMemory::kSlice + uint32_t(b / 16) * 0x20000;
    x.kind = b % 6 == 5 ? 2 : (b % 3 == 1 ? 1 : 0);
    for (uint32_t i = 0; i < x.bytes; ++i) guest.At(x.guest)[i] = uint8_t(rng());
    buffers.push_back(x);
  }
  std::map<uint64_t, std::vector<std::pair<uint64_t, std::vector<uint8_t>>>> in_flight;
  uint64_t draws = 0, bound = 0, wrong = 0, overwritten = 0, copy_bound = 0, trusted = 0;
  uint64_t work = 0, frame = 1;
  std::vector<uint8_t> expect, copy;
  for (int f = 0; f < 400; ++f, ++frame) {
    store.NewFrame(frame);
    for (int w = 0; w < 3; ++w) {
      ++work;
      if (work > kSlots) done_up_to = std::max(done_up_to, work - kSlots);
      if (rng() % 2 && done_up_to + 1 < work) ++done_up_to;
      for (auto it = in_flight.begin(); it != in_flight.end();) {
        if (it->first <= done_up_to) {
          for (const auto& [gpu, bytes] : it->second) {
            overwritten += std::memcmp(store_memory.data() + (gpu - kGpu), bytes.data(), bytes.size()) != 0;
          }
          it = in_flight.erase(it);
        } else {
          ++it;
        }
      }
      store.NewWork(work);
      // Between epochs: kind 1 buffers may change.
      for (auto& x : buffers) {
        if (x.kind == 1 && rng() % 40 == 0) guest.At(x.guest)[rng() % x.bytes] ^= 0x3C;
      }
      for (int d = 0; d < 40; ++d) {
        if (d == 20) {
          store.Forget();  // a non-draw command in the middle of the work
          for (auto& x : buffers) {
            if (x.kind == 1 && rng() % 40 == 0) guest.At(x.guest)[rng() % x.bytes] ^= 0x5A;
          }
        }
        Buffer& x = buffers[rng() % buffers.size()];
        if (x.kind == 2) {
          // The title rewrites it at any time; the draw uses the copy made at its submit.
          if (rng() % 3 == 0) guest.At(x.guest)[rng() % x.bytes] ^= 0x77;
        }
        const uint32_t stride = 16, first = uint32_t(rng() % 4) * 8;
        const uint32_t count = std::min<uint32_t>(x.bytes / stride - first, 64 + uint32_t(rng() % 3) * 32);
        const uint32_t bytes = count * stride;
        const uint32_t range_guest = x.guest + first * stride;
        const uint8_t* data = guest.At(range_guest);
        uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(data));
        uint64_t mode = stride;
        if (x.kind == 2) {
          copy.assign(data, data + bytes);  // a new host buffer every draw
          data = copy.data();
          key = range_guest;
          mode |= uint64_t(1) << 63;
        }
        const uint64_t trusted_before = store.stats().trusted_hits;
        const auto r = store.Find(data, key, bytes, mode, bytes, [&](uint8_t* t) { SwapWords(t, data, bytes); });
        trusted += store.stats().trusted_hits - trusted_before;
        ++draws;
        expect.resize(bytes);
        SwapWords(expect.data(), data, bytes);
        if (r.gpu) {
          store.Use(r.entry, work);
          ++bound;
          copy_bound += x.kind == 2;
          const uint8_t* held = store_memory.data() + (r.gpu - kGpu);
          wrong += std::memcmp(held, expect.data(), bytes) != 0;
          in_flight[work].push_back({r.gpu, std::vector<uint8_t>(held, held + bytes)});
        }
      }
    }
  }
  std::printf("STORE draws %llu bound %llu wrong %llu overwritten %llu copy_bound %llu trusted %llu\n",
              (unsigned long long)draws, (unsigned long long)bound, (unsigned long long)wrong,
              (unsigned long long)overwritten, (unsigned long long)copy_bound, (unsigned long long)trusted);
  return int(wrong + overwritten);
}

int IndexTest() {
  std::mt19937 rng(531);
  std::vector<uint8_t> store_memory(2u << 20);
  constexpr uint64_t kGpu = 0x200000000ull;
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 2;
  o.trust_epoch = true;
  uint64_t done = 0;
  store.Create(o, 3, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), kGpu}; },
               [&](uint64_t w) { return w <= done; });
  std::vector<std::vector<uint8_t>> lists(24);
  std::vector<bool> wide(24);
  for (size_t i = 0; i < lists.size(); ++i) {
    wide[i] = i % 3 == 0;
    const uint32_t count = 300 + uint32_t(rng() % 900);
    lists[i].resize(size_t(count) * (wide[i] ? 4 : 2));
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t v = 100 + rng() % 5000;
      if (wide[i]) {
        const uint32_t be = __builtin_bswap32(v);
        std::memcpy(lists[i].data() + k * 4, &be, 4);
      } else {
        const uint16_t be = __builtin_bswap16(uint16_t(v));
        std::memcpy(lists[i].data() + k * 2, &be, 2);
      }
    }
  }
  uint64_t lookups = 0, hits = 0, wrong_bytes = 0, wrong_range = 0;
  std::vector<uint8_t> scratch;
  for (uint64_t work = 1; work <= 600; ++work) {
    if (work > 3) done = work - 3;
    store.NewFrame(work);
    store.NewWork(work);
    // Between epochs (the start of the work): a list may change. Within an epoch a trusted key is not checked again;
    // a buffer the title rewrites at any moment is the front end's submit copy, keyed never-trusted (STORE checks it).
    for (auto& l : lists) {
      if (rng() % 500 == 0) l[0] ^= 0x01;
    }
    for (int d = 0; d < 30; ++d) {
      const size_t i = rng() % lists.size();
      const bool index32 = wide[i];
      const uint32_t count = uint32_t(lists[i].size() / (index32 ? 4 : 2));
      const IndexPolicy policy = IndexPolicyFor(4, false, 0, index32);
      IndexConversion c;
      bool converted = false;
      const uint8_t* src = lists[i].data();
      const uint64_t mode = (uint64_t(1) << 61) | uint64_t(index32) | (uint64_t(policy.restart) << 1);
      const auto r = store.Find(src, uint64_t(reinterpret_cast<uintptr_t>(src)), uint32_t(lists[i].size()), mode,
                                uint32_t(lists[i].size()), [&](uint8_t* t) {
                                  c = ConvertIndices(src, count, index32, policy, t);
                                  converted = true;
                                });
      ++lookups;
      scratch.resize(lists[i].size());
      const IndexConversion ref = ConvertIndices(src, count, index32, policy, scratch.data());
      if (r.gpu) {
        ++hits;
        store.Use(r.entry, work);
        if (converted) {
          store.SetRange(r.entry, c.lo, c.hi);
        } else {
          c.lo = r.lo;
          c.hi = r.hi;
        }
        wrong_bytes += std::memcmp(store_memory.data() + (r.gpu - kGpu), scratch.data(), scratch.size()) != 0;
        wrong_range += c.lo != ref.lo || c.hi != ref.hi;
      }
    }
  }
  std::printf("INDEX lookups %llu hits %llu wrong_bytes %llu wrong_range %llu\n", (unsigned long long)lookups,
              (unsigned long long)hits, (unsigned long long)wrong_bytes, (unsigned long long)wrong_range);
  return int(wrong_bytes + wrong_range) + (hits == 0);
}

void StoreBigEndian(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  p[0] = uint8_t(u >> 24);
  p[1] = uint8_t(u >> 16);
  p[2] = uint8_t(u >> 8);
  p[3] = uint8_t(u);
}

int SkinTest() {
  std::mt19937 rng(5353);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  int rounds = 0, errors = 0, flagged_good = 0, missed_bad = 0;
  for (int round = 0; round < 3000; ++round) {
    float bank[256 * 4] = {};
    // 64 rigid bones: rotation from a random unit quaternion, translation within 50 m.
    for (int b = 0; b < 64; ++b) {
      float q[4] = {u(rng), u(rng), u(rng), u(rng)};
      const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      for (float& c : q) c /= n;
      const float x = q[0], y = q[1], z = q[2], w = q[3];
      const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
      for (int r = 0; r < 3; ++r) {
        float* row = bank + (64 + b * 3 + r) * 4;
        row[0] = m[r][0];
        row[1] = m[r][1];
        row[2] = m[r][2];
        row[3] = 50.0f * u(rng);
      }
    }
    const int kind = round % 4;  // 0 good, 1 scaled bone, 2 zero bone, 3 indices one register off
    uint8_t bones[4] = {uint8_t(rng() % 64), uint8_t(rng() % 64), uint8_t(rng() % 64), uint8_t(rng() % 64)};
    uint8_t weights[4] = {uint8_t(100 + rng() % 100), 0, 0, 0};
    weights[1] = uint8_t(255 - weights[0]);
    if (kind == 1) {
      for (int k = 0; k < 12; ++k) bank[(64 + bones[0] * 3) * 4 + k] *= 3.0f;
    } else if (kind == 2) {
      for (int k = 0; k < 12; ++k) bank[(64 + bones[0] * 3) * 4 + k] = 0.0f;
    }
    const float p[3] = {u(rng) * 0.5f, u(rng) * 0.5f, u(rng) * 0.9f};
    uint8_t position[12];
    for (int k = 0; k < 3; ++k) StoreBigEndian(position + 4 * k, p[k]);
    // D3DCOLOR guest bytes [A, R, G, B]: components (x, y, z, w) = (R, G, B, A).
    const uint8_t w_guest[4] = {weights[3], weights[0], weights[1], weights[2]};
    uint8_t shown[4] = {bones[0], bones[1], bones[2], bones[3]};
    if (kind == 3) {
      for (uint8_t& b : shown) b = uint8_t(std::min(63, b + 1));  // a bone register off: another bone's rows
    }
    const uint8_t i_guest[4] = {shown[3], shown[0], shown[1], shown[2]};
    const auto k = dk::CheckSkinnedVertex(bank, position, 0x182886, w_guest, 0x182886, i_guest);
    // The direct computation with the true bones and the bank as built.
    float ref[3] = {};
    for (int i = 0; i < 2; ++i) {
      const float wt = weights[i] / 255.0f;
      for (int r = 0; r < 3; ++r) {
        const float* row = bank + (64 + bones[i] * 3 + r) * 4;
        ref[r] += wt * (row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3]);
      }
    }
    const bool flagged = k.bones_not_rigid || k.bones_zero || k.distance > 3.0f;
    if (kind == 0) {
      const float d = std::fabs(k.skinned[0] - ref[0]) + std::fabs(k.skinned[1] - ref[1]) + std::fabs(k.skinned[2] - ref[2]);
      errors += !k.decoded || k.bones_used != 2 || d > 1e-3f;
      flagged_good += flagged;
    } else if (kind == 1 || kind == 2) {
      missed_bad += !flagged;
    }
    ++rounds;
  }
  std::printf("SKIN rounds %d errors %d flagged_good %d missed_bad %d\n", rounds, errors, flagged_good, missed_bad);
  return errors + flagged_good + missed_bad;
}

int LodBiasTest() {
  int errors = 0;
  for (int32_t v = -512; v < 512; ++v) {
    const uint32_t word4 = (uint32_t(v) & 0x3FF) << 12 | 0xA5A00A5Au;  // other bits of the word set around it
    const uint32_t clean = (word4 & ~(0x3FFu << 12)) | ((uint32_t(v) & 0x3FF) << 12);
    const int32_t decoded = int32_t(clean << 10) >> 22;
    const float bias = NativeSamplerLodBias(decoded, 15.0f);
    const float expect = std::clamp(float(v) / 32.0f, -15.0f, 15.0f);
    errors += decoded != v || bias != expect;
  }
  std::printf("LODBIAS values 1024 errors %d\n", errors);
  return errors;
}

}  // namespace

int main() {
  const int failures = StoreTest() + IndexTest() + SkinTest() + LodBiasTest();
  return failures ? 1 : 0;
}
