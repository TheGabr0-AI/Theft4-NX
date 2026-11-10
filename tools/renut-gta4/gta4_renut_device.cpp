// ReNut on GTA IV, step R1: the game's Direct3D without a GPU (docs/switch-port/18-renut-migration.md).
//
// The design is ReNut's (reNut by masterspike52, as banjo-nx and sa2-nx carry it: renut_engine/gpu/d3d/hooks/
// device.cpp), written here for GTA IV's Direct3D 2.0.6274 from the European base's own code (gta4_d3d.h):
//   - Direct3D_CreateDevice builds the device through the library's own no-hardware path (device type 2), then gives
//     the title device what the hardware path gives it apart from the GPU;
//   - the command ring is a sink: KickOff and the segment functions rewind it, nothing reads it;
//   - a fence completes when its batch is kicked (R1 has no render thread: at once);
//   - Swap does the original's bookkeeping.
// The state setters, the resources and the device work of the draws, Clear and Resolve stay the game's own code: they
// keep the device's shadow state, which a renderer reads at each draw. R1 draws nothing: it proves the hook set and the
// device layout by running the game on it and reading the state of its draws (the 10 s line).
//
// gta4_renderer = "xenos" (default): every hook runs the original function.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "gta4_init.h"

#include "gta4_d3d.h"
#include "gta4_renut_select.h"

REXCVAR_DEFINE_STRING(gta4_renderer, "xenos", "Renderer",
                      "xenos = the emulated GPU (the reference); renut = ReNut's Direct3D-level path (step R1: "
                      "the game's Direct3D without a GPU, no picture yet)");

#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
#endif

namespace theft4::renut {

#if defined(_WIN32)
namespace {
// PC test aid: the first access violations with the faulting instruction as an offset into the executable (the link
// map of the THEFT4_RENUT build names the function), written at once to renut-faults.txt. The runtime's own handlers
// run after this one and are not changed.
LONG CALLBACK LogFault(EXCEPTION_POINTERS* info) {
  static std::atomic<int> count{0};
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && count.fetch_add(1) < 16) {
    if (FILE* f = std::fopen("renut-faults.txt", "a")) {
      const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      const auto rip = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
      std::fprintf(f, "access violation: rip exe+%llX, %s address %llX, thread %lu\n",
                   static_cast<unsigned long long>(rip - module), r->ExceptionInformation[0] ? "write" : "read",
                   static_cast<unsigned long long>(r->ExceptionInformation[1]), GetCurrentThreadId());
      std::fclose(f);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace
#endif

bool Selected() {
  static const bool selected = [] {
    const bool renut = REXCVAR_GET(gta4_renderer) == "renut";
#if defined(_WIN32)
    if (renut)
      AddVectoredExceptionHandler(1, LogFault);
#endif
    return renut;
  }();
  return selected;
}

namespace {

using namespace d3d;

constexpr uint32_t kRingSinkSize = 1u << 20;
// Room past the limit for the library's inline writers, which check the limit before a packet and not after it.
constexpr uint32_t kRingSinkSlack = 64u << 10;
constexpr uint32_t kRingDataSize = 1u << 20;
constexpr uint32_t kE_OUTOFMEMORY = 0x8007000Eu;

uint32_t g_ring_sink = 0;
uint32_t g_ring_data = 0;
std::atomic<uint32_t> g_ring_data_offset{0};

struct Counters {
  std::atomic<uint64_t> frames{0}, kicks{0}, fence_waits{0}, draws{0}, indexed_draws{0}, begin_vertices{0}, clears{0},
      resolves{0}, inline_allocations{0}, segments{0}, indirect_blocks{0}, tilings{0};
};
Counters g_count;
std::atomic<uint32_t> g_title_device{0};

uint8_t* Ptr(uint8_t* base, uint32_t address) {
  return base + address + REX_PHYS_HOST_OFFSET(address);
}
uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Ptr(base, address), 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Ptr(base, address), &value, 4);
}
uint8_t Load8(uint8_t* base, uint32_t address) {
  return *Ptr(base, address);
}
void Store8(uint8_t* base, uint32_t address, uint8_t value) {
  *Ptr(base, address) = value;
}

// A guest call with up to three arguments; the result is r3.
template <typename Fn>
uint32_t Call(Fn fn, PPCContext& ctx, uint8_t* base, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
  ctx.r3.u64 = a;
  ctx.r4.u64 = b;
  ctx.r5.u64 = c;
  fn(ctx, base);
  return ctx.r3.u32;
}

void RewindRing(uint8_t* base, uint32_t device) {
  if (!g_ring_sink)
    return;
  const uint32_t limit = g_ring_sink + kRingSinkSize - kRingSinkSlack;
  Store32(base, device + kRing, g_ring_sink);
  Store32(base, device + kRingLimit, limit);
  Store32(base, device + kRingGuarantee, limit - 160);  // as sub_829D74C0 places it
  Store8(base, device + kFlags1, Load8(base, device + kFlags1) & uint8_t(~kFlag1OutOfCommandMemory));
}

// The current fence covers the work issued so far and the device moves on to the next. With no render thread the
// work is "read" at once, so the fence is complete.
void KickFence(uint8_t* base, uint32_t device) {
  const uint32_t fence = Load32(base, device + kFenceTag);
  if (const uint32_t completion = Load32(base, device + kFenceCompletion))
    Store32(base, completion, fence);
  Store32(base, device + kFenceTag, fence + 2);
}

bool FencePending(uint8_t* base, uint32_t device, uint32_t fence) {
  const uint32_t tag = Load32(base, device + kFenceTag);
  const uint32_t completion = Load32(base, device + kFenceCompletion);
  return completion && tag - fence < tag - Load32(base, completion);
}

uint32_t RingData(uint32_t bytes, uint32_t alignment) {
  if (!g_ring_data || bytes > kRingDataSize / 2)
    return 0;
  if (alignment < 4 || (alignment & (alignment - 1)))
    alignment = 4;
  uint32_t offset = g_ring_data_offset.fetch_add(bytes + alignment, std::memory_order_relaxed);
  offset %= kRingDataSize / 2;
  offset = (offset + alignment - 1) & ~(alignment - 1);
  return g_ring_data + offset;
}

// Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, D3DPRESENT_PARAMETERS*, D3DDevice**).
void CreateDevice(PPCContext& ctx, uint8_t* base) {
  const uint32_t device_type = ctx.r4.u32;
  uint32_t behavior_flags = ctx.r6.u32;
  const uint32_t present_parameters = ctx.r7.u32;
  const uint32_t out_va = ctx.r8.u32;
  const bool title_device = device_type != kDeviceTypeNoHardware;
  const PPCContext saved = ctx;

  if (!g_ring_sink)
    g_ring_sink = Call(__imp__sub_8218BF20, ctx, base, kRingSinkSize, kPhysicalAllocFlags);
  if (!g_ring_data)
    g_ring_data = Call(__imp__sub_8218BF20, ctx, base, kRingDataSize, kPhysicalAllocFlags);
  const uint32_t fence_va = Call(__imp__sub_8218BF20, ctx, base, 0x80 + 0x20, kPhysicalAllocFlags);
  if (!g_ring_sink || !g_ring_data || !fence_va) {
    REXLOG_ERROR("renut: Direct3D_CreateDevice: guest allocation failed");
    ctx = saved;
    ctx.r3.u64 = kE_OUTOFMEMORY;
    return;
  }
  std::memset(Ptr(base, fence_va), 0, 0x80 + 0x20);

  // The library's own device without hardware.
  ctx = saved;
  ctx.r4.u64 = kDeviceTypeNoHardware;
  __imp__sub_829D0218(ctx, base);
  if (ctx.r3.u32 != 0)
    return;
  const uint32_t device = Load32(base, out_va);
  if (!device) {
    ctx.r3.u64 = kE_OUTOFMEMORY;
    return;
  }
  // Its ring was the side buffer, marked out of command memory (sub_829D74C0): from here the sink.
  Store32(base, device + kFenceCompletion, fence_va);
  Store32(base, device + kFenceScratch, fence_va + 0x80);
  RewindRing(base, device);

  if (title_device) {
    // What the hardware path (sub_829DF3F0) gives the title device apart from the GPU, in its order. Left out: the
    // ring (sub_829D8868), the worker threads (sub_829DE0C0),
    // the notification routines (sub_829DC610), the EDRAM retraining (sub_829E1998).
    Call(__imp__RtlInitializeCriticalSection, ctx, base, device + kCriticalSection0);
    Call(__imp__RtlInitializeCriticalSection, ctx, base, device + kCriticalSection1);
    Store8(base, device + kFlags2, Load8(base, device + kFlags2) | 0x04);
    // The library's graphics interrupt handler (sub_829DF308 registers it: VdSetGraphicsInterruptCallback). The
    // vertical blank's work (sub_829D4BF8: the blank count, the pending flips, the title's blank callback) is the
    // game's clock: without it the render thread waited for ever after its first scene frame (PC, 2026-11-10).
    if (!Call(__imp__sub_829DF308, ctx, base, device))
      REXLOG_ERROR("renut: the graphics interrupt set-up (sub_829DF308) failed");
    if (const uint32_t global = Load32(base, kVdGlobalDeviceImportSlot))
      Store32(base, global, device);
    if (!(behavior_flags & 0x100) && !(behavior_flags & 0x3F000000))
      behavior_flags |= 0x0C000000;
    Store32(base, device + kBehaviorFlags, behavior_flags);
    if (!Call(__imp__sub_829D0B80, ctx, base, device))
      REXLOG_ERROR("renut: the device's prebuilt commands (sub_829D0B80) failed");
    Store32(base, device + kMinusOneB, 0xFFFFFFFFu);
    Store32(base, device + kMinusOneC, 0xFFFFFFFFu);
    if (present_parameters && !Call(__imp__sub_829DEF28, ctx, base, device, present_parameters)) {
      REXLOG_ERROR("renut: InitializePresentationParameters (sub_829DEF28) failed");
      ctx = saved;
      ctx.r3.u64 = kE_OUTOFMEMORY;
      return;
    }
    Call(__imp__sub_829DECC0, ctx, base, device);
    __imp__KeQueryPerformanceFrequency(ctx, base);
    const float frequency = float(ctx.r3.u32);
    Store32(base, device + kPerfFrequency, std::bit_cast<uint32_t>(frequency));
    Store32(base, device + kPerfFrequency + 4, std::bit_cast<uint32_t>(frequency > 0.0f ? 1.0f / frequency : 0.0f));
    Call(__imp__sub_829CA608, ctx, base, device, 0);
    for (uint32_t i = 0; i < kTitleMinusOneWords; ++i)
      Store32(base, device + kTitleMinusOne + 4 * i, 0xFFFFFFFFu);
    RewindRing(base, device);
    g_title_device.store(device, std::memory_order_release);
  }
  REXLOG_INFO("renut: guest Direct3D device (type {}) at {:08X}, {} bytes, ring sink {:08X}, behaviour flags {:08X}",
              device_type, device, kDeviceAllocSize, g_ring_sink, behavior_flags);
  ctx = saved;
  Store32(base, out_va, device);
  ctx.r3.u64 = 0;  // S_OK
}

// The state a renderer reads at a draw, through gta4_d3d.h: logged for the first draws and once every 10 s.
void DescribeDraw(uint8_t* base, uint32_t device, const char* kind, uint64_t ordinal) {
  const uint32_t target = Load32(base, device + kRenderTargets);
  const uint32_t depth = Load32(base, device + kDepthStencil);
  const uint32_t size = target ? Load32(base, target + kSurfaceSize) : 0;
  REXLOG_INFO("renut: {} #{}: vertex shader {:08X}, pixel shader {:08X}, declaration {:08X}, index buffer {:08X}, "
              "stream 0 {:08X} (stride {}), texture 0 {:08X}, target 0 {:08X} ({}x{}), depth {:08X}",
              kind, ordinal, Load32(base, device + kVertexShader), Load32(base, device + kPixelShader),
              Load32(base, device + kVertexDeclaration), Load32(base, device + kIndexBuffer),
              Load32(base, device + kStreamSources), Load8(base, device + kStreamStrides) * 4,
              Load32(base, device + kTextures), target, target ? ((size >> 18) & 0x3FFF) + 1 : 0,
              target ? ((size >> 3) & 0x7FFF) + 1 : 0, depth);
}

std::atomic<int64_t> g_next_report_ms{0};
std::atomic<bool> g_describe_next{true};

void FrameDone() {
  const uint64_t frames = g_count.frames.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t next = g_next_report_ms.load(std::memory_order_relaxed);
  if (next == 0) {
    g_next_report_ms.store(now + 10000, std::memory_order_relaxed);
    return;
  }
  if (now < next || !g_next_report_ms.compare_exchange_strong(next, now + 10000))
    return;
  static uint64_t last_frames = 0, last_draws = 0;
  const uint64_t draws = g_count.draws + g_count.indexed_draws + g_count.begin_vertices;
  REXLOG_INFO("renut R1: {} frames ({} in this window, {} draws a frame); totals: {} draws ({} indexed, {} inline), "
              "{} BeginTiling, {} resolves, {} kicks, {} fence waits, {} inline allocations, {} segments, {} command "
              "blocks submitted, {} EndTiling tails",
              frames, frames - last_frames,
              frames > last_frames ? (draws - last_draws) / (frames - last_frames) : 0, draws,
              g_count.indexed_draws.load(), g_count.begin_vertices.load(), g_count.clears.load(),
              g_count.resolves.load(), g_count.kicks.load(), g_count.fence_waits.load(),
              g_count.inline_allocations.load(), g_count.segments.load(), g_count.indirect_blocks.load(),
              g_count.tilings.load());
  last_frames = frames;
  last_draws = draws;
  g_describe_next.store(true, std::memory_order_relaxed);
}

void CountDraw(uint8_t* base, uint32_t device, std::atomic<uint64_t>& counter, const char* kind) {
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 3 || g_describe_next.exchange(false, std::memory_order_relaxed))
    DescribeDraw(base, device, kind, n);
}

}  // namespace
}  // namespace theft4::renut

using namespace theft4::renut;
using namespace theft4::renut::d3d;

#define THEFT4_RENUT_HOOK(name) extern "C" void name(PPCContext& __restrict ctx, uint8_t* base)
#define THEFT4_RENUT_YIELD(name) \
  if (!Selected()) {             \
    __imp__##name(ctx, base);    \
    return;                      \
  }

THEFT4_RENUT_HOOK(sub_829D0218) {  // Direct3D_CreateDevice
  THEFT4_RENUT_YIELD(sub_829D0218)
  CreateDevice(ctx, base);
}

THEFT4_RENUT_HOOK(sub_829D8518) {  // PRING CDevice::KickOff()
  THEFT4_RENUT_YIELD(sub_829D8518)
  const uint32_t device = ctx.r3.u32;
  g_count.kicks.fetch_add(1, std::memory_order_relaxed);
  if (!Load32(base, device + kInKickOff))
    KickFence(base, device);
  RewindRing(base, device);
  ctx.r3.u64 = Load32(base, device + kRing);
}

THEFT4_RENUT_HOOK(sub_829D8730) {  // PRING CDevice::BeginRingBig(DWORD Count)
  THEFT4_RENUT_YIELD(sub_829D8730)
  const uint32_t device = ctx.r3.u32, count = ctx.r4.u32;
  if (Load32(base, device + kRing) + count * 4 > Load32(base, device + kRingLimit))
    RewindRing(base, device);
  ctx.r3.u64 = Load32(base, device + kRing);
}

THEFT4_RENUT_HOOK(sub_829D8138) {  // the ring segment allocator (CDevice*, DWORD Dwords): non-zero, the callers retry
  THEFT4_RENUT_YIELD(sub_829D8138)
  const uint32_t device = ctx.r3.u32;
  g_count.segments.fetch_add(1, std::memory_order_relaxed);
  RewindRing(base, device);
  ctx.r3.u64 = Load32(base, device + kRing) + 4;
}

THEFT4_RENUT_HOOK(sub_829D8390) {  // KickOff's tail: the next segment
  THEFT4_RENUT_YIELD(sub_829D8390)
  RewindRing(base, ctx.r3.u32);
}

THEFT4_RENUT_HOOK(sub_829D74E8) {  // void* RingBufferDeviceAllocate(CDevice*, DWORD Flags, DWORD* pSize, DWORD Alignment)
  THEFT4_RENUT_YIELD(sub_829D74E8)
  g_count.inline_allocations.fetch_add(1, std::memory_order_relaxed);
  const uint32_t size_va = ctx.r5.u32;
  ctx.r3.u64 = size_va ? RingData(Load32(base, size_va), ctx.r6.u32) : 0;
}

THEFT4_RENUT_HOOK(sub_829D76F0) {  // void* CDevice::AllocateInlineData(DWORD Dwords, DWORD Alignment)
  THEFT4_RENUT_YIELD(sub_829D76F0)
  g_count.inline_allocations.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = RingData(ctx.r4.u32 * 4, ctx.r5.u32);
}

THEFT4_RENUT_HOOK(sub_829D7D40) {  // void CDevice::BlockOnFence(DWORD Fence, CBlocker::Type, BOOL NoKick)
  THEFT4_RENUT_YIELD(sub_829D7D40)
  const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
  if (!fence || !FencePending(base, device, fence))
    return;
  g_count.fence_waits.fetch_add(1, std::memory_order_relaxed);
  // As the original: only the current fence needs a kick, and not while a KickOff is in progress. Every kicked
  // fence is complete (KickFence).
  if (fence == Load32(base, device + kFenceTag) && !Load32(base, device + kInKickOff)) {
    KickFence(base, device);
    RewindRing(base, device);
  }
}

// The submission of a command block to the GPU (it writes the ring registers). sub_829D8290 reaches it for the
// title device, whose ring set-up (sub_829D8868) is left out: the block is the GPU's, nothing here reads it.
THEFT4_RENUT_HOOK(sub_829D77D8) {
  THEFT4_RENUT_YIELD(sub_829D77D8)
  g_count.indirect_blocks.fetch_add(1, std::memory_order_relaxed);
}

THEFT4_RENUT_HOOK(sub_829CFE80) {  // BOOL CBlocker::Check()
  THEFT4_RENUT_YIELD(sub_829CFE80)
  ctx.r3.u64 = 0;
}

THEFT4_RENUT_HOOK(sub_829D5338) {  // void D3DDevice_Swap(D3DDevice*, D3DBaseTexture* pFrontBuffer, const D3DVIDEO_SCALER_PARAMETERS*)
  THEFT4_RENUT_YIELD(sub_829D5338)
  const uint32_t device = ctx.r3.u32;
  // The original's bookkeeping: count the swap, end the frame with a KickOff, record the frame's fence, clear the
  // pending flag, everything dirty for the next frame. The flip count is the vertical blank interrupt's: every swap
  // has flipped.
  const uint32_t swaps = Load32(base, device + kSwapCount) + 1;
  Store32(base, device + kSwapCount, swaps);
  Store32(base, device + kFlipCount, swaps);
  const uint32_t fence = Load32(base, device + kFenceTag);
  KickFence(base, device);
  RewindRing(base, device);
  Store32(base, device + kPresentFenceTag, fence);
  Store8(base, device + kFlags1, Load8(base, device + kFlags1) & uint8_t(~kFlag1SwapPending));
  Call(__imp__sub_829CA608, ctx, base, device, 0);
  FrameDone();
}

THEFT4_RENUT_HOOK(sub_829CCAC0) {  // D3DDevice_DrawVertices
  if (Selected())
    CountDraw(base, ctx.r3.u32, g_count.draws, "DrawVertices");
  __imp__sub_829CCAC0(ctx, base);
}

THEFT4_RENUT_HOOK(sub_829CCEA8) {  // D3DDevice_DrawIndexedVertices
  if (Selected())
    CountDraw(base, ctx.r3.u32, g_count.indexed_draws, "DrawIndexedVertices");
  __imp__sub_829CCEA8(ctx, base);
}

THEFT4_RENUT_HOOK(sub_829CC610) {  // D3DDevice_BeginVertices
  if (Selected())
    CountDraw(base, ctx.r3.u32, g_count.begin_vertices, "BeginVertices");
  __imp__sub_829CC610(ctx, base);
}

THEFT4_RENUT_HOOK(sub_829CD470) {  // D3DDevice_BeginTiling (the old renderer's notes called it Clear)
  if (Selected())
    g_count.clears.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_829CD470(ctx, base);
}

// BOOL EndTiling's tail (CDevice*, DWORD TileCount, DWORD Processors): the end of D3DDevice_EndTiling sub_829CD908.
// The original closes the segment the tiles' commands were recorded in, hands it to the GPU as a command block
// (sub_829D7C68: one more pending block at kPendingBlocks) behind a ring callback (sub_829DDD30) that wakes D3D's
// worker thread, which replays the block once per tile and takes the pending block off again. Without a GPU nothing
// takes it off: the next wait for an idle GPU (sub_829D8798) spun for ever, which was R1's stall at the first frame
// of the 3D scene. A renderer at the Direct3D level draws the tiled frame once at full size (ReNut's EndTiling does
// the same: "left out: the tiles' ring segment, D3D's WorkerDpc callback that replays it per tile"), so the tail
// keeps its device work and leaves the block out: the tiling words cleared, the segment lists ended, the batch's
// fence. The result is the original's: non-zero when the command memory ran out while the tiles were recorded.
THEFT4_RENUT_HOOK(sub_829DDF28) {
  THEFT4_RENUT_YIELD(sub_829DDF28)
  const uint32_t device = ctx.r3.u32;
  g_count.tilings.fetch_add(1, std::memory_order_relaxed);
  const uint8_t flags1 = Load8(base, device + kFlags1);
  Store32(base, device + kInKickOff, 0);
  Store32(base, device + kTileSegmentWords, 0);
  if (flags1 & kFlag1OutOfCommandMemory) {
    RewindRing(base, device);  // clears the flag, as the original after sub_829D6FD8
    ctx.r3.u64 = 1;
    return;
  }
  for (uint32_t i = 0; i < kSegmentListCount; ++i) {
    const uint32_t list = device + kSegmentLists + i * kSegmentListSize;
    Store32(base, list + 12, 0);  // sub_829D73D0 without the jump words a GPU would follow
  }
  KickFence(base, device);
  RewindRing(base, device);
  ctx.r3.u64 = 0;
}

THEFT4_RENUT_HOOK(sub_829CB7C8) {  // D3DDevice_Resolve
  if (Selected())
    g_count.resolves.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_829CB7C8(ctx, base);
}
