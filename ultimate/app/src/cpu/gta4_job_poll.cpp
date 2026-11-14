// Theft4-NX (2026-11-13, docs/switch-port/20 section 2.14): GTA IV's job workers poll their job's queue without ever
// giving up the core.
//
// sub_828E2350 is the loop of each of the game's three job worker threads (run U8-B's stack samples: more than half of
// their active samples in it and in the spinlock it takes, about 45 % of a core in all, on the three cores the game
// shares with the audio mixer and the main thread). Each pass takes the spinlock at 0x8312F23C (sub_82221358: a
// compare-and-swap, then a counting delay of 8 to 1,024 steps, never a yield), claims up to *0x82A939D4 items from the
// shared index at 0x83130250 against the job's total (*(*(ctx+20)+8)), releases the lock, runs the claimed items
// (sub_828E1F68) and goes round again until the job's byte +12 says it is over. With nothing left to claim a worker goes
// straight back to the lock: three of them do, on three cores the 360 gave six hardware threads.
//
// This hook is the generated function's own statements (app/generated/renut_recomp.140.cpp, same macros and the same
// compile definitions: BANJO_HOT_SOURCES and the CPU rounds' definitions, app/CMakeLists.txt), so every guest load,
// store, call and register is as the original makes them, plus one host call: when a pass claimed nothing and the job is
// still open, rex::thread::MaybeYield() (the SDK's polling-wait policy, switch_yield_backoff: a yield, then 100 us sleeps
// for a thread that keeps polling). The guest sees only time pass, as when its thread is not scheduled. The function is
// reached only through the function table (no generated code calls it directly), so this strong definition replaces the
// weak sub_828E2350 at link time and no generated file changes. gta4_job_poll_yield = false: the original loop.
#include "renut_pch.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/thread.h>

REXCVAR_DEFINE_BOOL(gta4_job_poll_yield, true, "Theft4/CPU",
                    "GTA IV's job workers (sub_828E2350): a pass that claims no item of a job still open gives up the "
                    "core (rex::thread::MaybeYield, switch_yield_backoff) before polling again; false: the original "
                    "loop, which polls without pause")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828E1F68);
REX_EXTERN(__imp__sub_82221358);

#if defined(__SWITCH__)
// Theft4-NX (2026-11-14, U9): the SDK's semaphores (ultimate/sdk/src/core/threading_posix.cpp, threading_posix.cpp is
// the Switch's threading only); named in the same start-up line, so a console log says which wait the run had.
REXCVAR_DECLARE(bool, thread_semaphore_address_wait);
#endif

namespace {
std::atomic<bool> g_logged{false};

bool YieldOn() {
  static const bool on = REXCVAR_GET(gta4_job_poll_yield);
  if (!g_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[gta4] job workers' poll loop (sub_828E2350): {}", on ? "an idle pass yields (gta4_job_poll_yield)"
                                                                       : "the original loop (gta4_job_poll_yield off)");
#if defined(__SWITCH__)
    REXLOG_INFO("[gta4] semaphores: {}", REXCVAR_GET(thread_semaphore_address_wait)
                                             ? "address waits, a release hands its units over (thread_semaphore_address_wait)"
                                             : "the condition variable (thread_semaphore_address_wait off)");
#endif
  }
  return on;
}
}  // namespace

REX_HOOK_RAW(sub_828E2350) {
	REX_FUNC_PROLOGUE();
	REX_R1_LOCAL();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	uint32_t ea{};
	const bool yield = YieldOn();
	REX_DEAD_DEF(r12, ctx.r12.u64 = ctx.lr);
	ctx.lr = 0x828E2358;
	ea = -144 + r1l.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, r1l.u32));
	r1l.u32 = ea; REX_R1_SYNC();
	r25.s64 = 0;
	r29.u64 = REX_LOAD_D_U32(ctx.r3.u32, 68);
	ctx.r8.s64 = -2095906816;
	ctx.r11.u64 = r25.u64;
	ctx.r10.u64 = r25.u64;
	ctx.r9.s64 = 1;
	r26.s64 = -2102853632;
	r27.s64 = -2095906816;
	r28.s64 = ctx.r8.s64 + -3524;
loc_828E2380:
	REX_LOOP_BARRIER();
	ctx.r9.u64 = ctx.r9.u32 & 0xFF;
	cr6.compare<uint32_t>(ctx.r9.u32, 0, xer);
	if (!cr6.eq) goto loc_828E2394;
	cr6.compare<int32_t>(ctx.r10.s32, 0, xer);
	if (!cr6.gt) goto loc_828E242C;
loc_828E2394:
	ctx.r9.u64 = REX_LOAD_D_U32(r29.u32, 20);
	ctx.r8.u64 = ((uint32_t)(ctx.r11.u32 << 1));
	cr6.compare<int32_t>(ctx.r10.s32, 0, xer);
	ctx.r11.u64 = ctx.r11.u64 + ctx.r8.u64;
	ctx.r11.u64 = ((uint32_t)(ctx.r11.u32 << 4));
	ctx.r9.u64 = REX_LOAD_D_U32(ctx.r9.u32, 0);
	ctx.r11.u64 = ctx.r11.u64 + ctx.r9.u64;
	if (!cr6.gt) goto loc_828E23E0;
	r31.s64 = ctx.r11.s64 + 8;
	r30.u64 = ctx.r10.u64;
loc_828E23BC:
	REX_LOOP_BARRIER();
	ctx.r3.u64 = r29.u64;
	ctx.r6.u64 = REX_LOAD_D_U32(r31.u32, 8);
	ctx.r5.u64 = REX_LOAD_D_U32(r31.u32, 0);
	ctx.r4.u64 = REX_LOAD_D_U32(r31.u32, -8);
	ctx.lr = 0x828E23D0;
	__imp__sub_828E1F68(ctx, base); REX_R1_ASSUMED();
	r30.s64 = r30.s64 + -1;
	r31.s64 = r31.s64 + 48;
	cr6.compare<uint32_t>(r30.u32, 0, xer);
	if (!cr6.eq) goto loc_828E23BC;
loc_828E23E0:
	ctx.r3.u64 = r28.u64;
	ctx.lr = 0x828E23E8;
	__imp__sub_82221358(ctx, base); REX_R1_KEEP();
	ctx.r10.u64 = REX_LOAD_D_U32(r29.u32, 20);
	ctx.r11.u64 = REX_LOAD_D_U32(r27.u32, 592);
	ctx.r9.u64 = REX_LOAD_D_U32(r26.u32, 14804);
	ctx.r10.u64 = REX_LOAD_D_U32(ctx.r10.u32, 8);
	ctx.r10.u64 = ctx.r10.u64 - ctx.r11.u64;
	cr6.compare<int32_t>(ctx.r10.s32, ctx.r9.s32, xer);
	if (cr6.lt) goto loc_828E2408;
	ctx.r10.u64 = ctx.r9.u64;
loc_828E2408:
	ctx.r9.u64 = ctx.r11.u64 + ctx.r10.u64;
	REX_STORE_D_U32_C(r27.u32, 592, ctx.r9.u32);
	std::atomic_thread_fence(std::memory_order_acq_rel);
	REX_STORE_D_U32_C(r28.u32, 0, r25.u32);
	ctx.r9.u64 = REX_LOAD_D_U32(r29.u32, 20);
	ctx.r9.u64 = REX_LOAD_D_U8(ctx.r9.u32, 12);
	ctx.r9.u64 = ctx.r9.u32 == 0 ? 32 : __builtin_clz(ctx.r9.u32);
	ctx.r9.u64 = ((ctx.r9.u32 >> 5) & 0x1u);
	// Theft4-NX: the only addition. Nothing claimed (r10 <= 0) and the job still open (r9 = 1, its byte +12 zero): the next
	// pass would only take the lock again. Give the core to whoever has work first (the lock is released above).
	if (yield && ctx.r10.s32 <= 0 && ctx.r9.u32 != 0)
		rex::thread::MaybeYield();
	goto loc_828E2380;
loc_828E242C:
	r1l.s64 = r1l.s64 + 144; REX_R1_SYNC();
	return;
}
