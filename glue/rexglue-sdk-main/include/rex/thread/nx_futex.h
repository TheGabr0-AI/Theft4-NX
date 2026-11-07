// Run 56: a futex on a 32-bit word (docs/switch-port/16-deko3d-devlog.md, section AD).
//
// Horizon's address arbiter is a futex: svcWaitForAddress(ArbitrationType_WaitIfEqual) sleeps only while the word still
// holds the value the caller saw (checked in the kernel, so a wake-up between the caller's look and its sleep is never
// lost), svcSignalToAddress(SignalType_Signal) wakes up to `count` sleepers (-1: all). No mutex on either side.
// Elsewhere (the PC tests compile this header): the same semantics from a striped table of mutexes and condition
// variables, the word compared under the stripe's lock.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <condition_variable>
#include <mutex>
#endif

namespace rex::thread::nx_futex {

#if !defined(__SWITCH__)
struct Stripe {
  std::mutex mutex;
  std::condition_variable cv;
};
inline Stripe& StripeOf(const void* word) {
  static Stripe stripes[64];
  return stripes[(reinterpret_cast<uintptr_t>(word) >> 2) % 64];
}
#endif

// Sleeps while *word == expected, at most timeout_ns (negative: no limit). Returns at once if the word differs; may
// return early (spuriously): the caller looks again.
inline void Wait(std::atomic<uint32_t>* word, uint32_t expected, int64_t timeout_ns) {
  static_assert(sizeof(std::atomic<uint32_t>) == 4, "the arbiter takes a 32-bit word");
#if defined(__SWITCH__)
  svcWaitForAddress(reinterpret_cast<void*>(word), ArbitrationType_WaitIfEqual, int32_t(expected),
                    timeout_ns < 0 ? INT64_MAX : timeout_ns);
#else
  Stripe& s = StripeOf(word);
  std::unique_lock<std::mutex> lock(s.mutex);
  if (word->load(std::memory_order_seq_cst) != expected) {
    return;
  }
  if (timeout_ns < 0) {
    s.cv.wait(lock);
  } else {
    s.cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns));
  }
#endif
}

// Wakes up to `count` sleepers on the word (-1: all). The caller changed the word before (seq_cst).
inline void Wake(std::atomic<uint32_t>* word, int32_t count) {
#if defined(__SWITCH__)
  svcSignalToAddress(reinterpret_cast<void*>(word), SignalType_Signal, 0, count);
#else
  (void)count;  // the stripe's waiters may be of other words: all of them look again
  Stripe& s = StripeOf(word);
  { std::lock_guard<std::mutex> lock(s.mutex); }
  s.cv.notify_all();
#endif
}

}  // namespace rex::thread::nx_futex
