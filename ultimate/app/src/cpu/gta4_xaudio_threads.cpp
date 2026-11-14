// Theft4-NX (2026-11-14, U11, docs/switch-port/20 section 2.17): GTA IV's XAudio engine threads at the priority the
// game asks for.
//
// The game's XAudio library starts its processing threads in sub_82169578: ExCreateThread with the entry 0x82169400 for
// the supervisor (it waits for the render driver callback's event, runs one pass of the voice graph and signals the
// end; console run U10-A: 79 % of a core, 84 % of its samples in the seven DSP functions) and 0x821694C8 for the
// others, then KeSetBasePriorityThread(thread, 15): time critical, as the Xbox 360 runs them. The base runs every guest
// thread at one host priority (0x3B, ignore_thread_priorities), which Horizon time-slices in 10 ms slices: the
// supervisor, woken for each 256-sample frame, waits for a slice behind the game's other threads on three saturated
// cores (U10-A: it waits 18-28 % of the time while 34 % of the frames are missing). The render driver callback waits
// for it (sub_82169B00, KeWaitForMultipleObjects without a timeout), so each frame costs its mixing plus that wait.
//
// With gta4_xaudio_thread_priority set (0x2D: banjo-nx's priority for XAudio's mixer threads in its four-core mode,
// below the audio output 0x2A, the Audio Worker and XMA Decoder 0x2B, the presentation 0x2C), each of these threads
// takes that priority when it starts: above the game threads' band, so it runs until it waits instead of sharing
// slices. The guest code is untouched: the hooks run the generated entry functions unchanged. -1 (the default): as
// before. Both entries are reached only through ExCreateThread (no generated code calls them), so these strong
// definitions replace the weak generated symbols at link time.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_INT32(gta4_xaudio_thread_priority, -1, "Theft4/CPU",
                     "GTA IV's XAudio engine threads (entries 0x82169400 and 0x821694C8, which the game asks to run at "
                     "time-critical priority) at this host priority when they start (Switch: 0x2D runs them above the "
                     "game threads' time-sliced 0x3B, below the audio output and the Audio Worker); -1: the base's "
                     "single guest priority, as before")
    .range(-1, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82169400);
REX_EXTERN(__imp__sub_821694C8);

#if REX_PLATFORM_SWITCH
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);
#endif

namespace {
void RaiseXAudioThread(const char* which) {
  static const int priority = REXCVAR_GET(gta4_xaudio_thread_priority);
  if (priority < 0) {
    REXLOG_INFO("[gta4] XAudio {} thread at the guest threads' priority (gta4_xaudio_thread_priority -1)", which);
    return;
  }
#if REX_PLATFORM_SWITCH
  const bool ok = RexSwitchSetCurrentThreadPriorityOk(priority);
  REXLOG_INFO("[gta4] XAudio {} thread at priority 0x{:X} (gta4_xaudio_thread_priority): {}", which, priority,
              ok ? "set" : "REFUSED by the kernel");
#else
  REXLOG_INFO("[gta4] XAudio {} thread: gta4_xaudio_thread_priority 0x{:X} applies on the Switch only", which, priority);
#endif
}
}  // namespace

REX_HOOK_RAW(sub_82169400) {
  RaiseXAudioThread("supervisor");
  __imp__sub_82169400(ctx, base);
}

REX_HOOK_RAW(sub_821694C8) {
  RaiseXAudioThread("peon");
  __imp__sub_821694C8(ctx, base);
}
