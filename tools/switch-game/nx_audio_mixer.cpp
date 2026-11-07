// switch-game: the guest mixer's voice loop, sub_8216DBB0, as native NEON (nx_audio_mixer.h). Both callers in the
// generated code (gta4_recomp.2.cpp, return addresses 8216D9D0 and 8216DB14) call sub_8216DBB0 instead of
// __imp__sub_8216DBB0 since run 51, so this strong definition replaces the weak alias for them and for the function
// table.

#include "nx_audio_mixer.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/memory/guest_table.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(nx_audio_neon_mixer, true, "Switch",
                    "Run 51: the guest audio mixer's voice loop (sub_8216DBB0) as native NEON instead of the "
                    "recompiled VMX code; false = the generated code");

extern "C" void __imp__sub_8216DBB0(PPCContext& ctx, uint8_t* base);

namespace theft4::nx_audio_mixer {
std::atomic<uint64_t> g_calls{0};
uint64_t Calls() { return g_calls.load(std::memory_order_relaxed); }
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
