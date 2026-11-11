// theft4: sa2-nx's enhancement keys as Renut's renderer uses them (thirdparty/renut/renut_engine: sa2_renut_pacing.cpp
// and sa2_compat/cpu/banjo_enhancements.h). sa2-nx's own file is its application's (src/cpu/sa2_enhancements.h): two
// masters that switch every CPU / non-CPU enhancement, a use counter per key and a 10 s report. Here the masters are
// two cvars of this port (gta4_renut_cpu_enhancements / gta4_renut_noncpu_enhancements, tools/renut-gta4/
// gta4_renut_compat.cpp) and a key is its own cvar; the report is not ported yet.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

namespace sa2::enh {

enum Group : uint8_t { kCpu = 0, kNonCpu = 1 };

struct Enhancement {
  const char* key;
  Group group;
  const char* what;
  bool (*own)();  // reads the enhancement's own cvar
  bool active = false;
  std::atomic<uint64_t> uses{0};
  std::atomic<uint64_t> amount{0};
  const char* amount_unit = nullptr;
  void Use(uint64_t n = 1) { uses.fetch_add(n, std::memory_order_relaxed); }
  std::atomic<uint32_t> sampled{0};
  void UseSampled() {
    const uint32_t n = sampled.load(std::memory_order_relaxed) + 1;
    sampled.store(n, std::memory_order_relaxed);
    if ((n & 255) == 0) Use(256);
  }
  void Add(uint64_t n) { amount.fetch_add(n, std::memory_order_relaxed); }
};

bool Register(Enhancement* e);
// After the configuration is read: every registered key's `active`.
void Resolve();
bool CpuMaster();
bool NonCpuMaster();
void StartReport();
void VerifyCheck(uint64_t n = 1);
void VerifyDifference();
bool VerifyMode();

}  // namespace sa2::enh

#define SA2_ENHANCEMENT(VAR, KEY, GROUP, DEFAULT, DESC)                                                     \
  REXCVAR_DEFINE_BOOL(KEY, DEFAULT, "GTA IV enhancements", DESC).lifecycle(rex::cvar::Lifecycle::kInitOnly); \
  ::sa2::enh::Enhancement VAR{#KEY, GROUP, DESC, [] { return REXCVAR_GET(KEY); }};                         \
  [[maybe_unused]] static const bool VAR##_registered_ = ::sa2::enh::Register(&VAR)
