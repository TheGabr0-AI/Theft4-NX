// The Switch services graphics_system.cpp's Switch code calls (run 51, 16-deko3d-devlog.md section Y).
//
// kabba536's port declares them in its own SDK (src/core/threading_switch.h, src/ui/switch_perf.cpp); ours has
// neither file, so switch-game defines them (tools/switch-game/nx_lra_platform.cpp) with the same names and meaning.

#pragma once

#include <cstdint>

extern "C" {
// svcSetThreadPriority of the current thread; false when Horizon refuses it.
bool RexSwitchSetCurrentThreadPriorityOk(int priority);
// The current thread's core mask without `core` (kept when it is the only core left); false when nothing changed.
bool RexSwitchExcludeCurrentThreadFromCore(int core);
// kabba's threading turns the yields of a raised guest thread into short sleeps; ours does not raise guest threads
// (gta4_native_producer_thread_priority = 0 in game.toml), so this only counts.
void RexSwitchMarkRaisedGuestThread(void);
// Counter `id` (0 = frames published by the renderer), read by switch-game's heartbeat.
void RexSwitchPerfCount(unsigned id);
}
