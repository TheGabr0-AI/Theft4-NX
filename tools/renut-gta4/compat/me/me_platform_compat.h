// theft4: the Switch thread helpers Renut's renderer calls (sa2-nx's src/me/me_platform_compat.h): on the console the
// few of sa2-nx's SDK threading_switch.h the renderer uses, written on libnx in gta4_renut_compat.cpp (this SDK has no
// threading_switch.h); no-ops on the PC. switch.h is not included here (its Thread, Mutex, Event... collide with the
// SDK's names).
#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
extern "C" {
// The calling thread's priority (0 highest .. 63); whether the kernel accepted it.
bool RexSwitchSetCurrentThreadPriorityOk(int priority);
// The calling thread pinned to one core of the process's mask (preferred core and the only one allowed).
bool RexSwitchPinCurrentThreadToCore(int core);
// The core the calling thread runs on now.
int RexSwitchCurrentCore(void);
}
// sa2-nx: SA2's SDK has no unpin, so the worker keeps its core (the same here).
inline bool RexSwitchUnpinCurrentThread(int) { return false; }
#else
inline bool RexSwitchSetCurrentThreadPriorityOk(int) { return true; }
inline int RexSwitchCurrentCore() { return -1; }
inline bool RexSwitchSetCurrentThreadCore(int) { return true; }
inline bool RexSwitchPinCurrentThreadToCore(int) { return true; }
inline bool RexSwitchUnpinCurrentThread(int) { return true; }
#endif
