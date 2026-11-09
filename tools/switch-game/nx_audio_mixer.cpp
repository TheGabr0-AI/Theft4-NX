// switch-game: the guest mixer's voice loop, sub_8216DBB0, as native NEON (nx_audio_mixer.h). Both callers in the
// generated code (gta4_recomp.2.cpp, return addresses 8216D9D0 and 8216DB14) call sub_8216DBB0 instead of
// __imp__sub_8216DBB0 since run 51, so this strong definition replaces the weak alias for them and for the function
// table.
// Run 64: their caller, the routing function sub_8216D7E0 (nx_audio_routes.h). The generated code reaches it only
// through the function table (no direct call), which holds the weak alias: this strong definition replaces it, with
// no change to the generated code.

#include "nx_audio_mixer.h"
#include "nx_audio_routes.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/memory/guest_table.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(nx_audio_neon_mixer, true, "Switch",
                    "Run 51: the guest audio mixer's voice loop (sub_8216DBB0) as native NEON instead of the "
                    "recompiled VMX code; false = the generated code");
REXCVAR_DEFINE_BOOL(nx_audio_native_routes, true, "Switch",
                    "Run 64: the guest audio mixer's routing function (sub_8216D7E0: the gain ramps and the "
                    "overwrite loop around the voice loop) as native code; false = the generated code. Needs "
                    "nx_audio_neon_mixer");

extern "C" void __imp__sub_8216DBB0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216D7E0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_821686F8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_829A1260(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_829A0E58(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__KeRaiseIrqlToDpcLevel(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__KeAcquireSpinLockAtRaisedIrql(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__KeReleaseSpinLockFromRaisedIrql(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__KfLowerIrql(PPCContext& ctx, uint8_t* base);

namespace theft4::nx_audio_mixer {
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_route_calls{0};
uint64_t Calls() { return g_calls.load(std::memory_order_relaxed); }
uint64_t RouteCalls() { return g_route_calls.load(std::memory_order_relaxed); }
}  // namespace theft4::nx_audio_mixer

extern "C" void sub_8216DBB0(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(nx_audio_neon_mixer)) {
    __imp__sub_8216DBB0(ctx, base);
    return;
  }
  // The generated body sets flush-to-zero before its first vector add (vaddfp).
  ctx.fpscr.enableFlushMode();
  theft4::nx_audio_mixer::MixVoice([](uint32_t ea) { return reinterpret_cast<uint8_t*>(rex::memory::GuestToHost(ea)); },
                                   ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  theft4::nx_audio_mixer::g_calls.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void sub_8216D7E0(PPCContext& ctx, uint8_t* base) {
  // The generated body calls sub_8216DBB0 above, which honours its own cvar: with that one off, the whole generated
  // function runs.
  if (!REXCVAR_GET(nx_audio_native_routes) || !REXCVAR_GET(nx_audio_neon_mixer)) {
    __imp__sub_8216D7E0(ctx, base);
    return;
  }
  static uint8_t* guest_base = nullptr;  // the generated functions' second argument (unused on the Switch's table)
  guest_base = base;
  static const theft4::nx_audio_mixer::RouteGuest guest = {
      [](PPCContext& c) { __imp__sub_821686F8(c, guest_base); },
      [](PPCContext& c) { __imp__sub_829A1260(c, guest_base); },
      [](PPCContext& c) { __imp__sub_829A0E58(c, guest_base); },
      [](PPCContext& c) { __imp__KeRaiseIrqlToDpcLevel(c, guest_base); },
      [](PPCContext& c) { __imp__KeAcquireSpinLockAtRaisedIrql(c, guest_base); },
      [](PPCContext& c) { __imp__KeReleaseSpinLockFromRaisedIrql(c, guest_base); },
      [](PPCContext& c) { __imp__KfLowerIrql(c, guest_base); },
  };
  theft4::nx_audio_mixer::MixRoutes(
      ctx, [](uint32_t ea) { return reinterpret_cast<uint8_t*>(rex::memory::GuestToHost(ea)); }, guest);
  theft4::nx_audio_mixer::g_route_calls.fetch_add(1, std::memory_order_relaxed);
}
