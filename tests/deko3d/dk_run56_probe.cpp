// Run 56's offline probe (tests/deko3d/test_run56.py): the copy arena, the futex waits, the anatomy conversion.
//   dk_run56_probe arena     -> "ARENA ... errors N"
//   dk_run56_probe futex     -> "FUTEX ... errors N"
//   dk_run56_probe anatomy   -> "ANATOMY ... errors N"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <random>
#include <thread>
#include <vector>

#include "native_draw_snapshot.h"
#include "dk_anatomy.h"
#include <rex/thread/nx_multi_wait.h>

using namespace rex::graphics::gta4_native;

static std::atomic<uint64_t> g_news{0};
void* operator new(size_t n) {
  g_news.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

// ---- ARENA: the pool's copies in the arena, a recorder thread holding and dropping them in order.
static int ArenaTest() {
  int errors = 0;
  // 1. The ring alone: random sizes, spans released oldest first after a random delay; no overlap of live spans.
  {
    DrawCopyArena arena(1u << 20);
    struct Live {
      uint8_t* p;
      size_t n;
      uint64_t gen;
      bool done;
    };
    std::vector<Live> owners(DrawCopyArena::kSpans);
    std::mt19937 rng(56);
    std::deque<uint32_t> fifo;
    uint64_t allocated = 0, refused = 0;
    for (uint32_t i = 0; i < 200000; ++i) {
      const uint32_t owner = i % DrawCopyArena::kSpans;
      if (!fifo.empty() && (rng() % 3 == 0 || fifo.size() > 200)) {
        owners[fifo.front()].done = true;
        fifo.pop_front();
      }
      if (owners[owner].p && !owners[owner].done) {
        continue;  // still live: the pool would not take it
      }
      const size_t n = 1 + rng() % (rng() % 8 == 0 ? 200000 : 4000);
      const uint64_t gen = i + 1;
      uint8_t* p = arena.Allocate(n, owner, gen, [&](const DrawCopyArena::Span& s) {
        return owners[s.owner].gen != s.generation || owners[s.owner].done;
      });
      if (!p) {
        ++refused;
        continue;
      }
      ++allocated;
      // no live span may overlap the new one
      for (uint32_t k : fifo) {
        const Live& l = owners[k];
        if (!l.done && l.p < p + n && p < l.p + l.n) {
          ++errors;
        }
      }
      std::memset(p, uint8_t(i), n);
      owners[owner] = {p, n, gen, false};
      fifo.push_back(owner);
      // the oldest live ones still hold their bytes
      if (!fifo.empty()) {
        const Live& l = owners[fifo.front()];
        if (!l.done && l.n && l.p[0] != l.p[l.n - 1]) ++errors;
      }
    }
    std::printf("ARENA ring: %llu spans, %llu refused (full), peak %zu KiB of 1024\n", (unsigned long long)allocated,
                (unsigned long long)refused, arena.in_use_peak() >> 10);
    if (!allocated) ++errors;
  }
  // 2. The pool: copies of a buffer the "title" rewrites right after each submit; the recorder thread checks every copy
  // against the bytes at submit, drops it after a random delay, in order.
  {
    DrawDataPool pool;
    std::vector<uint8_t> vertices(256 * 1024), indices(16 * 1024);
    struct Item {
      std::shared_ptr<const RecorderDrawData> data;
      uint64_t sum;
    };
    std::deque<Item> queue;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    std::atomic<uint64_t> checked{0}, wrong{0};
    std::thread recorder([&] {
      std::mt19937 rng(7);
      for (;;) {
        Item it;
        {
          std::unique_lock<std::mutex> lock(m);
          cv.wait(lock, [&] { return stop || !queue.empty(); });
          if (queue.empty()) return;
          it = std::move(queue.front());
          queue.pop_front();
        }
        const RecorderDrawData& d = *it.data;
        const uint64_t sum = HashBytes(0, d.base(), d.used);
        wrong += sum != it.sum;
        ++checked;
        if (rng() % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
        it.data.reset();  // the slot's release
      }
    });
    std::mt19937 rng(9);
    uint64_t news_warm = 0, take_news = 0, entries_made = 0;
    const uint32_t kDraws = 60000;
    for (uint32_t i = 0; i < kDraws; ++i) {
      if (i == kDraws / 2) news_warm = g_news.load();
      for (auto& b : vertices) b = uint8_t(rng());
      const uint32_t count = 16 + rng() % 4000;
      DrawStreamSpan spans[kSnapshotStreams] = {};
      spans[0] = {vertices.data(), vertices.size(), 32};
      spans[3] = {vertices.data() + 4096, vertices.size() - 4096, 16};
      DrawVertexRange r{true, uint32_t(rng() % 1000), std::min<uint32_t>(count, 6000)};
      const uint32_t index_bytes = (rng() % 2) ? 1024 : 0;
      const uint64_t news_before = g_news.load();
      const uint64_t created_before = pool.created();
      auto data = pool.TakeCopy(indices.data(), index_bytes, spans, (1u << 0) | (1u << 3), r);
      // A take that made a new entry (the pool grows while more copies are in flight, up to kMaxFree) allocates it once;
      // every other take must allocate nothing.
      if (i >= kDraws / 2 && pool.created() == created_before) take_news += g_news.load() - news_before;
      if (pool.created() != created_before) ++entries_made;
      Item it{data, HashBytes(0, data->base(), data->used)};
      // the reference: the same copy made fresh
      RecorderDrawData fresh;
      CopyDrawData(indices.data(), data->index_bytes, spans, (1u << 0) | (1u << 3), r, fresh);
      if (fresh.used != data->used || std::memcmp(fresh.bytes.data(), data->base(), fresh.used) != 0 ||
          fresh.stream_offset != data->stream_offset || fresh.stream_bytes != data->stream_bytes) {
        ++errors;
      }
      {
        std::lock_guard<std::mutex> lock(m);
        queue.push_back(std::move(it));
        while (queue.size() > 3000) {  // the ring's back-pressure
          m.unlock();
          std::this_thread::yield();
          m.lock();
        }
      }
      cv.notify_one();
    }
    const uint64_t news_after = g_news.load();
    {
      std::lock_guard<std::mutex> lock(m);
      stop = true;
    }
    cv.notify_all();
    recorder.join();
    errors += int(wrong.load());
    std::printf("ARENA pool: %u copies, %llu checked by the recorder thread, %llu wrong; %llu in the arena, %llu without "
                "room; pool entries made %llu; operator new inside the takes that reused an entry, warm half: %llu (the "
                "whole loop's, the test's own fresh copies and queue nodes included: %llu)\n",
                kDraws, (unsigned long long)checked.load(), (unsigned long long)wrong.load(),
                (unsigned long long)pool.arena_copies(), (unsigned long long)pool.arena_full(),
                (unsigned long long)entries_made, (unsigned long long)take_news,
                (unsigned long long)(news_after - news_warm));
    if (checked.load() != kDraws || pool.arena_copies() < kDraws / 2 || take_news != 0) ++errors;
  }
  std::printf("ARENA errors %d\n", errors);
  return errors;
}

// ---- FUTEX: the Switch's semaphore and event logic (threading_switch.cpp) on NxConditionBase in the futex mode.
enum class Result { kSuccess, kTimeout, kFailed };
using Base = rex::thread::detail::NxConditionBase<Result>;
namespace nx_sync = rex::thread::nx_sync;

class Sem : public Base {
 public:
  explicit Sem(uint32_t max) : max_(max) {}
  bool Signal() override { return Release(1); }
  bool Release(uint32_t n) {  // C5's lock-free release, as threading_switch.cpp
    uint32_t c = count_.load(std::memory_order_relaxed);
    do {
      if (n > max_ - c) return false;
    } while (!count_.compare_exchange_weak(c, c + n, std::memory_order_seq_cst, std::memory_order_relaxed));
    WakeSleepers(n == 1);
    return true;
  }
  uint32_t count() const { return count_.load(); }

 protected:
  bool signaled() const override { return count_.load(std::memory_order_seq_cst) > 0; }
  void post_execution() override {
    count_.fetch_sub(1, std::memory_order_seq_cst);
    if (!nx_sync::NotifyOne().load(std::memory_order_relaxed)) cond_.notify_all();
  }
  std::atomic<uint32_t> count_{0};
  const uint32_t max_;
};

class Ev : public Base {
 public:
  explicit Ev(bool manual) : manual_(manual) {}
  bool Signal() override {
    signal_.store(true, std::memory_order_seq_cst);
    WakeSleepers(!manual_);
    return true;
  }

 protected:
  bool signaled() const override { return signal_.load(std::memory_order_seq_cst); }
  void post_execution() override {
    if (!manual_) signal_.store(false, std::memory_order_seq_cst);
  }
  std::atomic<bool> signal_{false};
  const bool manual_;
};

static int FutexTest(bool futex) {
  nx_sync::LockFreeRelease().store(true);
  nx_sync::NotifyOne().store(true);
  nx_sync::SubscribedMultiWait().store(true);
  nx_sync::FutexWaits().store(futex);
  int errors = 0;
  const auto t0 = std::chrono::steady_clock::now();
  // 1. A semaphore: 2 producers release 1 count 40,000 times each (GTA IV's job dispatcher), 3 consumers take them.
  {
    Sem sem(1u << 30);
    std::atomic<uint64_t> taken{0};
    const uint64_t kTotal = 80000;
    std::vector<std::thread> threads;
    for (int c = 0; c < 3; ++c) {
      threads.emplace_back([&] {
        while (taken.load() < kTotal) {
          if (sem.Wait(std::chrono::milliseconds(200)) == Result::kSuccess) ++taken;
        }
      });
    }
    for (int p = 0; p < 2; ++p) {
      threads.emplace_back([&] {
        for (uint64_t i = 0; i < kTotal / 2; ++i) {
          sem.Release(1);
          if ((i & 255) == 0) std::this_thread::yield();
        }
      });
    }
    for (auto& t : threads) t.join();
    if (taken.load() != kTotal || sem.count() != 0) ++errors;
    std::printf("FUTEX(%d) semaphore: %llu of %llu counts taken, %u left\n", int(futex),
                (unsigned long long)taken.load(), (unsigned long long)kTotal, sem.count());
  }
  // 2. Auto-reset events, ping-pong 20,000 times: every signal reaches the other thread (no lost wake-up).
  {
    Ev ping(false), pong(false);
    std::atomic<int> missed{0};
    std::thread other([&] {
      for (int i = 0; i < 20000; ++i) {
        if (ping.Wait(std::chrono::milliseconds(2000)) != Result::kSuccess) ++missed;
        pong.Signal();
      }
    });
    for (int i = 0; i < 20000; ++i) {
      ping.Signal();
      if (pong.Wait(std::chrono::milliseconds(2000)) != Result::kSuccess) ++missed;
    }
    other.join();
    errors += missed.load();
    std::printf("FUTEX(%d) events: 20000 round trips, %d timed out\n", int(futex), missed.load());
  }
  // 3. A timed wait on nothing times out (and not much later than asked).
  {
    Sem idle(4);
    const auto a = std::chrono::steady_clock::now();
    const Result r = idle.Wait(std::chrono::milliseconds(30));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    if (r != Result::kTimeout || ms < 25 || ms > 500) ++errors;
    std::printf("FUTEX(%d) timeout: %s after %.1f ms\n", int(futex), r == Result::kTimeout ? "timed out" : "SUCCEEDED",
                ms);
  }
  // 4. Multi-object waiters (WaitForMultipleObjects) next to single-object ones on the same semaphores.
  {
    Sem a(1u << 20), b(1u << 20);
    std::atomic<uint64_t> got{0};
    const uint64_t kTotal = 20000;
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
      while (got.load() < kTotal) {
        auto r = Base::WaitMultiple({&a, &b}, false, std::chrono::milliseconds(200));
        if (r.first == Result::kSuccess) ++got;
      }
    });
    threads.emplace_back([&] {
      while (got.load() < kTotal) {
        if (a.Wait(std::chrono::milliseconds(200)) == Result::kSuccess) ++got;
      }
    });
    threads.emplace_back([&] {
      for (uint64_t i = 0; i < kTotal; ++i) {
        (i & 1 ? a : b).Release(1);
        if ((i & 63) == 0) std::this_thread::yield();
      }
    });
    for (auto& t : threads) t.join();
    if (got.load() != kTotal || a.count() + b.count() != 0) ++errors;
    std::printf("FUTEX(%d) multi-object: %llu of %llu taken, %u left\n", int(futex), (unsigned long long)got.load(),
                (unsigned long long)kTotal, a.count() + b.count());
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("FUTEX(%d) errors %d (%.2f s)\n", int(futex), errors, s);
  return errors;
}

// ---- ANATOMY: the conversions of dk_anatomy.h.
static int AnatomyTest() {
  using namespace rex::graphics::gta4_native::dk;
  int errors = 0;
  const struct {
    uint16_t h;
    float f;
  } halves[] = {{0x0000, 0.0f}, {0x3C00, 1.0f}, {0xC000, -2.0f}, {0x7BFF, 65504.0f}, {0x0001, 5.9604645e-8f},
                {0x3555, 0.33325195f}};
  for (const auto& t : halves) {
    if (std::fabs(HalfToFloat(t.h) - t.f) > 1e-6f * std::max(1.0f, std::fabs(t.f))) ++errors;
  }
  if (!std::isnan(HalfToFloat(0x7E00)) || !std::isinf(HalfToFloat(0x7C00))) ++errors;
  // RGBA16F 4x1: 1.0, 2.0, NaN, 0.5
  const uint16_t px[16] = {0x3C00, 0x3C00, 0x3C00, 0x3C00, 0x4000, 0, 0, 0x3C00,
                           0x7E00, 0, 0, 0, 0x3800, 0x3800, 0x3800, 0x3C00};
  std::vector<uint8_t> rgb;
  AnatomyStats st = AnatomyToRgb(AnatomyFormat::kRGBA16F, reinterpret_cast<const uint8_t*>(px), 4, 1, rgb);
  if (st.nan != 1 || st.max[0] != 2.0f || rgb[6] != 255 || rgb[7] != 0 || rgb[8] != 255) ++errors;
  // depth + stencil 2x1
  uint8_t z[16] = {};
  const float d0 = 0.25f, d1 = 0.75f;
  std::memcpy(z, &d0, 4);
  z[4] = 0x80;
  std::memcpy(z + 8, &d1, 4);
  z[12] = 0x01;
  st = AnatomyToRgb(AnatomyFormat::kZ32FS8, z, 2, 1, rgb);
  if (rgb[0] != 0x80 || rgb[1] != 0 || rgb[3] != 1 || rgb[4] != 255 || st.min[0] != 0.25f || st.max[0] != 0.75f) ++errors;
  // RGBA8 as it is
  const uint8_t c[4] = {10, 20, 30, 40};
  AnatomyToRgb(AnatomyFormat::kRGBA8, c, 1, 1, rgb);
  if (rgb[0] != 10 || rgb[1] != 20 || rgb[2] != 30) ++errors;
  std::printf("ANATOMY half floats, NaN marking, depth/stencil, RGBA8; %s\n", st.Describe().c_str());
  std::printf("ANATOMY errors %d\n", errors);
  return errors;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  if (!std::strcmp(argv[1], "arena")) return ArenaTest() ? 1 : 0;
  if (!std::strcmp(argv[1], "futex")) return (FutexTest(true) + FutexTest(false)) ? 1 : 0;
  if (!std::strcmp(argv[1], "anatomy")) return AnatomyTest() ? 1 : 0;
  return 2;
}
