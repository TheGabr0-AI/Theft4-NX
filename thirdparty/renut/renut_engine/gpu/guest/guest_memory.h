/**
 * @file    gpu/guest/guest_memory.h
 * @brief   Guest virtual address translation for the renderer.
 */
#pragma once

#include <atomic>

#include <rex/memory/guest_table.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace renut::gpu {

namespace detail {
// banjo-nx (2026-10-19, render-thread proposal 11): the guest memory bases, read once (they never change after
// Memory::Initialize): one load at a translation instead of the kernel state -> memory -> base chain (603 sites).
inline std::atomic<u8 *> g_guest_vbase{nullptr};
inline std::atomic<u8 *> g_guest_pbase{nullptr};
[[gnu::noinline, gnu::cold]] inline void LoadGuestBases() {
  rex::memory::Memory *memory = REX_KERNEL_MEMORY();
  g_guest_vbase.store(memory->virtual_membase(), std::memory_order_relaxed);
  g_guest_pbase.store(memory->physical_membase(), std::memory_order_relaxed);
}
inline u8 *GuestPhysicalBase() {
  u8 *base = g_guest_pbase.load(std::memory_order_relaxed);
  if (!base) [[unlikely]] {
    LoadGuestBases();
    base = g_guest_pbase.load(std::memory_order_relaxed);
  }
  return base;
}
inline u8 *GuestVirtualBase() {
  u8 *base = g_guest_vbase.load(std::memory_order_relaxed);
  if (!base) [[unlikely]] {
    LoadGuestBases();
    base = g_guest_vbase.load(std::memory_order_relaxed);
  }
  return base;
}
} // namespace detail

// Host pointer for a guest VA, or nullptr for VA 0.
template <typename T>
inline T *GuestPtr(u32 guest_address) {
  if (!guest_address)
    return nullptr;
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(__aarch64__))
  return REX_KERNEL_MEMORY()->TranslateVirtual<T *>(guest_address);  // heaps with host offsets there
#elif REX_PLATFORM_NX
  // theft4 (run 66): this SDK's Switch arena is not a linear 4 GB window (rex/memory/guest_table.h, memory design A):
  // only 0x00000000-0x8FFFFFFF are virtual_membase + VA; the physical views (0x7F, 0xA0-0xFF, 0xE0 with +0x1000) and
  // the 0x90 mirror go through the 16 MB slice table, as the generated code and TranslateVirtual do on NX. The plain
  // sum below put the ring sink (VA 0xBFAA0000) past the arena's end: Direct3D_CreateDevice's memset faulted.
  return reinterpret_cast<T *>(rex::memory::GuestToHost(guest_address));
#else
  return reinterpret_cast<T *>(detail::GuestVirtualBase() + guest_address);  // TranslateVirtual's sum (no offsets)
#endif
}

// Host pointer for a GPU physical address (fetch constants, index buffer
// addresses): the low 29 bits index physical memory.
template <typename T>
inline T *GuestPhysicalPtr(u32 physical_address) {
  return reinterpret_cast<T *>(detail::GuestPhysicalBase() + (physical_address & 0x1FFFFFFF));  // TranslatePhysical
}

} // namespace renut::gpu
