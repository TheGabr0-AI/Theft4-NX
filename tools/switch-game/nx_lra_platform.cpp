// switch-game: the Switch services Liberty Recompiled's renderer calls (nx_gpu = lra, run 51), declared in
// gta4_native/native_switch_platform.h. Same names and meaning as in kabba536's port (src/core/threading_switch.cpp,
// src/ui/switch_perf.cpp), written for our threads.

#include <atomic>
#include <cstdint>

#include <switch.h>

namespace theft4::nx_lra {
std::atomic<uint64_t> g_perf_counts[4];
std::atomic<uint32_t> g_raised_guest_threads{0};
uint64_t FramesPublished() { return g_perf_counts[0].load(std::memory_order_relaxed); }
}  // namespace theft4::nx_lra

extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority) {
  return R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority));
}

extern "C" bool RexSwitchExcludeCurrentThreadFromCore(int core) {
  s32 preferred = 0;
  u64 mask = 0;
  if (core < 0 || core > 63 || R_FAILED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE))) {
    return false;
  }
  const u64 without = mask & ~(u64(1) << core);
  if (!without || without == mask) {
    return false;
  }
  // The preferred core must stay inside the mask: -3 keeps it if it does, else take the lowest core left.
  const s32 keep = (preferred >= 0 && (without >> preferred) & 1) ? preferred : __builtin_ctzll(without);
  return R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, keep, without));
}

extern "C" void RexSwitchMarkRaisedGuestThread(void) {
  theft4::nx_lra::g_raised_guest_threads.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void RexSwitchPerfCount(unsigned id) {
  if (id < 4) {
    theft4::nx_lra::g_perf_counts[id].fetch_add(1, std::memory_order_relaxed);
  }
}
