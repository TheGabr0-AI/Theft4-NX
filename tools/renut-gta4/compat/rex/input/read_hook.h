// theft4: sa2-nx's SDK calls a hook at each controller read (its sdk/include/rex/input/read_hook.h; Renut's late
// input uses it). This SDK (v0.9) has no such call yet: the hook is kept and not called, so late input does nothing.
#pragma once

namespace rex::input {

using ReadHook = void (*)();
void SetReadHook(ReadHook hook);

}  // namespace rex::input
