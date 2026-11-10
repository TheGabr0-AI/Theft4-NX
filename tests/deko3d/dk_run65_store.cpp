// Offline probe of run 65 (docs/switch-port/16-deko3d-devlog.md section AM), compiled by tests/deko3d/test_run65.py.
//
// FRAMES: the geometry store with trust across frames (dk_geometry_store.h, run 65: trust_frames = 8) over the command
// stream of dk_run64_store.cpp: buffers never written, written through a Lock with its signal at any moment, copied at
// submit, drawn while locked, reached through two objects. With every write signalled, every range bound from the
// store must hold the bytes the draw is given, as with trust_frames = 1, and fewer bytes are hashed.
// RAW: buffers also written with no Lock between frames (what run 64 counted 0 times on the console). The store may
// then bind an old range, but only for less than trust_frames frames after the write and only until the object is
// found out: each change without a signal sends its object back to a check every frame, and the eighth ends the trust
// across frames for all (trust_frames() == 1), after which no old range is bound.
// BENCH: run 63's shape with the per-frame check (run 64) and with trust_frames = 8. A PC measure, not the console's.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "dk_geometry_store.h"

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

struct Outcome {
  uint64_t draws = 0, bound = 0, wrong = 0, wrong_after_end = 0, oldest_wrong = 0, frame_hits = 0, hashed = 0,
           unsignalled = 0, distrusted = 0, raw_writes = 0;
  uint32_t trust_frames_at_end = 0;
};

Outcome Run(uint32_t trust_frames, bool raw, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> memory(48u * 8192), store_memory(4u << 20);
  constexpr uint64_t kGpu = 0x100000000ull;
  constexpr uint32_t kSlots = 3;
  uint64_t done_up_to = 0;
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 4;
  o.trust_epoch = true;
  o.trust_signals = true;
  o.trust_frames = trust_frames;
  store.Create(o, kSlots, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), kGpu}; },
               [&](uint64_t w) { return w <= done_up_to; });
  struct Buffer {
    uint8_t* data = nullptr;
    uint32_t bytes = 0, handle = 0, second = 0;
    int kind = 0;
    bool locked = false;
    uint64_t raw_frame = 0;  // the frame before which it was last written with no Lock
  };
  std::vector<Buffer> buffers;
  for (uint32_t b = 0; b < 48; ++b) {
    Buffer x;
    x.data = memory.data() + size_t(b) * 8192;
    x.bytes = 2048 + 64 * uint32_t(rng() % 48);
    x.handle = 0x40100000u + b * 0x40;
    x.second = 0x40900000u + b * 0x40;
    x.kind = int(b % 6);
    for (uint32_t i = 0; i < x.bytes; ++i) x.data[i] = uint8_t(rng());
    buffers.push_back(x);
  }
  Outcome out;
  std::vector<uint8_t> expect, copy;
  uint64_t work = 0;
  for (uint64_t frame = 1; frame <= 900; ++frame) {
    store.NewFrame(frame);
    if (raw && frame > 20) {
      for (Buffer& x : buffers) {
        if (x.kind == 4 && rng() % 40 == 0) {
          x.data[rng() % x.bytes] ^= 0x11;  // no Lock, no signal: between two frames
          x.raw_frame = frame;
          ++out.raw_writes;
        }
      }
    }
    for (int w = 0; w < 3; ++w) {
      ++work;
      if (work > kSlots) done_up_to = std::max(done_up_to, work - kSlots);
      store.NewWork(work);
      for (int d = 0; d < 60; ++d) {
        if (d % 20 == 10) {
          store.Forget();
        }
        Buffer& x = buffers[rng() % buffers.size()];
        uint32_t owner = x.handle;
        switch (x.kind) {
          case 1:
            if (rng() % 5 == 0) {
              x.data[rng() % x.bytes] ^= 0x3C;
              store.Written(x.handle);
            }
            break;
          case 2:
            if (rng() % 3 == 0) x.data[rng() % x.bytes] ^= 0x77;
            break;
          case 3:
            if (!x.locked && rng() % 6 == 0) {
              x.locked = true;
            } else if (x.locked && rng() % 3 == 0) {
              x.locked = false;
              store.Written(x.handle);
            }
            if (x.locked) {
              x.data[rng() % x.bytes] ^= 0x5A;
              owner = 0;
            }
            break;
          case 5:
            if (frame > 4 && rng() % 5 == 0) {
              x.data[rng() % x.bytes] ^= 0x66;
              store.Written(rng() % 2 ? x.handle : x.second);
            }
            owner = rng() % 2 ? x.handle : x.second;
            break;
          default:
            break;
        }
        const uint32_t stride = 16, first = x.kind == 5 ? 0 : uint32_t(rng() % 4) * 8;
        const uint32_t count =
            x.kind == 5 ? 96 : std::min<uint32_t>(x.bytes / stride - first, 64 + uint32_t(rng() % 3) * 32);
        const uint32_t bytes = count * stride;
        const uint8_t* data = x.data + first * stride;
        uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(data));
        uint64_t mode = stride;
        if (x.kind == 2) {
          copy.assign(data, data + bytes);
          data = copy.data();
          key = uint64_t(x.handle) * 0x10000 + first * stride;
          mode |= uint64_t(1) << 63;
        }
        const uint32_t trust_before = store.trust_frames();
        const auto r = store.Find(data, key, bytes, mode, bytes, owner, [&](uint8_t* t) { SwapWords(t, data, bytes); });
        ++out.draws;
        if (r.gpu) {
          store.Use(r.entry, work);
          ++out.bound;
          expect.resize(bytes);
          SwapWords(expect.data(), data, bytes);
          if (std::memcmp(store_memory.data() + (r.gpu - kGpu), expect.data(), bytes) != 0) {
            ++out.wrong;
            out.wrong_after_end += trust_before == 1;
            out.oldest_wrong = std::max(out.oldest_wrong, x.kind == 4 ? frame - x.raw_frame : uint64_t(999));
          }
        }
      }
    }
  }
  out.frame_hits = store.stats().frame_hits;
  out.hashed = store.stats().hashed_bytes;
  out.unsignalled = store.stats().changes_unsignalled;
  out.distrusted = store.stats().distrusted;
  out.trust_frames_at_end = store.trust_frames();
  return out;
}

void Print(const char* name, const Outcome& r) {
  std::printf("%s draws %llu bound %llu wrong %llu wrong_after_end %llu oldest_wrong_frames %llu frame_hits %llu "
              "hashed_kib %llu unsignalled %llu distrusted %llu raw_writes %llu trust_frames_at_end %u\n",
              name, (unsigned long long)r.draws, (unsigned long long)r.bound, (unsigned long long)r.wrong,
              (unsigned long long)r.wrong_after_end, (unsigned long long)r.oldest_wrong, (unsigned long long)r.frame_hits,
              (unsigned long long)(r.hashed >> 10), (unsigned long long)r.unsignalled, (unsigned long long)r.distrusted,
              (unsigned long long)r.raw_writes, r.trust_frames_at_end);
}

int FramesTest() {
  const Outcome one = Run(1, false, 65);
  const Outcome eight = Run(8, false, 65);
  Print("FRAMES 1", one);
  Print("FRAMES 8", eight);
  int failures = 0;
  failures += one.wrong != 0 || eight.wrong != 0 || eight.bound == 0;
  failures += one.frame_hits != 0 || eight.frame_hits == 0;
  failures += eight.hashed >= one.hashed;
  failures += eight.unsignalled != 0 || eight.trust_frames_at_end != 8;
  const Outcome raw = Run(8, true, 65);
  Print("RAW 8", raw);
  failures += raw.unsignalled < GeometryStore::kUnsignalledLimit || raw.trust_frames_at_end != 1;
  failures += raw.wrong_after_end != 0;       // once the trust across frames has ended, as run 64: nothing old
  failures += raw.wrong != 0 && raw.oldest_wrong >= 8;  // an old range never outlives trust_frames
  failures += raw.distrusted == 0;
  return failures;
}

struct Bench {
  double ns_per_lookup = 0;
  uint64_t lookups = 0, hashed = 0, hits = 0;
};

Bench RunBench(uint32_t trust_frames) {
  std::mt19937 rng(63);
  constexpr uint32_t kRanges = 2400, kFrames = 240;
  std::vector<uint32_t> offset(kRanges), size(kRanges);
  uint32_t total = 0;
  for (uint32_t i = 0; i < kRanges; ++i) {
    size[i] = 1024 + 64 * uint32_t(rng() % 220);
    offset[i] = total;
    total += size[i];
  }
  std::vector<uint8_t> memory(total), store_memory(64u << 20);
  for (uint8_t& b : memory) b = uint8_t(rng());
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 64;
  o.trust_epoch = true;
  o.trust_signals = true;
  o.trust_frames = trust_frames;
  uint64_t done = 0;
  store.Create(o, 3, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), 0x400000000ull}; },
               [&](uint64_t w) { return w <= done; });
  auto frame_of = [&](uint64_t frame) {
    done = frame > 3 ? frame - 3 : 0;
    store.NewFrame(frame);
    store.NewWork(frame);
    for (int pass = 0; pass < 3; ++pass) {
      if (pass == 2) {
        for (uint32_t k = 0; k < 24; ++k) store.Written(0x40F00000u + k * 0x40);
        store.Forget();
      }
      for (uint32_t i = 0; i < kRanges; ++i) {
        const uint8_t* data = memory.data() + offset[i];
        const auto r = store.Find(data, uint64_t(reinterpret_cast<uintptr_t>(data)), size[i], 32, size[i],
                                  0x40300000u + i * 0x40, [&](uint8_t* t) { std::memcpy(t, data, size[i]); });
        store.Use(r.entry, frame);
      }
    }
  };
  for (uint64_t f = 1; f <= 8; ++f) frame_of(f);
  const GeometryStore::Stats before = store.stats();
  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t f = 9; f < 9 + kFrames; ++f) frame_of(f);
  const auto t1 = std::chrono::steady_clock::now();
  const GeometryStore::Stats& s = store.stats();
  Bench b;
  b.lookups = s.lookups - before.lookups;
  b.hashed = s.hashed_bytes - before.hashed_bytes;
  b.hits = s.hits - before.hits;
  b.ns_per_lookup = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / double(b.lookups);
  return b;
}

int BenchTest() {
  Bench best[2];
  const uint32_t frames[2] = {1, 8};
  for (int k = 0; k < 2; ++k) {
    for (int round = 0; round < 3; ++round) {
      const Bench b = RunBench(frames[k]);
      if (round == 0 || b.ns_per_lookup < best[k].ns_per_lookup) best[k] = b;
    }
    std::printf("BENCH trust_frames %u lookups %llu hits %llu hashed_bytes_per_lookup %llu ns_per_lookup %.1f\n", frames[k],
                (unsigned long long)best[k].lookups, (unsigned long long)best[k].hits,
                (unsigned long long)(best[k].hashed / best[k].lookups), best[k].ns_per_lookup);
  }
  return (best[0].hits != best[0].lookups) + (best[1].hits != best[1].lookups) + (best[1].hashed * 4 > best[0].hashed) +
         (best[1].ns_per_lookup >= best[0].ns_per_lookup);
}

}  // namespace

int main() {
  const int failures = FramesTest() + BenchTest();
  std::printf("RUN65 failures %d\n", failures);
  return failures ? 1 : 0;
}
