#pragma once

// Runtime switches for the Switch synchronisation fast paths (C2). They are
// plain relaxed atomics so the A/B schedule can flip them while the game runs;
// every path they select is correct on its own, they only differ in cost.
// All start off (the original paths); switch-game sets them from the nx_sync_*
// cvars before the runtime exists.

#include <atomic>

namespace rex::thread::nx_sync {

// Object table and dispatcher-header lookups without the global critical
// region (lookups only; every table update still takes the lock). Objects are
// freed after a grace period so a concurrent lookup never touches freed memory.
inline std::atomic<bool>& FastObjectLookup() {
  static std::atomic<bool> value{false};
  return value;
}

// Wake one waiter instead of all of them when only one can proceed: auto-reset
// events and timers, mutants, and semaphore releases of one count.
inline std::atomic<bool>& NotifyOne() {
  static std::atomic<bool> value{false};
  return value;
}

// Guest mfmsr/mtmsrd interrupt lock tracked per thread instead of on the
// global critical region (system/ppc_generated.cpp).
inline std::atomic<bool>& GuestInterruptLockFree() {
  static std::atomic<bool> value{false};
  return value;
}

// C5: semaphore releases and event signals without taking the object mutex
// unless a thread may be asleep on it, and wake-ups sent after the mutex is
// released (rex/thread/nx_multi_wait.h).
inline std::atomic<bool>& LockFreeRelease() {
  static std::atomic<bool> value{false};
  return value;
}

// Multi-object waits subscribe to the objects they wait on and sleep until
// one of them changes (rex/thread/nx_multi_wait.h); off: the original loop
// that polls every object once a millisecond.
inline std::atomic<bool>& SubscribedMultiWait() {
  static std::atomic<bool> value{false};
  return value;
}

}  // namespace rex::thread::nx_sync
