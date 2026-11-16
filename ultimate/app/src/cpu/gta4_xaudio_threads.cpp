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

// Theft4-NX (2026-11-16, U17, docs/switch-port/20 section 2.23): the supervisor on core 3. U16-A on the console: in the
// street the threads allowed on cores 0-2 asked for ~357 % of those cores' 300 % (supervisor ~105 %, GTA IV's render
// thread ~50 %, Renut's render thread ~60 %, main thread ~39 %), while core 3, granted to the process (mask 0xF), stayed
// unused (banjo_system_core 0). kabba536's GTA IV port runs its XAudio2 thread on core 3 ("~85 % of a core taken off
// cores 0-2", its switch_core3_thread_entry, by default not strict: it prefers core 3 and may migrate). 1: that
// (preferred core 3, the process's four cores allowed: Horizon may run it elsewhere when core 3 is taken by the
// system); 2: core 3 only (it can wait behind the system's threads there); 0: as before. A change of placement only:
// the guest code and the audio it mixes are the same.
REXCVAR_DEFINE_INT32(gta4_xaudio_supervisor_core3, 0, "Theft4/CPU",
                     "GTA IV's XAudio supervisor thread (entry 0x82169400, the mixer: ~1 core) on core 3 when the "
                     "process has it (Switch): 1 = prefers core 3, may run on the others; 2 = core 3 only; 0 = as before")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82169400);
REX_EXTERN(__imp__sub_821694C8);

#if REX_PLATFORM_SWITCH
#include <switch.h>
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);
// The SDK wraps svcSetThreadCoreMask (sdk/src/core/threading_switch.cpp): with banjo_system_core 0 or 1 it takes core 3
// out of every mask of more than one core. Mode 1 needs that mask as asked, so it calls the real one.
extern "C" Result __real_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask);
#endif

namespace {
void MoveSupervisorToCore3() {
  static const int mode = REXCVAR_GET(gta4_xaudio_supervisor_core3);
  if (mode == 0)
    return;
#if REX_PLATFORM_SWITCH
  u64 process_mask = 0;
  if (R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !(process_mask & 0x8)) {
    REXLOG_WARN("[gta4] XAudio supervisor: core 3 is not the process's (mask 0x{:X}): left where it is "
                "(gta4_xaudio_supervisor_core3 {})", process_mask, mode);
    return;
  }
  const u32 mask = mode == 2 ? 0x8u : u32(process_mask & 0xF);
  const Result rc = __real_svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, mask);
  s32 preferred = -1;
  u64 now_mask = 0;
  svcGetThreadCoreMask(&preferred, &now_mask, CUR_THREAD_HANDLE);
  REXLOG_INFO("[gta4] XAudio supervisor on core 3 (gta4_xaudio_supervisor_core3 {}: {}): result 0x{:X}, now preferred "
              "core {} mask 0x{:X}, running on core {}", mode, mode == 2 ? "core 3 only" : "prefers core 3, may migrate",
              rc, preferred, now_mask, svcGetCurrentProcessorNumber());
#else
  REXLOG_INFO("[gta4] XAudio supervisor: gta4_xaudio_supervisor_core3 {} applies on the Switch only", mode);
#endif
}

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
  MoveSupervisorToCore3();  // theft4-nx (U17)
  __imp__sub_82169400(ctx, base);
}

REX_HOOK_RAW(sub_821694C8) {
  RaiseXAudioThread("peon");
  __imp__sub_821694C8(ctx, base);
}
