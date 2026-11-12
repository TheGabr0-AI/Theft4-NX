/**
 * @file    gpu/backend/queued.cpp
 */
#include "renut_engine/gpu/backend/queued.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"  // blur-nx (2026-11-10, port 76; its key 215): StackHeaderSet
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/line_compare.h"

// banjo-nx (2026-10-20, from sa2-nx): see SnapshotKind below.
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas, true, "Banjo/Enhancements",
                    "Renut's renderer: a draw keeps what changed in the device since the device's previous draw (its "
                    "64-byte chunks that differ, and the float constant groups the game marked dirty) instead of a "
                    "copy of the device's first 12,712 bytes (N&B's; SA2 13,088); the render thread rebuilds the device from them. Off: "
                    "the whole copy for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas_constants, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): the chain's draws tell the constant uploads which "
                    "float constant groups changed since the previous draw, so only those are compared with what the "
                    "GPU holds (as a recorded command buffer's replayed draws do); a frame's first draw compares them "
                    "all. Off: every draw compares every group its shaders read")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_capture_batches, true, "Banjo/Enhancements",
                    "Renut's renderer (with banjo_renut_capture_pool): the game thread takes its draws' captures from "
                    "the pool, and the render thread gives them back, 32 at a time instead of one lock of the pool's "
                    "per draw on each thread. Off: one at a time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): see CapturePool::Keeps.
REXCVAR_DEFINE_BOOL(banjo_renut_small_captures, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_capture_pool): a delta is written into the chain's own scratch and "
                    "copied into its capture at its size (about 250 bytes), instead of into the capture's 12.9 KB "
                    "storage (zero-filled when the capture was made), and the pool keeps up to 8,192 captures or 32 MB "
                    "instead of 2,048. Bangai-O's missile storms queue ~4,250 draws a frame: past 2,048 the pool deleted "
                    "and made again ~2,200 captures a frame. Same data. In-run A/B target 82 (70 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): see DeltaRuns.
REXCVAR_DEFINE_BOOL(banjo_renut_delta_masks, true, "Banjo/Enhancements",
                    "Renut's renderer on the Switch (banjo_renut_snapshot_deltas): the game thread finds a draw's "
                    "changed device chunks with one NEON reduction per four chunks of a run (the 40 compared chunks are "
                    "three runs), then visits only those (1.1 a draw), instead of a reduction, a move to a register and "
                    "a branch per chunk (65 % of CaptureDelta, 18 % of the game thread in the v4 console storm). The "
                    "same chunks. In-run A/B target 81 (73 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29, v6): see PrefetchDeltaLines.
REXCVAR_DEFINE_BOOL(banjo_renut_delta_prefetch, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): a queued draw first asks for the device and shadow "
                    "lines its delta compares (80 lines), so that their cache misses overlap each other and the draw's "
                    "other work instead of stalling the compare (CaptureDelta's samples were its loads in the v5 console "
                    "storm). A hint only: the same data. In-run A/B target 84 (74 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas_narrow, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): the game thread compares only the device chunks the "
                    "render thread reads (39 of 71 in N&B, 40 of 77 in SA2: not the dirty masks, ring pointers and call tables before the fetch "
                    "constants, nor the private words, 10880-11775 in N&B (SA2 11008-12223), which only the hooks use on the live "
                    "device). SA2's second console run: comparing all of them was 9 % of its game thread. Off: all")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_neon_compare, true, "Banjo/Enhancements",
                    "Renut's renderer on the Switch: the device chunks of the delta chain and the pushed shared "
                    "constants' lines are compared 64 bytes at a time with NEON (four 128-bit loads a side) instead of "
                    "eight 64-bit words, which GCC left scalar. Off: the 64-bit words")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas_verify, false, "Banjo/Enhancements",
                    "Tests (banjo_renut_snapshot_deltas): every draw also keeps the whole copy, and the render thread "
                    "counts the draws whose rebuilt device differs from it in any byte (the 10 s \"[renut] device "
                    "snapshots\" line)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-20, from futari-nx's futari_renut_deferred_chain): see QueueDeferredDraw.
REXCVAR_DEFINE_BOOL(banjo_renut_deferred_chain, false, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): a Begin*Vertices draw joins the delta chain too "
                    "instead of a whole device copy (made under the chain's lock and queued under it by the next hook's "
                    "FlushDeferredVertices, before any other draw of the device). OFF by default since package 36q: the "
                    "chain is one for every device, and N&B draws on two (from two threads): a draw of the other device "
                    "made and queued between a Begin*Vertices delta and its queueing puts that delta on the wrong device "
                    "(emulator hangs in 4 of 8 runs with it on, 0 before). Off: a whole copy, as sa2-nx has it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): how a queued draw holds its device.
//   Whole:  DrawCapture::state is the device's first kDeviceSnapshotSize bytes (every draw before; the Begin*Vertices
//           draws, which are queued later than they are captured, still).
//   Rebase: the same, and the render thread takes it as its device (the chain's first draw, and each frame's).
//   Delta:  DrawCapture::state is a DeltaHeader and the bytes it names: the render thread applies them to its device.
// banjo-nx: N&B records most of its draws in command buffers, which keep deltas the same way (command_buffers.cpp);
// this is for its immediate draws. SA2 records no command buffers, so every draw was a whole copy: 13 KB a draw, 15-18 MB a frame of City Escape's
// 1,100-1,400 draws written on the game thread, into pooled captures last touched by the render thread on another
// core, then read back by the render thread. A draw changes a few bindings, render states and constants: the deltas are
// those. The chain's order is the queue's: deltas are made and queued under g_delta.mutex, and the render thread runs
// the queue in order.
enum class SnapshotKind : u8 { Whole, Rebase, Delta };

// What a queued draw reads besides guest memory the game keeps until a fence.
struct DrawCapture {
  std::vector<u8> state;  // the device, kDeviceSnapshotSize bytes (or a delta: SnapshotKind)
  std::vector<u8> vertices, indices;
  SnapshotKind snapshot = SnapshotKind::Whole;  // banjo-nx (2026-10-20, from sa2-nx)
  std::vector<u8> verify;  // banjo-nx (from sa2-nx): the whole device as well (banjo_renut_snapshot_deltas_verify)
  DrawCall call;
  u32 binding = 0;  // halo3-nx (2026-11-03, banjo_renut_recorded_binds): the shader's bind at the draw (LastBindingOf)
  // banjo-nx (2026-10-19, CPU proposal 23): a queued clear's or resolve's arguments. They come from the same pool as
  // the draws' captures: each made three heap allocations on the game thread (its arguments, a device snapshot, a
  // closure too large for std::function's own storage), freed on the render thread through the one malloc lock.
  u32 flags = 0;
  bool has_rect = false, has_color = false, has_dest_format = false;
  ClearRect rect{};
  float color[4] = {};
  float depth = 0.0f;
  u32 stencil = 0;
  u32 dest_texture_va = 0, dest_level = 0, dest_slice = 0;
  i32 dest_x = 0, dest_y = 0;
  d3d::TextureFetchConstant dest_format{};
  u32 held = 0;  // bangaio-nx (2026-10-29): the bytes it holds, set when it goes back to the pool (CapturePool::Give)
  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers): the headers of the stack surfaces bound
  // as targets when this draw, clear or resolve was captured (render/surfaces.h); count 0 nearly always.
  StackHeaderSet stack_headers;
};

// banjo-nx (banjo_renut_capture_pool): a draw's capture comes from a pool and goes back to it once the render thread
// has drawn it. Each draw made three heap allocations on the game thread (the capture, its 12.7 KB device snapshot,
// the queued closure) and freed them on the render thread: about 2,000 a frame through the Switch's one malloc lock.
// A pooled capture keeps its snapshot's storage, and the closure holds a plain pointer, which std::function stores
// without allocating. Off: every capture is deleted after use, as before (a new one per draw).
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_capture_batches): the pool's lock was taken twice a draw, by the game thread (Take) and
// by the render thread (Give), on two cores: its cache line went back and forth about 1,800 times a City Escape frame.
// Each thread now keeps a few captures of its own and goes to the pool kBatch at a time.
bool CaptureBatchesOn();
bool SmallCapturesOn();

class CapturePool {
public:
  // bangaio-nx (2026-10-29): room for the most the pool keeps, so that no push under the lock reallocates.
  CapturePool() {
    free_.reserve(kMaxFreeSmall);
    free_held_.reserve(kMaxFreeSmall);
  }

  // banjo-nx (2026-10-20, from sa2-nx): 'chain_locked' when the caller holds g_delta.mutex (the chain's draws): its batch is a plain
  // list that lock guards. Thread-local lists cost a call a use here (this toolchain's TLS goes through
  // __aarch64_read_tp: 0.8 % of the render thread in the second console run).
  DrawCapture *Take(bool chain_locked = false) {
    if (CaptureBatchesOn() && chain_locked) {
      std::vector<DrawCapture *> &taken = chain_taken_;
      if (taken.empty()) {
        std::lock_guard lock(mutex_);
        const size_t n = std::min(kBatch, free_.size());
        taken.assign(free_.end() - std::ptrdiff_t(n), free_.end());
        free_.resize(free_.size() - n);
        DropHeldLocked(n);
      }
      if (!taken.empty()) {
        DrawCapture *capture = taken.back();
        taken.pop_back();
        // bangaio-nx (2026-10-29, ARM): the next draw's capture, last written by the render thread on another core, is
        // fetched for writing while the game runs to that draw (its first lines read in CaptureDelta were a miss each
        // draw in the v4 console profile).
        if (!taken.empty()) {
          const char *next = reinterpret_cast<const char *>(taken.back());
          __builtin_prefetch(next, 1);
          __builtin_prefetch(next + 64, 1);
          __builtin_prefetch(next + 128, 1);
          // sasrt-nx (2026-11-12, batch C/D, item C8; port 93): the line of the stack headers' count, which
          // CaptureStackHeaders writes at every capture (sasrt-nx's +320 and +384, its recorded surfaces, are the
          // stack headers' own here, written only for a stack surface: not fetched)
          __builtin_prefetch(next + 256, 1);
        }
        return capture;
      }
      return new DrawCapture();
    }
    {
      std::lock_guard lock(mutex_);
      if (!free_.empty()) {
        DrawCapture *capture = free_.back();
        free_.pop_back();
        DropHeldLocked(1);
        return capture;
      }
    }
    return new DrawCapture();
  }
  // banjo-nx (2026-10-20, from sa2-nx): 'render_thread' from the render thread's items (only that thread then touches its batch; a
  // queue run inline at shutdown has the render thread gone).
  // bangaio-nx (2026-10-29): the captures the pool does not keep are deleted after its lock is let go (the frees take
  // the malloc lock, which the game thread's allocations wait on).
  void Give(DrawCapture *capture, bool render_thread = false) {
    if (!capture)
      return;
    if (banjo::renut_gpu::CapturePoolOn()) {
      capture->vertices.clear();
      capture->indices.clear();
      capture->held = HeldBytes(*capture);  // its lines are this thread's now: not read again under the lock
      if (CaptureBatchesOn() && render_thread) {
        std::vector<DrawCapture *> &given = render_given_;
        given.push_back(capture);
        if (given.size() < kBatch)
          return;
        const bool small = SmallCapturesOn();  // its guard and the A/B word read once, not under the lock a capture
        {
          std::lock_guard lock(mutex_);
          for (DrawCapture *&c : given) {
            if (KeepLocked(c, small))
              c = nullptr;
          }
        }
        for (DrawCapture *c : given)
          delete c;
        given.clear();
        return;
      }
      const bool small = SmallCapturesOn();
      {
        std::lock_guard lock(mutex_);
        if (KeepLocked(capture, small))
          return;
      }
    }
    delete capture;
  }

private:
  // bangaio-nx (2026-10-29, banjo_renut_small_captures, A/B 82 here): the console's missile storm queued ~4,250 draws a frame.
  // At 2,048 the pool deleted the rest when the render thread gave them back, and the game thread made ~2,200 again a
  // frame: each a malloc, and a 12.9 KB zero-fill of its delta storage, under the one malloc lock that the render
  // thread's frees took too. CaptureDelta now copies a delta into its capture at its size, so a capture keeps a few
  // hundred bytes, and the pool keeps up to kMaxFreeSmall. Whole copies (rebases, clears, resolves, deltas off) keep
  // 12.9 KB each: the byte cap holds them to about as many as before (32 MB: 2,048 of those were 26 MB).
  static constexpr size_t kMaxFree = 2048;        // more than a frame's draws in flight (banjo-nx: ~2,000 a frame)
  static constexpr size_t kMaxFreeSmall = 8192;   // bangaio-nx: twice the storm's
  static constexpr size_t kMaxHeldBytes = size_t(32) << 20;
  static constexpr size_t kBatch = 32;      // banjo-nx (from sa2-nx): captures a thread takes or gives back at once

  static u32 HeldBytes(const DrawCapture &c) {
    return u32(sizeof(DrawCapture) + c.state.capacity() + c.vertices.capacity() + c.indices.capacity() +
               c.verify.capacity());
  }
  // Under mutex_; 'small': SmallCapturesOn().
  bool KeepLocked(DrawCapture *c, bool small) {
    if (small ? free_.size() >= kMaxFreeSmall || held_ + c->held > kMaxHeldBytes : free_.size() >= kMaxFree)
      return false;
    free_.push_back(c);
    free_held_.push_back(c->held);
    held_ += c->held;
    return true;
  }
  // Under mutex_: the last n captures' bytes, which left free_, from free_held_ (contiguous, so no capture is read).
  void DropHeldLocked(size_t n) {
    const size_t end = free_held_.size();
    size_t bytes = 0;
    for (size_t i = end - n; i < end; ++i)
      bytes += free_held_[i];
    free_held_.resize(end - n);
    held_ -= bytes;
  }

  std::mutex mutex_;
  std::vector<DrawCapture *> free_;
  std::vector<u32> free_held_;  // bangaio-nx (2026-10-29): free_'s captures' bytes, in the same order
  size_t held_ = 0;             // their sum
  std::vector<DrawCapture *> chain_taken_;   // banjo-nx (from sa2-nx): under g_delta.mutex
  std::vector<DrawCapture *> render_given_;  // banjo-nx (from sa2-nx): the render thread's
};
CapturePool g_capture_pool;

// A Begin*Vertices draw waiting for its vertices, per device.
struct DeferredDraw {
  DrawCapture *capture = nullptr;  // owned (g_capture_pool)
  const u8 *scratch = nullptr;
  u32 bytes = 0;
};
std::mutex g_deferred_mutex;
std::unordered_map<u32, DeferredDraw> g_deferred;
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_callbacks_fast): g_deferred.size(), kept under g_deferred_mutex and read without it
// (FlushDeferredVertices: every hook calls it first). A Begin*Vertices draw is flushed by the next hook of its device,
// on the thread that made it, which sees the count in program order.
std::atomic<size_t> g_deferred_count{0};

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): the delta chain's layout. Of the device's first kDeviceSnapshotSize
// bytes, the float constants (VertexShaderF, PixelShaderF: 8 KB, two thirds) go by the groups m_Pending marks dirty
// (one bit per four registers, bit 63 - group, as the recorded command buffers keep them: command_buffers.cpp); the
// rest goes by 64-byte chunks (30 before the constants, 47 after them, the last one 32 bytes), those that differ from
// the chain's previous draw (compared on the game thread with its shadow).
constexpr u32 kDeltaVertexF = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderF));
constexpr u32 kDeltaPixelF = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, PixelShaderF));
constexpr u32 kDeltaTailBegin = kDeltaPixelF + 256 * 16;
constexpr u32 kDeltaUnitBytes = 64;  // a chunk, and a constant group
constexpr u32 kDeltaHeadChunks = kDeltaVertexF / kDeltaUnitBytes;
constexpr u32 kDeltaTailBytes = d3d::kDeviceSnapshotSize - kDeltaTailBegin;
constexpr u32 kDeltaChunks = kDeltaHeadChunks + (kDeltaTailBytes + kDeltaUnitBytes - 1) / kDeltaUnitBytes;
static_assert(kDeltaVertexF % kDeltaUnitBytes == 0 && kDeltaPixelF == kDeltaVertexF + 256 * 16);
static_assert(kDeltaTailBegin == u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderB)));
static_assert(kDeltaChunks <= 128);
static_assert(kDeltaHeadChunks == kChainHeadChunks && kDeltaTailBegin == kChainTailBegin);  // draw.h's ChainHint

constexpr u32 DeltaChunkOffset(u32 i) {
  return i < kDeltaHeadChunks ? i * kDeltaUnitBytes : kDeltaTailBegin + (i - kDeltaHeadChunks) * kDeltaUnitBytes;
}
constexpr u32 DeltaChunkSize(u32 i) {
  return i < kDeltaHeadChunks ? kDeltaUnitBytes
                              : std::min(kDeltaUnitBytes, kDeltaTailBytes - (i - kDeltaHeadChunks) * kDeltaUnitBytes);
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas_narrow): the device bytes the render thread never reads from a draw's
// device (DrawStateLocked and what it calls: the accessors of render/draw.cpp, textures.cpp, surfaces.cpp, the pipeline
// key): before the fetch constants (the dirty masks, ring pointers and Set/Get*State call tables, which a recorded
// command buffer's replay leaves zero too), and the private words 11008-12223 (the creation thread, the fences, the
// swap flags: the hooks read and write them on the live device). Their chunks are not compared; the render thread's
// device keeps them as the frame's whole copy had them.
constexpr u32 kDeltaUnreadEnd = kDeltaVertexF - 0x300;  // 0x480: the fetch constants' start
// banjo-nx (2026-10-20): N&B's private block lies 128 bytes earlier than SA2's (d3d.h: the creation thread at
// 10888, the fence tags at 10908/10928, the swap flags at 10941), and its vertex declaration at 11820 (SA2 12240).
// The render thread's last packet read is m_MiscPacket (d3d_native_state.cpp, up to 0x2A50); nothing it reads lies
// between 0x2A50 and the vertex declaration. So 10880 (0x2A80, the first chunk past m_MiscPacket) to 11776 (the
// chunk before the one holding the vertex declaration). SA2: 11008-12224.
// katamari-nx: a 64-byte chunk boundary, not a field: 11776 is still the last boundary before Beautiful Katamari's
// vertex declaration (+11812), and the words between are private ones the render thread does not read.
constexpr u32 kDeltaPrivateUnreadBegin = 10880, kDeltaPrivateUnreadEnd = 11776;
constexpr bool DeltaChunkRead(u32 i) {
  const u32 at = DeltaChunkOffset(i);
  return !(at < kDeltaUnreadEnd || (at >= kDeltaPrivateUnreadBegin && at + 64 <= kDeltaPrivateUnreadEnd));
}
static_assert(kDeltaUnreadEnd == 0x480 && kDeltaUnreadEnd % kDeltaUnitBytes == 0);
static_assert((kDeltaPrivateUnreadBegin - kDeltaTailBegin) % kDeltaUnitBytes == 0 &&
              (kDeltaPrivateUnreadEnd - kDeltaTailBegin) % kDeltaUnitBytes == 0);
static_assert(d3d::kDeviceOffsetVertexDeclaration >= kDeltaPrivateUnreadEnd);
// The chunks compared: all, or the read ones.
struct DeltaChunkList {
  u32 count = 0;
  u8 index[128] = {};
};
constexpr DeltaChunkList MakeDeltaChunkList(bool narrow) {
  DeltaChunkList l{};
  for (u32 i = 0; i < kDeltaChunks; ++i)
    if (!narrow || DeltaChunkRead(i))
      l.index[l.count++] = u8(i);
  return l;
}
constexpr DeltaChunkList kDeltaAllChunks = MakeDeltaChunkList(false);
constexpr DeltaChunkList kDeltaReadChunks = MakeDeltaChunkList(true);
static_assert(kDeltaReadChunks.count == 39);  // N&B (SA2 40; bangaio-nx's XDK 20209: 40)

// bangaio-nx (2026-10-29, banjo_renut_delta_masks, A/B 81 here, 73 in bangaio-nx): a chunk list as runs of whole chunks at consecutive
// offsets, and each short chunk (the tail's last, 24 bytes in XDK 20209) as a run of its own. CaptureDelta asks
// Lines64DifferMask which chunks of a run differ (line_compare.h: one NEON reduction per four chunks and no branch per
// chunk), then visits the set bits in order: the chunks the per-chunk loop found, in its order. The narrow list is
// three runs (12, 12 and 15 chunks) and the short chunk.
struct DeltaRun {
  u8 first = 0;  // chunk index
  u8 count = 0;  // chunks (at most 32: Lines64DifferMask's)
  bool short_chunk = false;
};
struct DeltaRuns {
  u32 count = 0;
  DeltaRun run[16] = {};
};
constexpr DeltaRuns MakeDeltaRuns(const DeltaChunkList &l) {
  DeltaRuns r{};
  for (u32 k = 0; k < l.count; ++k) {
    const u32 i = l.index[k];
    const bool whole = DeltaChunkSize(i) == kDeltaUnitBytes;
    if (whole && r.count) {
      DeltaRun &last = r.run[r.count - 1];
      if (!last.short_chunk && last.count < 32 && last.first + last.count == i &&
          DeltaChunkOffset(i) == DeltaChunkOffset(last.first) + last.count * kDeltaUnitBytes) {
        ++last.count;
        continue;
      }
    }
    r.run[r.count++] = DeltaRun{u8(i), u8(1), !whole};
  }
  return r;
}
// A short chunk (fewer than 64 bytes): 8 bytes at a time, inline (memcmp with a size known only at run time was a call).
inline bool ShortChunkDiffers(const u8 *a, const u8 *b, u32 size) {
  uint64_t diff = 0;
  u32 k = 0;
  for (; k + 8 <= size; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  for (; k < size; ++k)
    diff |= u8(a[k] ^ b[k]);
  return diff != 0;
}
constexpr DeltaRuns kDeltaAllRuns = MakeDeltaRuns(kDeltaAllChunks);
constexpr DeltaRuns kDeltaReadRuns = MakeDeltaRuns(kDeltaReadChunks);
// renut-ultimate: bangaio-nx asserted its XDK's layout (4 runs, the last the short chunk); the runs are made from whatever the
// chunk lists are, so only their number is bounded here.
static_assert(kDeltaReadRuns.count <= 16 && kDeltaAllRuns.count <= 16);
// Whether device byte 'at' is one the render thread reads (the verify mode compares those).
constexpr bool DeltaByteRead(u32 at) {
  return !(at < kDeltaUnreadEnd || (at >= kDeltaPrivateUnreadBegin && at < kDeltaPrivateUnreadEnd));
}

struct DeltaHeader {
  uint64_t chunks[2];  // chunk i follows: bit i % 64 of word i / 64
  uint64_t vs, ps;     // the float constant groups that follow (bit 63 - group), the vertex shader's first
};
// The most a delta holds: every chunk and every group.
constexpr size_t kDeltaMaxBytes = sizeof(DeltaHeader) + d3d::kDeviceSnapshotSize;

// Calls f(first, count) for each run of consecutive groups in an m_Pending mask (bit 63 - group).
template <typename F>
void ForEachGroupRun(uint64_t mask, F f) {
  while (mask) {
    const u32 first = u32(std::countl_zero(mask));
    const u32 count = u32(std::countl_one(mask << first));
    f(first, count);
    mask = first + count >= 64 ? 0 : mask & (~uint64_t(0) >> (first + count));
  }
}

// Whether two whole chunks differ: fixed-size loads the compiler keeps inline and vectorises.
bool DeltaChunkDiffers(const u8 *a, const u8 *b) {
  uint64_t diff = 0;
  for (u32 k = 0; k < kDeltaUnitBytes; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

// The game threads' side of the chain.
struct DeltaChain {
  std::mutex mutex;                     // the chain's draws are made and queued in one order
  // The device it follows; none: the next draw rebases. banjo-nx (2026-10-26): atomic so ForgetSnapshotDeltas can
  // tell without the lock that a recorded op's device is not the chain's; written under the lock only.
  std::atomic<const d3d::Device *> device{nullptr};
  uint64_t vs = 0, ps = 0;              // groups whole captures consumed since its last draw: they follow
  uint64_t deltas = 0, rebases = 0, bytes = 0, chunks = 0, groups = 0;  // the 10 s report's
  uint64_t whole = 0;  // banjo-nx (from sa2-nx): whole captures outside the chain (Begin*Vertices draws), under the lock too
  alignas(64) u8 shadow[d3d::kDeviceSnapshotSize];  // the device at its last draw (the chunks)
  // bangaio-nx (2026-10-29, banjo_renut_small_captures): each delta is written here, then copied into its capture at its
  // size. The same lines every draw: they stay in the game thread's cache.
  alignas(64) u8 scratch[kDeltaMaxBytes];
};
DeltaChain g_delta;

// The render thread's side: the device the chain's draws rebuild.
alignas(64) u8 g_delta_device[d3d::kDeviceSnapshotSize];
std::atomic<uint64_t> g_delta_verified{0}, g_delta_differ{0};
// banjo_renut_snapshot_deltas_constants: the chain as a replay of the constant uploads' tracking (draw.h,
// ReplayConstants): its id (a new one at each whole copy; the top bit keeps it apart from the command buffers'
// replays, numbered from 1) and the float constant groups written since the upload last consumed them.
uint64_t g_chain_id = 0, g_chain_count = 0;
uint64_t g_chain_vs_dirty = ~uint64_t(0), g_chain_ps_dirty = ~uint64_t(0);
// banjo_renut_chain_reuse: the number of the chain's last draw (draw.h ChainHint::seq). banjo-nx (2026-10-20): from the
// shared NextChainSeq, since the command buffers' replays give hints too: when a draw of theirs came between two of
// the chain's (the number is not this one + 1), the chain's next draw marks every chunk changed.
uint64_t g_chain_seq = 0;

bool DeltasOn() {
  static const bool on = REXCVAR_GET(banjo_renut_snapshot_deltas);
  return on;
}
bool ChainConstantsOn() {
  static const bool on = DeltasOn() && REXCVAR_GET(banjo_renut_snapshot_deltas_constants);
  return on;
}
// bangaio-nx (2026-10-29, ARM): these read the latched switches (banjo_renut_tuning.h, plain loads), not function
// statics: each static's guard was a load-acquire at every draw on the game thread (the v4 console profile).
bool CaptureBatchesOn() {
  return banjo::renut_gpu::Latch().capture_batches;
}
bool SmallCapturesOn() {
  return banjo::renut_gpu::Latch().small_captures && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 82;  // 70 in bangaio-nx
}
bool NeonCompareOn() {
  return banjo::renut_gpu::Latch().neon_compare;
}
bool DeltasNarrowOn() {
  return banjo::renut_gpu::Latch().deltas_narrow;
}
bool DeltaMasksOn() {  // bangaio-nx (2026-10-29, A/B 81 here)
  return banjo::renut_gpu::Latch().delta_masks && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 81;  // 73 in bangaio-nx
}
bool DeltaPrefetchOn() {  // bangaio-nx (2026-10-29, v6, A/B 74)
  return banjo::renut_gpu::Latch().delta_prefetch && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 84;  // 74 in bangaio-nx
}
// banjo-nx (2026-10-20, from futari-nx): banjo_renut_deferred_chain.
bool DeferredChainOn() {
  static const bool on = REXCVAR_GET(banjo_renut_deferred_chain);
  return on;
}
bool DeltasVerifyOn() {
  return banjo::renut_gpu::Latch().deltas_verify;  // bangaio-nx (2026-10-29): latched (with the deltas on)
}

// The original draw calls consume the device's dirty masks as they write
// their packets; a snapshot taken after this has them clear, as the command
// buffer ops' do.
void ClearPending(d3d::Device *device) {
  // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas): the dirty constant groups a whole capture consumes go with the chain's next
  // delta (its callers hold g_delta.mutex when the deltas are on).
  if (device == g_delta.device.load(std::memory_order_relaxed)) {
    g_delta.vs |= uint64_t(device->m_Pending[0]);
    g_delta.ps |= uint64_t(device->m_Pending[1]);
  }
  for (auto &mask : device->m_Pending)
    mask = 0;
}

// banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas), on the render thread: the device a queued draw reads.
const u8 *DeviceForDraw(const DrawCapture &capture) {
  if (capture.snapshot == SnapshotKind::Whole) {
    // banjo-nx (2026-10-28, per-draw night, A/B 62): a number of its own, every chunk and field changed (as a replay's
    // first draw): the fast path may take it after the draw before it. The chain's own number stays (its next delta
    // draw is not consecutive with it: every chunk then too). Off half: not a chain draw, as before.
    if (banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 62) {
      g_chain_hint = {};  // not a chain draw (banjo_renut_chain_reuse)
    } else {
      g_chain_hint.seq = NextChainSeq();
      g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);
      g_chain_hint.fields = kFieldsAll;
    }
    return capture.state.data();
  }
  if (capture.snapshot == SnapshotKind::Rebase) {
    std::memcpy(g_delta_device, capture.state.data(), d3d::kDeviceSnapshotSize);
    g_chain_id = (uint64_t(1) << 63) | ++g_chain_count;  // its first draw compares every group
    g_chain_vs_dirty = g_chain_ps_dirty = ~uint64_t(0);
    g_chain_hint.seq = g_chain_seq = NextChainSeq();
    g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);  // a whole copy: every chunk
    g_chain_hint.fields = kFieldsAll;
  } else {
    const u8 *from = capture.state.data();
    DeltaHeader h;
    std::memcpy(&h, from, sizeof(h));
    from += sizeof(h);
    const uint64_t seq = NextChainSeq();
    const bool consecutive = seq == g_chain_seq + 1;  // banjo-nx: no replayed draw in between
    g_chain_hint.seq = g_chain_seq = seq;
    g_chain_hint.changed[0] = consecutive ? h.chunks[0] : ~uint64_t(0);
    g_chain_hint.changed[1] = consecutive ? h.chunks[1] : ~uint64_t(0);
    g_chain_hint.fields = consecutive ? ChainFields(h.chunks) : kFieldsAll;  // banjo-nx (2026-10-26)
    g_chain_vs_dirty |= h.vs;
    g_chain_ps_dirty |= h.ps;
    for (u32 word = 0; word < 2; ++word) {
      for (uint64_t bits = h.chunks[word]; bits; bits &= bits - 1) {
        const u32 i = word * 64 + u32(std::countr_zero(bits));
        const u32 size = DeltaChunkSize(i);
        // Whole chunks as a fixed-size copy, which stays inline.
        if (size == kDeltaUnitBytes)
          std::memcpy(g_delta_device + DeltaChunkOffset(i), from, kDeltaUnitBytes);
        else
          std::memcpy(g_delta_device + DeltaChunkOffset(i), from, size);
        from += size;
      }
    }
    for (const auto [dirty, base] : {std::pair{h.vs, kDeltaVertexF}, std::pair{h.ps, kDeltaPixelF}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        const u32 bytes = count * kDeltaUnitBytes;
        std::memcpy(g_delta_device + base + first * kDeltaUnitBytes, from, bytes);
        from += bytes;
      });
    }
  }
  if (!capture.verify.empty()) {
    g_delta_verified.fetch_add(1, std::memory_order_relaxed);
    // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas_narrow): the bytes the render thread reads; with the narrow compare off, all.
    bool same;
    if (DeltasNarrowOn()) {
      same = std::memcmp(g_delta_device + kDeltaUnreadEnd, capture.verify.data() + kDeltaUnreadEnd,
                         kDeltaPrivateUnreadBegin - kDeltaUnreadEnd) == 0 &&
             std::memcmp(g_delta_device + kDeltaPrivateUnreadEnd, capture.verify.data() + kDeltaPrivateUnreadEnd,
                         d3d::kDeviceSnapshotSize - kDeltaPrivateUnreadEnd) == 0;
    } else {
      same = std::memcmp(g_delta_device, capture.verify.data(), d3d::kDeviceSnapshotSize) == 0;
    }
    if (!same) {
      g_delta_differ.fetch_add(1, std::memory_order_relaxed);
      static uint32_t logged = 0;
      if (logged < 8) {
        ++logged;
        u32 at = 0;
        while (at < d3d::kDeviceSnapshotSize &&
               (g_delta_device[at] == capture.verify[at] || (DeltasNarrowOn() && !DeltaByteRead(at))))
          ++at;
        RGPU_WARN("Device snapshot delta: the rebuilt device differs from the device at +{} ({} draw): {:02X}, the "
                  "device {:02X} (banjo_renut_snapshot_deltas_verify)",
                  at, capture.snapshot == SnapshotKind::Rebase ? "a whole" : "a delta", g_delta_device[at],
                  capture.verify[at]);
      }
    }
  }
  return g_delta_device;
}

void QueueCapture(DrawCapture *capture) {
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): the capture and its device bytes, written on the game thread's core,
  // are fetched into the render thread's cache while it does the item before this one.
  render_thread::QueuePrefetched(capture, capture->state.data(), [capture] {
    DrawCall call = capture->call;
    if (!capture->vertices.empty())
      call.up_vertices = capture->vertices.data();
    if (!capture->indices.empty())
      call.up_indices = capture->indices.data();
    const u8 *device = DeviceForDraw(*capture);
    banjo::renut_gpu::CountDrawSource(0);
    // blur-nx (2026-11-10, port 76; its key 215)
    SetReplayStackHeaders(capture->stack_headers.count ? &capture->stack_headers : nullptr);
    if (capture->binding) {  // halo3-nx (2026-11-03): after a replay's, this draw's own
      ApplyBindingLocked(capture->binding);
      capture->binding = 0;
    }
    if (capture->snapshot != SnapshotKind::Whole && ChainConstantsOn()) {
      // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas_constants): every constant is the rebuilt device's (no running device).
      const ReplayConstants chain{nullptr, ~uint64_t(0), ~uint64_t(0), &g_chain_vs_dirty, &g_chain_ps_dirty,
                                  g_chain_id};
      DrawStateLocked(device, call, &chain);
    } else {
      DrawStateLocked(device, call);
    }
    g_chain_hint = {};  // banjo-nx (from sa2-nx): only for the draw itself
    SetReplayStackHeaders(nullptr);  // blur-nx (2026-11-10, port 76; its key 215)
    g_capture_pool.Give(capture, true);
  });
}

DrawCapture *Capture(u32 device_va, const DrawCall &call) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  ClearPending(device);
  DrawCapture *capture = g_capture_pool.Take();
  // assign() keeps a pooled capture's storage (Snapshot() made a new vector each time).
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  capture->state.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
  capture->snapshot = SnapshotKind::Whole;  // banjo-nx (from sa2-nx)
  CaptureStackHeaders(bytes, capture->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  capture->verify.clear();
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  return capture;
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): a draw of the chain. Requires g_delta.mutex, held until the capture
// is queued.
DrawCapture *CaptureDelta(u32 device_va, const DrawCall &call) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  DeltaChain &c = g_delta;
  DrawCapture *capture = g_capture_pool.Take(true);
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  CaptureStackHeaders(bytes, capture->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  if (c.device.load(std::memory_order_relaxed) != device) {
    // The chain's first draw (or a frame's): the whole device, which the render thread takes as its own.
    for (auto &mask : device->m_Pending)
      mask = 0;
    capture->state.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
    std::memcpy(c.shadow, bytes, d3d::kDeviceSnapshotSize);
    c.device.store(device, std::memory_order_relaxed);
    c.vs = c.ps = 0;
    capture->snapshot = SnapshotKind::Rebase;
    ++c.rebases;
    c.bytes += d3d::kDeviceSnapshotSize;
  } else {
    const uint64_t vs = c.vs | uint64_t(device->m_Pending[0]);
    const uint64_t ps = c.ps | uint64_t(device->m_Pending[1]);
    c.vs = c.ps = 0;
    for (auto &mask : device->m_Pending)
      mask = 0;
    std::vector<u8> &out = capture->state;
    // bangaio-nx (2026-10-29, banjo_renut_small_captures): into the chain's scratch, copied into the capture at its size
    // below; off, into the capture's own storage at its greatest size.
    const bool small = SmallCapturesOn();
    if (!small && out.size() < kDeltaMaxBytes)
      out.resize(kDeltaMaxBytes);  // once per pooled capture: written below, never cleared
    u8 *const begin = small ? c.scratch : out.data();
    u8 *to = begin + sizeof(DeltaHeader);
    DeltaHeader h{};
    u32 chunks = 0;
    // A changed chunk: into the shadow and the delta, and its bit. bangaio-nx (2026-10-29, ARM): a whole chunk with
    // constant-size copies (inline q-register pairs; a variable size was two memcpy calls).
    const auto take = [&](u32 i) {
      const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
      if (size == kDeltaUnitBytes) {
        std::memcpy(c.shadow + at, bytes + at, kDeltaUnitBytes);
        std::memcpy(to, bytes + at, kDeltaUnitBytes);
      } else {
        std::memcpy(c.shadow + at, bytes + at, size);
        std::memcpy(to, bytes + at, size);
      }
      to += size;
      h.chunks[i / 64] |= uint64_t(1) << (i % 64);
      ++chunks;
    };
    const bool narrow = DeltasNarrowOn();
    if (DeltaMasksOn()) {
      // bangaio-nx (2026-10-29, banjo_renut_delta_masks): which chunks of each run differ, then those in order.
      const DeltaRuns &runs = narrow ? kDeltaReadRuns : kDeltaAllRuns;
      for (u32 r = 0; r < runs.count; ++r) {
        const DeltaRun &run = runs.run[r];
        const u32 at = DeltaChunkOffset(run.first);
        uint32_t mask = run.short_chunk ? uint32_t(ShortChunkDiffers(bytes + at, c.shadow + at, DeltaChunkSize(run.first)))
                                        : banjo::renut_gpu::Lines64DifferMask(bytes + at, c.shadow + at, run.count);
        for (; mask; mask &= mask - 1)
          take(run.first + u32(std::countr_zero(mask)));
      }
    } else {
      const DeltaChunkList &list = narrow ? kDeltaReadChunks : kDeltaAllChunks;
      const bool neon = NeonCompareOn();  // banjo-nx (from sa2-nx) (banjo_renut_neon_compare)
      for (u32 k = 0; k < list.count; ++k) {
        const u32 i = list.index[k];
        const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
        const bool differs = size == kDeltaUnitBytes ? banjo::renut_gpu::Line64Differs(bytes + at, c.shadow + at, neon)
                                                     : std::memcmp(bytes + at, c.shadow + at, size) != 0;
        if (differs)
          take(i);
      }
    }
    for (const auto [dirty, base] : {std::pair{vs, kDeltaVertexF}, std::pair{ps, kDeltaPixelF}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        const u32 group_bytes = count * kDeltaUnitBytes;
        std::memcpy(to, bytes + base + first * kDeltaUnitBytes, group_bytes);
        to += group_bytes;
      });
    }
    h.vs = vs;
    h.ps = ps;
    std::memcpy(begin, &h, sizeof(h));
    if (small)
      out.assign(begin, to);  // the capture's storage kept (a pooled vector keeps its capacity)
    capture->snapshot = SnapshotKind::Delta;
    ++c.deltas;
    c.bytes += uint64_t(to - begin);
    c.chunks += chunks;
    c.groups += uint64_t(std::popcount(vs) + std::popcount(ps));
  }
  if (DeltasVerifyOn())
    capture->verify.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
  else
    capture->verify.clear();
  return capture;
}

} // namespace

void SnapshotDeltasNewFrame() {
  if (!DeltasOn())
    return;
  std::lock_guard lock(g_delta.mutex);
  g_delta.device.store(nullptr, std::memory_order_relaxed);  // the frame's first draw rebases: whatever went wrong lasts a frame at most
}

void ForgetSnapshotDeltas(const void *device) {
  if (!DeltasOn())
    return;
  // banjo-nx (2026-10-26): only the chain's own device's masks matter to it; a recording device's ops (every recorded
  // op came here) took the chain's lock each, and made its next draw a whole copy, for nothing. The chain's device is
  // the title device, which the game does not record on while it draws on it elsewhere: an op of it is ordered with
  // its draws by the game, and so is this load with the store that made it the chain's.
  if (g_delta.device.load(std::memory_order_relaxed) != device)
    return;
  std::lock_guard lock(g_delta.mutex);
  g_delta.device.store(nullptr, std::memory_order_relaxed);
}

std::string SnapshotDeltaReport() {
  if (!DeltasOn())
    return {};
  uint64_t deltas, rebases, bytes, chunks, groups, whole;
  {
    std::lock_guard lock(g_delta.mutex);
    whole = g_delta.whole;
    g_delta.whole = 0;
    deltas = g_delta.deltas;
    rebases = g_delta.rebases;
    bytes = g_delta.bytes;
    chunks = g_delta.chunks;
    groups = g_delta.groups;
    g_delta.deltas = g_delta.rebases = g_delta.bytes = g_delta.chunks = g_delta.groups = 0;
  }
  const uint64_t draws = deltas + rebases;
  if (!draws)
    return {};
  const double per_delta = double(std::max<uint64_t>(deltas, 1));
  std::string line = fmt::format(
      "device snapshots (banjo_renut_snapshot_deltas): {} draws, {:.0f} bytes kept a draw on average (the whole device "
      "is {}); {:.1f} chunks and {:.1f} constant groups a delta; {} whole copies (the frames' first draws); {} "
      "Begin*Vertices draws as whole copies",
      draws, double(bytes) / double(draws), d3d::kDeviceSnapshotSize, double(chunks) / per_delta,
      double(groups) / per_delta, rebases, whole);
  if (DeltasVerifyOn()) {
    line += fmt::format("; verify: {} draws checked, {} differ", g_delta_verified.exchange(0, std::memory_order_relaxed),
                        g_delta_differ.exchange(0, std::memory_order_relaxed));
  }
  return line;
}

// bangaio-nx (2026-10-29, v6, banjo_renut_delta_prefetch): the lines CaptureDelta is about to compare, the device's and
// the shadow's, asked for at once (prfm: a hint, which cannot fault). The game's own work between two draws pushes them
// out of the L1, and the compare then waited on them a group at a time.
void PrefetchDeltaLines(u32 device_va) {
  const u8 *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  const DeltaRuns &runs = DeltasNarrowOn() ? kDeltaReadRuns : kDeltaAllRuns;
  for (u32 r = 0; r < runs.count; ++r) {
    const u32 at = DeltaChunkOffset(runs.run[r].first);
    for (u32 k = 0; k < runs.run[r].count; ++k) {
      __builtin_prefetch(device + at + k * kDeltaUnitBytes);
      __builtin_prefetch(g_delta.shadow + at + k * kDeltaUnitBytes);
    }
  }
}

void QueueDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes) {
  const bool deltas = DeltasOn();  // bangaio-nx (2026-10-29, ARM): its static's guard read once, not three times
  if (deltas && DeltaPrefetchOn())
    PrefetchDeltaLines(device_va);  // bangaio-nx (2026-10-29, v6)
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): made and queued under the chain's lock.
  std::unique_lock<std::mutex> chain;
  if (deltas)
    chain = std::unique_lock<std::mutex>(g_delta.mutex);
  DrawCapture *capture = deltas ? CaptureDelta(device_va, call) : Capture(device_va, call);
  if (!capture)
    return;
  if (call.up_vertices)
    capture->vertices.assign(call.up_vertices, call.up_vertices + up_vertex_bytes);
  if (call.up_indices)
    capture->indices.assign(call.up_indices, call.up_indices + up_index_bytes);
  if (const auto *guest_device = GuestPtr<const u8>(device_va)) {
    u32 vs_word;
    std::memcpy(&vs_word, guest_device + d3d::kDeviceOffsetVertexShader, 4);
    capture->binding = LastBindingOf(__builtin_bswap32(vs_word));
  }
  QueueCapture(capture);
}

void QueueDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  // banjo-nx (2026-10-20, from futari-nx, banjo_renut_deferred_chain): a Begin*Vertices draw of the chain too. Made under
  // the chain's lock and queued by FlushDeferredVertices under it as well, before any other draw of the device is made
  // (every hook flushes its device's deferred draw first), so the render thread still applies the chain in order. A
  // recorded op made meanwhile (NewOp: ForgetSnapshotDeltas) only makes the next capture a whole one, queued after this
  // draw. A draw that will not be queued (no scratch) stays a whole capture outside the chain.
  if (DeltasOn() && DeferredChainOn() && scratch) {
    std::unique_lock<std::mutex> chain(g_delta.mutex);
    {
      std::lock_guard lock(g_deferred_mutex);
      auto it = g_deferred.find(device_va);
      if (it != g_deferred.end()) {
        // One not flushed (FlushDeferredVertices above queued it, so none is expected): dropped as the original
        // replaced it; its delta never reaches the render thread, so the chain rebases.
        g_capture_pool.Give(it->second.capture);
        g_deferred.erase(it);
        g_deferred_count.store(g_deferred.size(), std::memory_order_release);
        g_delta.device.store(nullptr, std::memory_order_relaxed);
      }
    }
    DrawCapture *capture = CaptureDelta(device_va, call);
    if (!capture)
      return;
    std::lock_guard lock(g_deferred_mutex);
    g_deferred[device_va] = {capture, scratch, bytes};
    g_deferred_count.store(g_deferred.size(), std::memory_order_release);
    return;
  }
  DrawCapture *capture = nullptr;
  {
    // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas): a whole capture (queued later than made: outside the chain), whose dirty
    // constant groups ClearPending hands to the chain's next delta.
    std::unique_lock<std::mutex> chain;
    if (DeltasOn())
      chain = std::unique_lock<std::mutex>(g_delta.mutex);
    capture = Capture(device_va, call);
    if (DeltasOn())
      ++g_delta.whole;
  }
  if (!capture)
    return;
  if (!scratch) {
    g_capture_pool.Give(capture);
    return;
  }
  std::lock_guard lock(g_deferred_mutex);
  DeferredDraw &slot = g_deferred[device_va];
  g_capture_pool.Give(slot.capture);  // one not flushed (the original replaced it the same way)
  slot = {capture, scratch, bytes};
  g_deferred_count.store(g_deferred.size(), std::memory_order_release);
}

void FlushDeferredVertices(u32 device_va) {
  FlushRecordedVertices(device_va);
  if (HookFastPathsOn() && g_deferred_count.load(std::memory_order_acquire) == 0)
    return;  // banjo-nx (2026-10-20, from sa2-nx): none waiting, no lock
  // banjo-nx (2026-10-20, from futari-nx, banjo_renut_deferred_chain): a chain draw is queued under the chain's lock
  // (taken before the deferred map's, as QueueDeferredDraw takes them), so no draw of the chain is made or queued between.
  std::unique_lock<std::mutex> chain;
  if (DeltasOn() && DeferredChainOn())
    chain = std::unique_lock<std::mutex>(g_delta.mutex);
  DeferredDraw deferred;
  {
    std::lock_guard lock(g_deferred_mutex);
    auto it = g_deferred.find(device_va);
    if (it == g_deferred.end())
      return;
    deferred = std::move(it->second);
    g_deferred.erase(it);
    g_deferred_count.store(g_deferred.size(), std::memory_order_release);
  }
  deferred.capture->vertices.assign(deferred.scratch, deferred.scratch + deferred.bytes);
  QueueCapture(deferred.capture);
}

void QueueClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4], float depth,
                u32 stencil) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  const auto *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  DrawCapture *args = g_capture_pool.Take();  // banjo-nx (2026-10-19): pooled, see DrawCapture
  args->state.assign(device, device + d3d::kDeviceSnapshotSize);
  args->has_rect = rect != nullptr;
  args->rect = rect ? *rect : ClearRect{};
  std::memcpy(args->color, color, sizeof(args->color));
  args->flags = flags;
  args->depth = depth;
  args->stencil = stencil;
  CaptureStackHeaders(device, args->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  render_thread::Queue([args] {
    SetReplayStackHeaders(args->stack_headers.count ? &args->stack_headers : nullptr);
    ClearStateLocked(args->state.data(), args->flags, args->has_rect ? &args->rect : nullptr, args->color,
                     args->depth, args->stencil);
    SetReplayStackHeaders(nullptr);
    g_capture_pool.Give(args, true);
  });
}

void QueueResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                  i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                  const float clear_color[4], float clear_depth, u32 clear_stencil) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  const auto *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  // The destination header as it is now: the game often builds it on the
  // stack (as for recorded resolves).
  DrawCapture *args = g_capture_pool.Take();  // banjo-nx (2026-10-19): pooled, see DrawCapture
  args->state.assign(device, device + d3d::kDeviceSnapshotSize);
  args->has_rect = source_rect != nullptr;
  args->rect = source_rect ? *source_rect : ClearRect{};
  args->has_color = clear_color != nullptr;
  if (clear_color)
    std::memcpy(args->color, clear_color, sizeof(args->color));
  const auto *header = GuestPtr<const d3d::BaseTexture>(dest_texture_va);
  args->has_dest_format = header != nullptr;
  if (header)
    args->dest_format = header->Format;
  args->flags = flags;
  args->dest_texture_va = dest_texture_va;
  args->dest_x = dest_x;
  args->dest_y = dest_y;
  args->dest_level = dest_level;
  args->dest_slice = dest_slice;
  args->depth = clear_depth;
  args->stencil = clear_stencil;
  CaptureStackHeaders(device, args->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  render_thread::Queue([args] {
    SetReplayStackHeaders(args->stack_headers.count ? &args->stack_headers : nullptr);
    ResolveStateLocked(args->state.data(), args->flags, args->has_rect ? &args->rect : nullptr,
                       args->dest_texture_va, args->has_dest_format ? &args->dest_format : nullptr,
                       args->dest_x, args->dest_y, args->dest_level, args->dest_slice,
                       args->has_color ? args->color : nullptr, args->depth, args->stencil);
    SetReplayStackHeaders(nullptr);
    g_capture_pool.Give(args, true);
  });
}

} // namespace renut::gpu
