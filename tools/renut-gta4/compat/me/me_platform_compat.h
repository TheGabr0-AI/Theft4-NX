// theft4: the Switch thread helpers Renut's renderer calls (sa2-nx's src/me/me_platform_compat.h): the SDK's on the
// console (threading_switch.h), no-ops on the PC.
#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH
#include <threading_switch.h>
inline bool RexSwitchUnpinCurrentThread(int) { return false; }
#else
inline bool RexSwitchSetCurrentThreadPriorityOk(int) { return true; }
inline int RexSwitchCurrentCore() { return -1; }
inline bool RexSwitchSetCurrentThreadCore(int) { return true; }
inline bool RexSwitchPinCurrentThreadToCore(int) { return true; }
inline bool RexSwitchUnpinCurrentThread(int) { return true; }
#endif
