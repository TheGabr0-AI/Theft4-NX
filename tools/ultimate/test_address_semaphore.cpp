// Theft4-NX (2026-11-14, thread_semaphore_address_wait; docs/switch-port/20-ultimate-base-migration.md, U9): host
// stress test of the semaphores' address wait in ultimate/sdk/src/core/address_event_core.h (ReleaseLocked,
// BlockUntil), with Windows' WaitOnAddress standing in for svcWaitForAddress. The Semaphore below mirrors
// PosixCondition<Semaphore> in ultimate/sdk/src/core/threading_posix.cpp with the key on (Release, Wait,
// InterruptWaiters). The events' test is the base's ultimate/tools/test_address_event.cpp (run it too: BlockUntil
// is the events' Block).
//
//   C:\devkitPro\msys2\clang64\bin\clang++.exe -O2 -std=c++20 tools/ultimate/test_address_semaphore.cpp
//       -o <out>/test_address_semaphore.exe -lsynchronization
//
// Tests: ping-pong (lost wake-ups); K blocked waiters released by one Release(K) and by K back-to-back Release(1)
// (no unit stranded while a waiter sleeps); the maximum count and the previous count; interrupts (woken, not the
// 100 ms net); a random mix of releases of 1-3 units, infinite / zero / short waits and interrupts, where units are
// conserved exactly (units released = units taken + the count left) and the counters end at 0. Exit code 0 when every
// test passed.
#define NOMINMAX
#include <windows.h>
#include <synchapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "../../ultimate/sdk/src/core/address_event_core.h"

using namespace std::chrono;
using rex::thread::detail::AddressEventCore;
using rex::thread::detail::AddressWaitResult;

struct WinArbiter {
  static bool WaitIfEqual(std::atomic<uint32_t>* word, uint32_t value, int64_t ns) {
    DWORD ms = ns < 0 ? INFINITE : DWORD((ns + 999999) / 1000000);
    uint32_t v = value;
    if (WaitOnAddress(reinterpret_cast<volatile void*>(word), &v, sizeof(v), ms)) return true;
    return GetLastError() != ERROR_TIMEOUT;
  }
  static void Wake(std::atomic<uint32_t>* word, int32_t count) {
    if (count == 1)
      WakeByAddressSingle(reinterpret_cast<void*>(word));
    else
      WakeByAddressAll(reinterpret_cast<void*>(word));
  }
};

enum class R { kSuccess, kTimeout };

class Semaphore {
 public:
  Semaphore(uint32_t initial, uint32_t maximum) : count_(initial), maximum_(maximum) {}
  bool Release(uint32_t n, int* previous = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (n > maximum_ - count_) return false;
    if (previous) *previous = int(count_);
    count_ += n;
    const int32_t wake = core_.ReleaseLocked(count_, n);
    Check();
    lock.unlock();
    if (wake) core_.Wake(wake);
    return true;
  }
  void Interrupt() {
    int32_t wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake = core_.InterruptLocked();
    }
    if (wake) core_.Wake(wake);
  }
  // timeout_ms < 0: infinite
  R Wait(int timeout_ms, const std::atomic<bool>* interrupt = nullptr) {
    const bool infinite = timeout_ms < 0;
    const auto deadline = infinite ? steady_clock::time_point::max() : steady_clock::now() + milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mutex_);
    if (count_ > 0) {
      --count_;
      return R::kSuccess;
    }
    if (interrupt && interrupt->load(std::memory_order_acquire)) return R::kTimeout;
    if (!infinite && steady_clock::now() >= deadline) return R::kTimeout;
    const auto r = core_.BlockUntil(lock, [this] { return count_ > 0; }, interrupt, infinite, deadline, 100'000'000);
    Check();
    if (r == AddressWaitResult::kSignaled) {
      --count_;
      return R::kSuccess;
    }
    return r == AddressWaitResult::kGranted ? R::kSuccess : R::kTimeout;
  }
  uint32_t blocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.blocked_locked();
  }
  uint32_t grants() {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.grants_locked();
  }
  uint32_t count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  static std::atomic<int> violations;

 private:
  void Check() {
    if (core_.grants_locked() > core_.blocked_locked() || count_ > maximum_) violations.fetch_add(1);
  }
  std::mutex mutex_;
  uint32_t count_;
  const uint32_t maximum_;
  AddressEventCore<WinArbiter> core_;
};
std::atomic<int> Semaphore::violations{0};

static bool WaitUntil(const std::atomic<bool>& done, int seconds) {
  const auto end = steady_clock::now() + std::chrono::seconds(seconds);
  while (!done.load()) {
    if (steady_clock::now() > end) return false;
    std::this_thread::sleep_for(milliseconds(5));
  }
  return true;
}

static bool PingPong() {
  Semaphore s1(0, 1), s2(0, 1);
  constexpr int N = 200000;
  std::atomic<bool> done{false};
  std::thread b([&] {
    for (int i = 0; i < N; ++i) {
      s1.Wait(-1);
      s2.Release(1);
    }
  });
  std::thread a([&] {
    for (int i = 0; i < N; ++i) {
      s1.Release(1);
      s2.Wait(-1);
    }
    done = true;
  });
  if (!WaitUntil(done, 120)) {
    std::printf("ping-pong: HUNG\n");
    std::_Exit(2);
  }
  a.join();
  b.join();
  std::printf("ping-pong: %d round trips, ok\n", N);
  return true;
}

// K threads blocked; the units arrive as one Release(K) (together) or K Release(1) back to back. Each waiter takes
// one unit a round; none may stay asleep while a unit is left.
static bool MultiRelease(bool together) {
  constexpr int K = 4, Rounds = 20000;
  Semaphore sem(0, 1000);
  std::atomic<int> taken{0}, round{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> waiters;
  for (int k = 0; k < K; ++k)
    waiters.emplace_back([&] {
      int seen = 0;
      while (true) {
        sem.Wait(-1);
        if (stop.load()) return;
        taken.fetch_add(1);
        while (round.load() == seen && !stop.load()) std::this_thread::yield();
        seen = round.load();
      }
    });
  const char* name = together ? "one Release(K)" : "K Release(1)";
  bool ok = true;
  for (int r = 0; r < Rounds && ok; ++r) {
    const auto end = steady_clock::now() + seconds(5);
    while (sem.blocked() != K) {
      if (steady_clock::now() > end) {
        std::printf("%s: round %d: only %u of %d blocked\n", name, r, sem.blocked(), K);
        ok = false;
        break;
      }
      std::this_thread::yield();
    }
    if (!ok) break;
    if (together) {
      sem.Release(K);
    } else {
      for (int k = 0; k < K; ++k) sem.Release(1);
    }
    while (taken.load() != (r + 1) * K) {
      if (steady_clock::now() > end) {
        std::printf("%s: round %d: %d taken of %d (stranded)\n", name, r, taken.load() - r * K, K);
        ok = false;
        break;
      }
      std::this_thread::yield();
    }
    if (sem.count() != 0) {
      std::printf("%s: round %d: count %u left with every waiter served\n", name, r, sem.count());
      ok = false;
    }
    round.fetch_add(1);
  }
  stop = true;
  sem.Release(K * 4);
  for (auto& t : waiters) t.join();
  std::printf("%s: %d rounds of %d waiters, taken %d, %s\n", name, Rounds, K, taken.load(), ok ? "ok" : "FAILED");
  return ok;
}

static bool Limits() {
  Semaphore sem(1, 3);
  int prev = -1;
  bool ok = sem.Release(2, &prev) && prev == 1 && sem.count() == 3;
  ok &= !sem.Release(1, &prev) && sem.count() == 3;  // past the maximum: refused, nothing changed
  ok &= sem.Wait(0) == R::kSuccess && sem.Wait(0) == R::kSuccess && sem.Wait(0) == R::kSuccess;
  ok &= sem.Wait(0) == R::kTimeout && sem.Wait(2) == R::kTimeout && sem.count() == 0;
  // A blocked waiter takes a released unit as a grant: the count stays 0, and the previous count reported is 0.
  std::atomic<bool> back{false};
  std::thread w([&] {
    if (sem.Wait(-1) != R::kSuccess) std::printf("limits: the waiter failed\n");
    back = true;
  });
  while (sem.blocked() != 1) std::this_thread::yield();
  ok &= sem.Release(1, &prev) && prev == 0;
  w.join();
  ok &= back.load() && sem.count() == 0 && sem.blocked() == 0 && sem.grants() == 0;
  std::printf("limits (maximum, previous count, zero waits, a grant): %s\n", ok ? "ok" : "FAILED");
  return ok;
}

static bool Interrupts() {
  Semaphore sem(0, 10);
  std::atomic<bool> flag{false};
  int slow = 0;
  double worst = 0;
  for (int i = 0; i < 1000; ++i) {
    flag = false;
    std::atomic<bool> back{false};
    std::thread w([&] {
      if (sem.Wait(-1, &flag) != R::kTimeout) std::printf("interrupt: woke with success?\n");
      back = true;
    });
    while (sem.blocked() != 1) std::this_thread::yield();
    const auto raised = steady_clock::now();
    flag = true;
    sem.Interrupt();
    while (!back.load()) std::this_thread::yield();
    const double ms = duration<double, std::milli>(steady_clock::now() - raised).count();
    worst = ms > worst ? ms : worst;
    if (ms > 50) ++slow;
    w.join();
  }
  std::printf("interrupts: 1000, worst %.2f ms, %d over 50 ms (the 100 ms net): %s\n", worst, slow,
              slow ? "FAILED" : "ok");
  return slow == 0;
}

static bool RandomMix(int C, int P, bool sleeps) {
  constexpr uint32_t kMax = 64;
  Semaphore sem(0, kMax);
  std::atomic<long long> released{0}, refused{0}, taken{0}, timeouts{0}, interrupts{0};
  std::atomic<bool> stop_producers{false}, stop{false};
  std::vector<std::atomic<bool>> flags(C);
  for (auto& f : flags) f = false;
  std::vector<std::thread> threads;
  for (int c = 0; c < C; ++c)
    threads.emplace_back([&, c] {
      std::mt19937 rng(c * 7919 + 1);
      while (!stop.load()) {
        const int pick = rng() % 4;
        const int t = pick == 0 ? -1 : pick == 1 ? 0 : int(rng() % 3);
        const R r = sem.Wait(t, &flags[c]);
        if (r == R::kSuccess)
          taken.fetch_add(1);
        else if (flags[c].exchange(false))
          interrupts.fetch_add(1);
        else
          timeouts.fetch_add(1);
      }
    });
  for (int p = 0; p < P; ++p)
    threads.emplace_back([&, p] {
      std::mt19937 rng(p * 104729 + 3);
      while (!stop_producers.load()) {
        const int a = rng() % 100;
        if (a < 75) {
          const uint32_t n = 1 + rng() % 3;
          if (sem.Release(n))
            released.fetch_add(n);
          else
            refused.fetch_add(1);
        } else if (a < 82) {
          flags[rng() % C] = true;
          sem.Interrupt();
        } else if (sleeps) {
          std::this_thread::sleep_for(microseconds(rng() % 200));
        }
        if (rng() % 8 == 0) std::this_thread::yield();
      }
    });
  std::this_thread::sleep_for(seconds(10));
  stop_producers = true;
  for (int p = 0; p < P; ++p) threads[C + p].join();
  stop = true;
  const auto end = steady_clock::now() + seconds(5);
  long long extra = 0;
  while (sem.blocked() != 0 && steady_clock::now() < end) {
    if (sem.Release(1)) ++extra;  // lets the consumers out
    std::this_thread::sleep_for(milliseconds(1));
  }
  for (int c = 0; c < C; ++c) threads[c].join();
  const bool counters = sem.blocked() == 0 && sem.grants() == 0;
  const long long left = sem.count();
  const bool conserve = released.load() + extra == taken.load() + left;
  std::printf("random mix (%d consumers, %d producers%s): %lld units released (+%lld to drain, %lld releases refused at "
              "the maximum), %lld taken, %lld left, %lld timeouts, %lld interrupts; counters %s, units conserved %s\n",
              C, P, sleeps ? ", sleeps" : "", released.load(), extra, refused.load(), taken.load(), left,
              timeouts.load(), interrupts.load(), counters ? "0" : "NOT 0", conserve ? "yes" : "NO");
  return counters && conserve;
}

int main() {
  bool ok = true;
  ok &= PingPong();
  ok &= MultiRelease(true);
  ok &= MultiRelease(false);
  ok &= Limits();
  ok &= Interrupts();
  ok &= RandomMix(6, 3, true);
  ok &= RandomMix(16, 4, false);
  ok &= RandomMix(3, 1, false);
  const uint64_t safety = AddressEventCore<WinArbiter>::safety_releases();
  std::printf("grants > blocked or count > maximum seen: %d; released at a safety re-check without a wake: %llu\n",
              Semaphore::violations.load(), (unsigned long long)safety);
  ok &= Semaphore::violations.load() == 0;
  std::printf("%s\n", ok ? "ALL OK" : "FAILED");
  return ok ? 0 : 1;
}
