// switch-game: Liberty Recompiled's native hooks on the Switch (docs/switch-port/16-deko3d-devlog.md, M3 B).
//
// Forced into the hook sources only (CMakeLists.txt). The hooks reach guest memory through one function,
// GuestPointer(base, address) = base + address + REX_PHYS_HOST_OFFSET(address), written for aliased views (the PC,
// Android). The Switch has none: every guest address goes through rex::memory::GuestToHost (guest_table.h). The
// offset is defined here so that the sum is exactly GuestToHost(address); `base` is GuestPointer's own parameter.

#pragma once

#include <cstdint>

#include <rex/memory/guest_table.h>

#define REX_PHYS_HOST_OFFSET(address)                                                   \
  (reinterpret_cast<uintptr_t>(rex::memory::GuestToHost(uint32_t(address))) -        \
   reinterpret_cast<uintptr_t>(base) - uintptr_t(address))
