// Offline probe of run 64 (docs/switch-port/16-deko3d-devlog.md section AL), compiled by tests/deko3d/test_run64.py.
//
// SIGNALS: the geometry store with trust by write signal (dk_geometry_store.h, run 64) over a simulated command stream
// in which the title writes its buffers at any moment of a frame, in the middle of a work and between two draws of
// the same range. Six kinds of buffer:
//   0 never written;
//   1 written between Lock and Unlock, the unlock sent as a write signal (Written) before the next draw;
//   2 a dynamic buffer the front end copies at submit (mode bit 63, a new host address every draw), written at any time;
//   3 drawn while the title holds it locked (the draw passes owner 0), the signal sent at the unlock, later;
//   4 written without any Lock (no signal), only between frames when `raw` is on: found by the hash of the next frame;
//   5 one range reached through two buffer objects, written through either with that object's signal, once the store
//     has met both (a range met through one object only is trusted on that object's signals: the rule's assumption is
//     that a buffer is written through the object it is drawn through; a write that breaks it is found by the next
//     frame's hash and counted as a change without a signal).
// Every range bound from the store must hold the host form of the bytes the draw is given, with the signals on and
// off; the signals must save hashes; a change without a signal must be counted when there is one and only then.
// RELEASE: a buffer object released and its memory given to a new object with other bytes within one frame.
// BENCH: run 63's shape (2,400 ranges of 1-15 KiB, each drawn twice in one epoch and once in another every frame, as
// its 32 % of lookups trusted within an epoch; 24 unrelated write signals a frame): time and bytes hashed per lookup
// with the signals off and on. A PC measure of the same code, not the console's.

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
  uint64_t draws = 0, bound = 0, wrong = 0, signal_hits = 0, hashed = 0, changes = 0, unsignalled = 0, raw_writes = 0;
};

Outcome Run(bool signals, bool raw, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> memory(48u * 8192), store_memory(4u << 20);
  constexpr uint64_t kGpu = 0x100000000ull;
  constexpr uint32_t kSlots = 3;
  uint64_t done_up_to = 0;
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 4;
  o.trust_epoch = true;
  o.trust_signals = signals;
  store.Create(o, kSlots, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), kGpu}; },
               [&](uint64_t w) { return w <= done_up_to; });
  struct Buffer {
    uint8_t* data = nullptr;
    uint32_t bytes = 0, handle = 0, second = 0;
    int kind = 0;
    bool locked = false;
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
  for (uint64_t frame = 1; frame <= 600; ++frame) {
    store.NewFrame(frame);
    if (raw) {
      for (Buffer& x : buffers) {
        if (x.kind == 4 && rng() % 12 == 0) {
          x.data[rng() % x.bytes] ^= 0x11;  // no Lock, no signal: between two frames only
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
          store.Forget();  // a clear or a resolve in the middle of the work
        }
        Buffer& x = buffers[rng() % buffers.size()];
        uint32_t owner = x.handle;
        switch (x.kind) {
          case 1:  // Lock, write, Unlock: the signal reaches the recorder before the next draw of the stream
            if (rng() % 5 == 0) {
              x.data[rng() % x.bytes] ^= 0x3C;
              store.Written(x.handle);
            }
            break;
          case 2:
            if (rng() % 3 == 0) x.data[rng() % x.bytes] ^= 0x77;
            break;
          case 3:  // locked across draws: written while locked, the signal only at the unlock
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
          case 5:  // two objects on one range: written through one, drawn through either (from the second frame on:
                   // the store has then met both objects on the range and trusts neither's signal for it)
            if (frame > 4 && rng() % 5 == 0) {
              x.data[rng() % x.bytes] ^= 0x66;
              store.Written(rng() % 2 ? x.handle : x.second);
            }
            owner = rng() % 2 ? x.handle : x.second;
            break;
          default:
            break;
        }
        // Kind 5 draws one range (so the store meets both objects on it in the first frames).
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
        const auto r = store.Find(data, key, bytes, mode, bytes, owner, [&](uint8_t* t) { SwapWords(t, data, bytes); });
        ++out.draws;
        if (r.gpu) {
          store.Use(r.entry, work);
          ++out.bound;
          expect.resize(bytes);
          SwapWords(expect.data(), data, bytes);
          out.wrong += std::memcmp(store_memory.data() + (r.gpu - kGpu), expect.data(), bytes) != 0;
        }
      }
    }
  }
  out.signal_hits = store.stats().signal_hits;
  out.hashed = store.stats().hashed_bytes;
  out.changes = store.stats().changes;
  out.unsignalled = store.stats().changes_unsignalled;
  return out;
}

int SignalsTest() {
  int failures = 0;
  const struct {
    const char* name;
    bool signals, raw;
  } cases[] = {{"off", false, false}, {"on", true, false}, {"on+raw", true, true}};
  uint64_t hashed_off = 0, hashed_on = 0;
  for (const auto& c : cases) {
    const Outcome r = Run(c.signals, c.raw, 64);
    std::printf("SIGNALS %s draws %llu bound %llu wrong %llu signal_hits %llu hashed_kib %llu changes %llu unsignalled "
                "%llu raw_writes %llu\n",
                c.name, (unsigned long long)r.draws, (unsigned long long)r.bound, (unsigned long long)r.wrong,
                (unsigned long long)r.signal_hits, (unsigned long long)(r.hashed >> 10), (unsigned long long)r.changes,
                (unsigned long long)r.unsignalled, (unsigned long long)r.raw_writes);
    failures += r.wrong != 0 || r.bound == 0;
    if (!c.signals) {
      hashed_off = r.hashed;
      failures += r.signal_hits != 0;
    } else if (!c.raw) {
      hashed_on = r.hashed;
      failures += r.signal_hits == 0 || r.unsignalled != 0;
    } else {
      failures += r.unsignalled == 0;
    }
  }
  failures += hashed_on >= hashed_off;
  return failures;
}

// A buffer object released and its memory handed to a new object with other bytes, drawn again in the same frame.
int ReleaseTest() {
  std::vector<uint8_t> memory(4096), store_memory(1u << 20);
  constexpr uint64_t kGpu = 0x300000000ull;
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 1;
  o.trust_epoch = true;
  o.trust_signals = true;
  uint64_t done = 0;
  store.Create(o, 3, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), kGpu}; },
               [&](uint64_t w) { return w <= done; });
  std::mt19937 rng(640);
  int wrong = 0, bound = 0, trusted = 0;
  std::vector<uint8_t> expect(4096);
  const uint32_t handle = 0x40200000u;
  for (uint64_t frame = 1; frame <= 200; ++frame) {
    done = frame > 3 ? frame - 3 : 0;
    store.NewFrame(frame);
    store.NewWork(frame);
    for (int d = 0; d < 12; ++d) {
      if (d == 6 && frame % 3 == 0) {
        store.Written(handle);  // released: the same address and the same handle value come back with a new model
        for (uint8_t& b : memory) b = uint8_t(rng());
      }
      store.Forget();
      const uint64_t before = store.stats().signal_hits;
      const auto r = store.Find(memory.data(), uint64_t(reinterpret_cast<uintptr_t>(memory.data())), 4096, 16, 4096,
                                handle, [&](uint8_t* t) { SwapWords(t, memory.data(), 4096); });
      trusted += int(store.stats().signal_hits - before);
      if (r.gpu) {
        store.Use(r.entry, frame);
        ++bound;
        SwapWords(expect.data(), memory.data(), 4096);
        wrong += std::memcmp(store_memory.data() + (r.gpu - kGpu), expect.data(), 4096) != 0;
      }
    }
  }
  std::printf("RELEASE bound %d wrong %d trusted %d\n", bound, wrong, trusted);
  return wrong + (bound == 0) + (trusted == 0);
}

struct Bench {
  double ns_per_lookup = 0;
  uint64_t lookups = 0, hashed = 0, hits = 0, signal_hits = 0, epoch_hits = 0;
};

Bench RunBench(bool signals) {
  std::mt19937 rng(63);
  constexpr uint32_t kRanges = 2400, kFrames = 240;
  std::vector<uint32_t> offset(kRanges), size(kRanges);
  uint32_t total = 0;
  for (uint32_t i = 0; i < kRanges; ++i) {
    size[i] = 1024 + 64 * uint32_t(rng() % 220);  // 1-15 KiB, 7.9 KiB on average (run 63: 188 GB in 23.7 M hashes)
    offset[i] = total;
    total += size[i];
  }
  std::vector<uint8_t> memory(total), store_memory(64u << 20);
  for (uint8_t& b : memory) b = uint8_t(rng());
  GeometryStore store;
  GeometryStore::Options o;
  o.mb = 64;
  o.trust_epoch = true;
  o.trust_signals = signals;
  uint64_t done = 0;
  store.Create(o, 3, [&](uint32_t) { return std::pair<uint8_t*, DkGpuAddr>{store_memory.data(), 0x400000000ull}; },
               [&](uint64_t w) { return w <= done; });
  auto frame_of = [&](uint64_t frame) {
    done = frame > 3 ? frame - 3 : 0;
    store.NewFrame(frame);
    store.NewWork(frame);
    // The shadow pass: every range twice in one epoch (two cascades).
    for (int pass = 0; pass < 2; ++pass) {
      for (uint32_t i = 0; i < kRanges; ++i) {
        const uint8_t* data = memory.data() + offset[i];
        const auto r = store.Find(data, uint64_t(reinterpret_cast<uintptr_t>(data)), size[i], 32, size[i],
                                  0x40300000u + i * 0x40, [&](uint8_t* t) { std::memcpy(t, data, size[i]); });
        store.Use(r.entry, frame);
      }
    }
    // Dynamic buffers unlocked after a write (other objects), then the G-buffer pass after a clear.
    for (uint32_t k = 0; k < 24; ++k) store.Written(0x40F00000u + k * 0x40);
    store.Forget();
    for (uint32_t i = 0; i < kRanges; ++i) {
      const uint8_t* data = memory.data() + offset[i];
      const auto r = store.Find(data, uint64_t(reinterpret_cast<uintptr_t>(data)), size[i], 32, size[i],
                                0x40300000u + i * 0x40, [&](uint8_t* t) { std::memcpy(t, data, size[i]); });
      store.Use(r.entry, frame);
    }
  };
  for (uint64_t f = 1; f <= 8; ++f) frame_of(f);  // admission
  const GeometryStore::Stats before = store.stats();
  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t f = 9; f < 9 + kFrames; ++f) frame_of(f);
  const auto t1 = std::chrono::steady_clock::now();
  const GeometryStore::Stats& s = store.stats();
  Bench b;
  b.lookups = s.lookups - before.lookups;
  b.hashed = s.hashed_bytes - before.hashed_bytes;
  b.hits = s.hits - before.hits;
  b.signal_hits = s.signal_hits - before.signal_hits;
  b.epoch_hits = s.trusted_hits - before.trusted_hits;
  b.ns_per_lookup = double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / double(b.lookups);
  return b;
}

int BenchTest() {
  Bench best[2];
  for (int k = 0; k < 2; ++k) {
    for (int round = 0; round < 3; ++round) {
      const Bench b = RunBench(k == 1);
      if (round == 0 || b.ns_per_lookup < best[k].ns_per_lookup) best[k] = b;
    }
    std::printf("BENCH signals %s lookups %llu hits %llu epoch_hits %llu signal_hits %llu hashed_bytes_per_lookup %llu "
                "ns_per_lookup %.1f\n",
                k ? "on" : "off", (unsigned long long)best[k].lookups, (unsigned long long)best[k].hits,
                (unsigned long long)best[k].epoch_hits, (unsigned long long)best[k].signal_hits,
                (unsigned long long)(best[k].hashed / best[k].lookups), best[k].ns_per_lookup);
  }
  return (best[0].hits != best[0].lookups) + (best[1].hits != best[1].lookups) + (best[1].hashed * 10 > best[0].hashed * 6) +
         (best[1].ns_per_lookup >= best[0].ns_per_lookup);
}

}  // namespace

int main() {
  const int failures = SignalsTest() + ReleaseTest() + BenchTest();
  std::printf("RUN64 failures %d\n", failures);
  return failures ? 1 : 0;
}
