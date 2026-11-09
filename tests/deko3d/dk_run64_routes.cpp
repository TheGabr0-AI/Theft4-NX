// Offline probe of run 64's audio change (devlog section AL), compiled by tests/deko3d/test_run64.py.
//
// ROUTES: tools/switch-game/nx_audio_routes.h (the native routing function) against the generated functions
// themselves: test_run64.py extracts the bodies of sub_8216D7E0 and sub_8216DBB0 from the local
// gta4_recomp.2.cpp into run64_generated.inc (game-derived code: never in the repository), and this file compiles
// them with the real generated header and macros. Both run on the same guest memory (a 4 GiB reservation, the
// objects, tables, buffers, constants and the lock at their guest addresses) over random mix objects: with and
// without a route table, the three values of the object's byte 26, source channels past the source's count,
// destinations past the destination's, a source that is the destination or overlaps it, null descriptors, the lock
// free, held by this thread or by another, a format match that fails, gains from denormal to huge, random
// constants. The guest's other functions are stubs that log their arguments and return address.
// After each run: every byte of guest memory outside the dead stack frame, r1, r3, the flush mode and the call log
// must be equal.
// SPEED: the two on one mix object of 8 routes (this PC, simde's NEON on SSE: not the console).

#define REX_RECOMPILED_TU
#include "gta4_init.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "nx_audio_routes.h"

// The generated bodies define __imp__<name> only (no alias: the callers below name what they call).
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) REX_EXTERN(__imp__##name)

namespace {

struct Call {
  uint32_t id, r3, r4, r5, lr;
  bool operator==(const Call&) const = default;
};
std::vector<Call> g_log;
int32_t g_match_result = 0;
uint32_t g_irql = 0;

// `arguments`: how many of r3, r4, r5 the function takes (the others hold what the caller left there).
void Log(uint32_t id, const PPCContext& ctx, uint32_t arguments) {
  g_log.push_back({id, arguments > 0 ? ctx.r3.u32 : 0, arguments > 1 ? ctx.r4.u32 : 0, arguments > 2 ? ctx.r5.u32 : 0,
                   uint32_t(ctx.lr)});
}

}  // namespace

extern "C" void __imp__sub_821686F8(PPCContext& ctx, uint8_t*) {
  Log(1, ctx, 3);
  ctx.r3.s64 = g_match_result;
}
extern "C" void __imp__sub_829A1260(PPCContext& ctx, uint8_t* base) {
  Log(2, ctx, 3);
  std::memset(base + ctx.r3.u32, int(ctx.r4.u32), ctx.r5.u32);
}
extern "C" void __imp__sub_829A0E58(PPCContext& ctx, uint8_t* base) {
  Log(3, ctx, 3);
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}
extern "C" void __imp__KeRaiseIrqlToDpcLevel(PPCContext& ctx, uint8_t*) {
  Log(4, ctx, 0);
  ctx.r3.u64 = g_irql;
}
extern "C" void __imp__KeAcquireSpinLockAtRaisedIrql(PPCContext& ctx, uint8_t*) { Log(5, ctx, 1); }
extern "C" void __imp__KeReleaseSpinLockFromRaisedIrql(PPCContext& ctx, uint8_t*) { Log(6, ctx, 1); }
extern "C" void __imp__KfLowerIrql(PPCContext& ctx, uint8_t*) { Log(7, ctx, 1); }
// The two call sites of the generated routing function (hand-patched in run 51 to the overridable name).
extern "C" void sub_8216DBB0(PPCContext& ctx, uint8_t* base) { __imp__sub_8216DBB0(ctx, base); }

#include "run64_generated.inc"

namespace {

using theft4::nx_audio_mixer::kRouteConstants;
using theft4::nx_audio_mixer::kRouteLock;

uint8_t* g_base = nullptr;

struct Range {
  uint32_t start, bytes;
};
// Committed guest memory: page 0 (null descriptors), the objects, the buffers, the stack, the constants, the lock.
constexpr uint32_t kObjects = 0x40010000u, kBuffers = 0x40100000u, kStackTop = 0x70010000u;
const Range kRanges[] = {{0x00000000u, 0x1000u},  {kObjects, 0x4000u},     {kBuffers, 0x8000u},
                         {0x70000000u, 0x10000u}, {0x820A0000u, 0x10000u}, {0x83147000u, 0x1000u}};

void W8(uint32_t ea, uint32_t v) { g_base[ea] = uint8_t(v); }
void W32(uint32_t ea, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(g_base + ea, &v, 4);
}
void WF(uint32_t ea, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  W32(ea, u);
}

std::vector<uint8_t> Snapshot() {
  std::vector<uint8_t> s;
  for (const Range& r : kRanges) s.insert(s.end(), g_base + r.start, g_base + r.start + r.bytes);
  return s;
}
void Restore(const std::vector<uint8_t>& s) {
  size_t at = 0;
  for (const Range& r : kRanges) {
    std::memcpy(g_base + r.start, s.data() + at, r.bytes);
    at += r.bytes;
  }
}

const theft4::nx_audio_mixer::RouteGuest kGuest = {
    [](PPCContext& c) { __imp__sub_821686F8(c, g_base); },
    [](PPCContext& c) { __imp__sub_829A1260(c, g_base); },
    [](PPCContext& c) { __imp__sub_829A0E58(c, g_base); },
    [](PPCContext& c) { __imp__KeRaiseIrqlToDpcLevel(c, g_base); },
    [](PPCContext& c) { __imp__KeAcquireSpinLockAtRaisedIrql(c, g_base); },
    [](PPCContext& c) { __imp__KeReleaseSpinLockFromRaisedIrql(c, g_base); },
    [](PPCContext& c) { __imp__KfLowerIrql(c, g_base); },
};

void Native(PPCContext& ctx) {
  theft4::nx_audio_mixer::MixRoutes(ctx, [](uint32_t ea) { return g_base + ea; }, kGuest);
}

float Gain(std::mt19937& rng) {
  switch (rng() % 12) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return 1e-41f * float(rng() % 100);                  // denormal
    case 3: return 3e-38f * float(int(rng() % 9) - 4);            // around the smallest normal
    case 4: return 1e30f * float(int(rng() % 9) - 4);
    default: return float(int(rng() % 4001) - 2000) / 1000.0f;
  }
}

}  // namespace

int main() {
  g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100002000ull, MEM_RESERVE, PAGE_NOACCESS));
  if (!g_base) {
    std::printf("ROUTES no address space\n");
    return 2;
  }
  for (const Range& r : kRanges) {
    if (!VirtualAlloc(g_base + r.start, r.bytes, MEM_COMMIT, PAGE_READWRITE)) {
      std::printf("ROUTES no memory\n");
      return 2;
    }
  }
  std::mt19937 rng(64);
  const uint32_t self = kObjects + 0x100, table = kObjects + 0x400, source = kObjects + 0x1008, target = kObjects + 0x2008;
  const uint32_t thread = 0x7FFE0000u;
  uint64_t rounds = 0, errors = 0, memory_errors = 0, log_errors = 0, register_errors = 0;
  uint64_t by_kind[8] = {};
  PPCContext start{};
  start.fpscr.InitHost();
  for (int round = 0; round < 6000; ++round) {
    for (const Range& r : kRanges) {
      for (uint32_t i = 0; i < r.bytes; i += 4) {
        const uint32_t v = rng();
        std::memcpy(g_base + r.start + i, &v, 4);
      }
    }
    // The constants: the title's (lane offsets 0 1 2 3, 1/256 a sample, 4 samples a vector) or random ones.
    const bool title_constants = round % 5 != 0;
    WF(kRouteConstants - 7996, title_constants ? 0.0f : Gain(rng));
    WF(kRouteConstants - 7984, title_constants ? 1.0f : Gain(rng));
    WF(kRouteConstants - 10000, title_constants ? 2.0f : Gain(rng));
    WF(kRouteConstants - 31616, title_constants ? 3.0f : Gain(rng));
    WF(kRouteConstants - 6592, title_constants ? 1.0f / 256.0f : Gain(rng));
    WF(kRouteConstants - 32404, title_constants ? 4.0f : Gain(rng));
    // Buffers: floats of a sound's size, some denormal; up to 8 channels of 1,024 bytes; the source at the
    // destination, apart from it, or overlapping it by part of a channel.
    for (uint32_t i = 0; i < 0x8000; i += 4) {
      WF(kBuffers + i, rng() % 50 == 0 ? 1e-40f * float(rng() % 1000) : float(int(rng() % 20001) - 10000) / 10000.0f);
    }
    const uint32_t target_base = kBuffers + (rng() % 4) * 16;
    const uint32_t placement = rng() % 6;
    const uint32_t source_base = placement == 0   ? target_base
                                 : placement == 1 ? target_base + 16 * (1 + rng() % 40)
                                 : placement == 2 ? target_base + 1024 * (rng() % 3) + 8
                                                  : kBuffers + 0x4000 + (rng() % 4) * 16;
    const uint32_t source_channels = rng() % 5, target_channels = rng() % 5;
    const uint32_t routes = rng() % 3 == 0 ? 0 : 1 + rng() % 7;
    const uint32_t mode = rng() % 3;
    W8(self + 16, routes);
    W32(self + 20, table);
    W8(self + 25, target_channels);
    W8(self + 26, mode);
    for (uint32_t r = 0; r < 8; ++r) {
      W8(table + r * 12 + 0, rng() % 6);
      W8(table + r * 12 + 1, rng() % 6);
      WF(table + r * 12 + 4, Gain(rng));
      WF(table + r * 12 + 8, Gain(rng));
    }
    W8(source - 8 + 21, source_channels);
    W32(source - 8 + 28, source_base);
    W8(target - 8 + 21, target_channels);
    W32(target - 8 + 28, target_base);
    // Null descriptors read page 0.
    W8(21, rng() % 3);
    W32(28, kBuffers + 0x4000);
    const uint32_t descriptors = rng() % 16;
    const uint32_t r4 = descriptors == 0 ? 0 : (descriptors == 1 ? target : source);
    const uint32_t r5 = descriptors == 2 ? 0 : target;
    const uint32_t lock_state = rng() % 4;  // free, held by this thread, held by another, a count without an owner
    W32(kRouteLock + 4, lock_state == 0 ? 0 : 1 + rng() % 3);
    W32(kRouteLock + 8, lock_state == 1 ? thread : (lock_state == 2 ? thread + 0x100 : 0));
    g_match_result = rng() % 10 == 0 ? -int32_t(rng() % 1000 + 1) : int32_t(rng() % 3);
    g_irql = rng() % 3;
    ++by_kind[(routes ? 0 : 4) + (mode ? 1 : 0) + (placement < 3 ? 2 : 0)];

    PPCContext ctx = start;
    ctx.r1.u64 = kStackTop - 0x400;
    ctx.r3.u64 = self;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r13.u64 = thread;
    ctx.lr = 0x82174400;
    if (rng() % 2) ctx.fpscr.enableFlushModeUnconditional(); else ctx.fpscr.disableFlushModeUnconditional();
    const std::vector<uint8_t> before = Snapshot();

    PPCContext a = ctx;
    g_log.clear();
    a.fpscr.setcsr(a.fpscr.csr);
    __imp__sub_8216D7E0(a, g_base);
    const std::vector<Call> log_a = g_log;
    std::vector<uint8_t> memory_a = Snapshot();

    Restore(before);
    PPCContext b = ctx;
    g_log.clear();
    b.fpscr.setcsr(b.fpscr.csr);
    Native(b);
    std::vector<uint8_t> memory_b = Snapshot();

    // The frame below the caller's stack pointer is dead after the return: not compared.
    size_t stack_at = 0;
    for (const Range& r : kRanges) {
      if (r.start == 0x70000000u) break;
      stack_at += r.bytes;
    }
    // (And below that frame: the generated sub_8216DBB0 saves r31 at -8(r1), under the routing function's frame.)
    const size_t frame = stack_at + (ctx.r1.u32 - 272 - 0x70000000u);
    std::memset(memory_a.data() + frame - 256, 0, 272 + 256);
    std::memset(memory_b.data() + frame - 256, 0, 272 + 256);
    const bool memory_ok = memory_a == memory_b;
    const bool log_ok = log_a == g_log;
    const bool registers_ok = a.r1.u64 == b.r1.u64 && a.r3.u64 == b.r3.u64 && a.fpscr.csr == b.fpscr.csr;
    ++rounds;
    memory_errors += !memory_ok;
    log_errors += !log_ok;
    register_errors += !registers_ok;
    if (!memory_ok && errors < 5) {
      size_t at = 0;
      while (memory_a[at] == memory_b[at]) ++at;
      uint32_t guest = 0;
      size_t skip = at;
      for (const Range& r : kRanges) {
        if (skip < r.bytes) {
          guest = r.start + uint32_t(skip);
          break;
        }
        skip -= r.bytes;
      }
      std::printf("ROUTES round %d: first different byte at guest %08X (generated %02X, native %02X)\n", round, guest,
                  memory_a[at], memory_b[at]);
    }
    if (!(memory_ok && log_ok && registers_ok) && errors++ < 5) {
      std::printf("ROUTES round %d differs: memory %d log %d (%zu/%zu calls) registers %d; routes %u mode %u "
                  "placement %u channels %u/%u\n",
                  round, memory_ok, log_ok, log_a.size(), g_log.size(), registers_ok, routes, mode, placement,
                  source_channels, target_channels);
    }
  }
  std::printf("ROUTES rounds %llu errors %llu (memory %llu, calls %llu, registers %llu); with a table %llu (overwrite "
              "%llu, overlapping %llu), without %llu\n",
              (unsigned long long)rounds, (unsigned long long)(memory_errors + log_errors + register_errors),
              (unsigned long long)memory_errors, (unsigned long long)log_errors, (unsigned long long)register_errors,
              (unsigned long long)(by_kind[0] + by_kind[1] + by_kind[2] + by_kind[3]),
              (unsigned long long)(by_kind[1] + by_kind[3]), (unsigned long long)(by_kind[2] + by_kind[3]),
              (unsigned long long)(by_kind[4] + by_kind[5] + by_kind[6] + by_kind[7]));

  // SPEED: 8 routes of 4 source channels into 4 destination channels, adding (byte 26 = 0) then overwriting (2).
  for (uint32_t mode = 0; mode <= 2; mode += 2) {
    for (const Range& r : kRanges) std::memset(g_base + r.start, 0, r.bytes);
    WF(kRouteConstants - 7996, 0.0f);
    WF(kRouteConstants - 7984, 1.0f);
    WF(kRouteConstants - 10000, 2.0f);
    WF(kRouteConstants - 31616, 3.0f);
    WF(kRouteConstants - 6592, 1.0f / 256.0f);
    WF(kRouteConstants - 32404, 4.0f);
    W8(self + 16, 8);
    W32(self + 20, table);
    W8(self + 25, 4);
    W8(self + 26, mode);
    for (uint32_t r = 0; r < 8; ++r) {
      W8(table + r * 12 + 0, r % 4);
      W8(table + r * 12 + 1, (r + r / 4) % 4);
      WF(table + r * 12 + 4, 0.25f);
      WF(table + r * 12 + 8, 0.75f);
    }
    W8(source - 8 + 21, 4);
    W32(source - 8 + 28, kBuffers + 0x4000);
    W8(target - 8 + 21, 4);
    W32(target - 8 + 28, kBuffers);
    for (uint32_t i = 0; i < 0x1000; i += 4) WF(kBuffers + 0x4000 + i, float(i % 97) / 97.0f);
    double ns[2] = {};
    for (int which = 0; which < 2; ++which) {
      double best = 1e30;
      for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < 20000; ++n) {
          PPCContext ctx = start;
          ctx.r1.u64 = kStackTop - 0x400;
          ctx.r3.u64 = self;
          ctx.r4.u64 = source;
          ctx.r5.u64 = target;
          ctx.r13.u64 = thread;
          g_log.clear();
          if (which == 0) __imp__sub_8216D7E0(ctx, g_base); else Native(ctx);
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / 20000.0);
      }
      ns[which] = best;
    }
    std::printf("SPEED %s generated_ns %.0f native_ns %.0f ratio %.2f\n", mode ? "overwrite" : "add", ns[0], ns[1],
                ns[0] / ns[1]);
  }
  return (memory_errors + log_errors + register_errors) ? 1 : 0;
}
