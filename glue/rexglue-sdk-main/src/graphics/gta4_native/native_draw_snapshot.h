// gta4_native: a draw's vertex and index bytes as the title submitted them (docs/switch-port/16-deko3d-devlog.md, run 49,
// section V).
//
// The deko3d recorder reads vertex and index buffers from guest memory when it records a draw, on its own thread. In run 48
// that thread was saturated and the ring of 4,096 packets full (1.3-1.5 frames of draws), so a draw is read up to that much
// after the title submitted it. Gta4NativeGraphicsSystem (LibertyRecompAndroid) copies every buffer at submit instead
// (CaptureBufferResource) and keeps a sampled comparison because "guest bytes can change without an unlock". Here:
// - the front end copies, at submit, the draw's range of the buffers the title rewrites: a buffer still locked, one written
//   again after its first fill (a second guest-write unlock) and one the recorder caught changing (below);
// - for 1 draw in kDrawHashEvery of the others it hashes the same range at submit; the recorder hashes it again when it
//   records the draw, counts a difference and hands the buffers back to the front end, which copies them from then on.
// So a buffer rewritten in place under a waiting draw is drawn with its submitted bytes, and the count says how often.
//
// No Switch headers here: tests/deko3d/test_draw_snapshot.py compiles it on the PC.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "native_index_policy.h"

namespace rex::graphics::gta4_native {

inline constexpr uint32_t kDrawHashEvery = 32;
inline constexpr uint32_t kSnapshotStreams = 17;  // = kVertexStreamCount (title_commands.h); checked in native_recorder.h

// One vertex stream of a draw: its data from SetVertexStream's offset on, the bytes there, the stride. data null: unread.
struct DrawStreamSpan {
  const uint8_t* data = nullptr;
  uint64_t size = 0;
  uint32_t stride = 0;
};

// The vertices a draw uses: [first_vertex, first_vertex + vertex_count) of every stream.
struct DrawVertexRange {
  bool valid = false;
  uint32_t first_vertex = 0;
  uint32_t vertex_count = 0;
};

inline DrawVertexRange IndexedVertexRange(const uint8_t* indices, uint32_t index_count, bool index32,
                                          const IndexPolicy& policy, int32_t base_vertex) {
  DrawVertexRange r;
  if (!indices || !index_count) {
    return r;
  }
  const IndexConversion c = ScanIndices(indices, index_count, index32, policy);
  if (c.lo > c.hi) {
    return r;
  }
  r.valid = true;
  r.first_vertex = uint32_t(int64_t(base_vertex) + c.lo);
  r.vertex_count = c.hi - c.lo + 1;
  return r;
}

// The bytes of one stream the range covers, or false when they are outside the stream.
inline bool StreamRangeBytes(const DrawStreamSpan& s, const DrawVertexRange& r, uint64_t& start, uint64_t& bytes) {
  start = uint64_t(r.first_vertex) * s.stride;
  bytes = uint64_t(r.vertex_count) * s.stride;
  return s.data && s.stride && start + bytes <= s.size;
}

// A 64-bit hash of bytes, 8 at a time (no library: the front end, the recorder and the PC test share it).
inline uint64_t HashBytes(uint64_t h, const uint8_t* p, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  for (; i < n; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h ^ (h >> 32) ^ n;
}

// The hash of a draw's data: its indices, then the range of every stream in `mask` (a stream outside its data adds a
// marker, so the recorder and the front end agree on that too).
// Run 60: the range by its values. Runs 49-59 hashed the struct's 12 bytes, three of them the padding after `valid`,
// which neither side writes: the front end's and the recorder's copies held different stack bytes there, so equal draws
// hashed differently. Run 59 measured it: 4,088 draws "changed since submit", 0 of them with a different part (indices,
// streams 0-2, DrawDataPartHashes); every promotion and every rewritten page came from those.
inline uint64_t DrawDataHash(const uint8_t* indices, uint64_t index_bytes, const DrawStreamSpan* streams, uint32_t mask,
                             const DrawVertexRange& r) {
  uint64_t h = HashBytes(0xCBF29CE484222325ull, indices, indices ? index_bytes : 0);
  const uint32_t range_words[3] = {r.valid ? 1u : 0u, r.first_vertex, r.vertex_count};
  h = HashBytes(h, reinterpret_cast<const uint8_t*>(range_words), sizeof(range_words));
  for (uint32_t s = 0; s < kSnapshotStreams; ++s) {
    if (!(mask & (1u << s))) {
      continue;
    }
    uint64_t start = 0, bytes = 0;
    if (StreamRangeBytes(streams[s], r, start, bytes)) {
      h = HashBytes(h, streams[s].data + start, size_t(bytes));
    } else {
      h = (h ^ (0xDEAD0000ull + s)) * 0x9E3779B97F4A7C15ull;
    }
  }
  return h;
}

// Run 59: the hash of each part of a draw's data the recorder can name when a sampled draw changed: the indices, then
// streams 0, 1 and 2 over the same range (a stream outside its data or not in `mask` hashes as 0).
inline std::array<uint32_t, 4> DrawDataPartHashes(const uint8_t* indices, uint64_t index_bytes,
                                                  const DrawStreamSpan* streams, uint32_t mask, const DrawVertexRange& r) {
  std::array<uint32_t, 4> parts{};
  parts[0] = uint32_t(HashBytes(0xCBF29CE484222325ull, indices, indices ? size_t(index_bytes) : 0));
  for (uint32_t s = 0; s < 3; ++s) {
    uint64_t start = 0, bytes = 0;
    if ((mask & (1u << s)) && StreamRangeBytes(streams[s], r, start, bytes)) {
      parts[1 + s] = uint32_t(HashBytes(0xCBF29CE484222325ull, streams[s].data + start, size_t(bytes)));
    }
  }
  return parts;
}

// Run 59: guest memory the title rewrote under a waiting draw, by 64 KiB page, in the view the draw reads it through
// (physical through a fetch constant, virtual through a pointer: IsPhysicalDataAddress).
//
// Run 58 measured the race the copies at submit exist for (run 49) in every A/B phase: of the sampled draws that were
// not copied, 9-17 % had different bytes when the recorder read them than at submit (61 % with the copies off), and the
// front end kept promoting buffers it had never seen (33,650 handles, still thousands a phase): the title draws from
// buffer objects that are new every frame in memory it has just rewritten, so a promotion by handle never comes before
// the next one. A page is marked once a sampled draw read from it changed (the recorder) and stays marked; the front end
// copies at submit every draw whose range touches a marked page, whatever its buffer object.
// Run 60: those run 58 numbers were DrawDataHash's padding bytes (see there), not rewritten bytes; with the hash fixed a
// page is marked only for bytes that really changed, and the mechanism stays for those.
class VolatilePages {
 public:
  static constexpr uint32_t kPageShift = 16;
  static constexpr uint32_t kPagesPerView = 1u << (32 - kPageShift);

  void Mark(bool physical, uint32_t address, uint64_t bytes) {
    if (!bytes) {
      return;
    }
    const uint64_t last = std::min<uint64_t>(uint64_t(address) + bytes - 1, 0xFFFFFFFFull);
    for (uint64_t page = address >> kPageShift; page <= (last >> kPageShift); ++page) {
      const uint64_t bit = (physical ? kPagesPerView : 0) + page;
      const uint64_t mask = uint64_t(1) << (bit & 63);
      if (!(words_[bit >> 6].fetch_or(mask, std::memory_order_relaxed) & mask)) {
        marked_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  bool Any(bool physical, uint32_t address, uint64_t bytes) const {
    if (!bytes || !marked_.load(std::memory_order_relaxed)) {
      return false;
    }
    // A word (64 pages, 4 MiB) at a time: a draw from a large pool buffer tests its whole data on every submit.
    const uint64_t view = physical ? kPagesPerView : 0;
    const uint64_t first = view + (address >> kPageShift);
    const uint64_t last = view + (std::min<uint64_t>(uint64_t(address) + bytes - 1, 0xFFFFFFFFull) >> kPageShift);
    for (uint64_t w = first >> 6; w <= (last >> 6); ++w) {
      uint64_t bits = words_[w].load(std::memory_order_relaxed);
      if (w == (first >> 6)) {
        bits &= ~uint64_t(0) << (first & 63);
      }
      if (w == (last >> 6) && (last & 63) != 63) {
        bits &= (uint64_t(1) << ((last & 63) + 1)) - 1;
      }
      if (bits) {
        return true;
      }
    }
    return false;
  }

  uint64_t marked() const { return marked_.load(std::memory_order_relaxed); }

 private:
  std::array<std::atomic<uint64_t>, 2 * kPagesPerView / 64> words_{};
  std::atomic<uint64_t> marked_{0};
};

// The process's pages: the recorder marks, the front end reads (atomics, no lock).
inline VolatilePages& GlobalVolatilePages() {
  static VolatilePages* pages = new VolatilePages;
  return *pages;
}

// Run 55: a vector whose growth leaves the new bytes as they are (the copy overwrites them): resize() of a plain vector
// zero-fills them first, which run 54's profile of the title's render thread showed (std::__fill_n_a under CaptureDraw).
template <class T>
struct DefaultInitAllocator : std::allocator<T> {
  template <class U>
  struct rebind {
    using other = DefaultInitAllocator<U>;
  };
  DefaultInitAllocator() = default;
  template <class U>
  DefaultInitAllocator(const DefaultInitAllocator<U>&) noexcept {}
  template <class U>
  void construct(U* p) noexcept {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};
using DrawBytes = std::vector<uint8_t, DefaultInitAllocator<uint8_t>>;

// The copy: indices (guest order, from start_index on) and each stream's range (guest order, from first_vertex on).
struct RecorderDrawData {
  DrawBytes bytes;  // the first `used` bytes hold the copy (run 50: a recycled copy keeps its storage)
  // Run 56: the copy in the pool's arena (DrawCopyArena) instead of `bytes`; null: in `bytes`.
  const uint8_t* arena = nullptr;
  uint64_t generation = 0;  // the pool's: bumped at every take, so the arena knows a span of an older copy is free
  size_t used = 0;
  uint32_t index_offset = 0;
  uint32_t index_bytes = 0;
  DrawVertexRange range;
  std::array<uint32_t, kSnapshotStreams> stream_offset{};
  std::array<uint32_t, kSnapshotStreams> stream_bytes{};  // 0: not copied (the recorder reads guest memory)
  const uint8_t* base() const { return arena ? arena : bytes.data(); }
};

// The bytes a copy needs and each stream's part of it.
inline uint64_t DrawCopyLayout(const uint8_t* indices, uint64_t index_bytes, const DrawStreamSpan* streams, uint32_t mask,
                               const DrawVertexRange& r, std::array<uint64_t, kSnapshotStreams>& start,
                               std::array<uint64_t, kSnapshotStreams>& bytes) {
  uint64_t total = indices ? index_bytes : 0;
  for (uint32_t s = 0; s < kSnapshotStreams; ++s) {
    if ((mask & (1u << s)) && StreamRangeBytes(streams[s], r, start[s], bytes[s])) {
      total += bytes[s];
    } else {
      bytes[s] = 0;
    }
  }
  return total;
}

// The copy into `to` (DrawCopyLayout's total bytes), its offsets into `out`.
inline void CopyDrawDataTo(uint8_t* to, uint64_t total, const uint8_t* indices, uint64_t index_bytes,
                           const DrawStreamSpan* streams, const DrawVertexRange& r,
                           const std::array<uint64_t, kSnapshotStreams>& start,
                           const std::array<uint64_t, kSnapshotStreams>& bytes, RecorderDrawData& out) {
  out.used = size_t(total);
  out.range = r;
  out.index_offset = 0;
  out.index_bytes = 0;
  out.stream_offset.fill(0);
  out.stream_bytes.fill(0);
  size_t at = 0;
  if (indices && index_bytes) {
    std::memcpy(to, indices, size_t(index_bytes));
    out.index_offset = 0;
    out.index_bytes = uint32_t(index_bytes);
    at = size_t(index_bytes);
  }
  for (uint32_t s = 0; s < kSnapshotStreams; ++s) {
    if (bytes[s]) {
      std::memcpy(to + at, streams[s].data + start[s], size_t(bytes[s]));
      out.stream_offset[s] = uint32_t(at);
      out.stream_bytes[s] = uint32_t(bytes[s]);
      at += size_t(bytes[s]);
    }
  }
}

inline void CopyDrawData(const uint8_t* indices, uint64_t index_bytes, const DrawStreamSpan* streams, uint32_t mask,
                         const DrawVertexRange& r, RecorderDrawData& out) {
  std::array<uint64_t, kSnapshotStreams> start{}, bytes{};
  const uint64_t total = DrawCopyLayout(indices, index_bytes, streams, mask, r, start, bytes);
  if (out.bytes.size() < total) {
    // Grows only: a recycled copy is not zeroed again. Run 56: emptied first, so the growth does not move the old copy's
    // bytes into the new storage (run 55's profile: std::__uninitialized_copy_a under CaptureDraw).
    out.bytes.clear();
    out.bytes.resize(size_t(total));
  }
  out.arena = nullptr;
  CopyDrawDataTo(out.bytes.data(), total, indices, index_bytes, streams, r, start, bytes, out);
}

// Run 56: one block for every copy at submit, used as a ring. Run 55's pool kept a vector per entry and the entries are
// taken round-robin, so an entry met copies of every size: the bigger ones reallocated (malloc under newlib's one lock,
// the old bytes moved), and with 192 MiB kept the budget let storage go that the next copy allocated again. Run 55's
// stacks of the title's render thread: 23 % of its samples in malloc (the lock wait and _malloc_r), 14.5 % memcpy. Here a
// copy takes the next bytes of the block; a span is free again once its entry left the recorder (the pool's reference
// alone) or was taken for another copy (its generation moved on). Spans are given back oldest first; the packets are
// consumed in order, so the oldest is the first to be done. Full (a copy larger than the free part): the caller copies
// into the entry's own vector, as through run 55, and the count says how often.
class DrawCopyArena {
 public:
  static constexpr size_t kSpans = 8192;  // = DrawDataPool::kMaxFree: an entry holds one span at most

  explicit DrawCopyArena(size_t bytes) : capacity_(bytes) {}
  ~DrawCopyArena() { std::free(block_); }

  // `free_at(span)` says whether span's owner is done with it. Returns null when there is no room.
  template <class FreeAt>
  uint8_t* Allocate(size_t n, uint32_t owner, uint64_t generation, FreeAt free_at) {
    n = (n + 63) & ~size_t(63);
    if (!n || n > capacity_) {
      return nullptr;
    }
    if (!block_) {
      block_ = static_cast<uint8_t*>(std::malloc(capacity_));  // once, at the first copy
      if (!block_) {
        capacity_ = 0;
        return nullptr;
      }
    }
    while (count_ && free_at(spans_[first_])) {
      first_ = (first_ + 1) % kSpans;
      --count_;
    }
    if (count_ == kSpans) {
      return nullptr;
    }
    size_t at;
    if (!count_) {
      at = 0;  // all free: start again at the beginning
    } else {
      // Strictly before the tail: head never meets it while a span is live, so head > tail means "not wrapped".
      const size_t tail = spans_[first_].begin;
      if (head_ > tail) {  // live part [tail, head): room after head, or before tail
        if (head_ + n <= capacity_) {
          at = head_;
        } else if (n < tail) {
          at = 0;
        } else {
          return nullptr;
        }
      } else {  // live part wraps: [tail, end) and [0, head)
        if (head_ + n < tail) {
          at = head_;
        } else {
          return nullptr;
        }
      }
    }
    spans_[(first_ + count_) % kSpans] = {owner, generation, at};
    ++count_;
    head_ = at + n;
    in_use_peak_ = std::max(in_use_peak_, InUse());
    return block_ + at;
  }

  struct Span {
    uint32_t owner = 0;
    uint64_t generation = 0;
    size_t begin = 0;
  };
  size_t capacity() const { return capacity_; }
  size_t spans() const { return count_; }
  size_t in_use_peak() const { return in_use_peak_; }

 private:
  size_t InUse() const {
    if (!count_) {
      return 0;
    }
    const size_t tail = spans_[first_].begin;
    return head_ > tail ? head_ - tail : capacity_ - tail + head_;
  }

  uint8_t* block_ = nullptr;
  size_t capacity_;
  size_t head_ = 0;
  std::array<Span, kSpans> spans_{};
  size_t first_ = 0, count_ = 0;
  size_t in_use_peak_ = 0;
};

// Run 50: the copies recycled. Run 49 allocated one per copied draw on the title's thread (make_shared and a vector of
// ~16 KB) and freed it on the recorder's, through newlib's one malloc lock: 60 % of the draws were copied and the front
// end's time per call went from 5.6 us (run 48) to 12.5 us.
// Run 55: run 50's pool still allocated a shared_ptr control block for every copy (freed by the recorder) and dropped the
// storage of copies over 64 KiB, which the next one allocated and zero-filled again; run 54's profile of the title's
// render thread: 13 % of its samples waiting for newlib's malloc lock (the recorder freeing at the same time), plus
// malloc, free and the zero fill. Now each entry is a shared_ptr made once and kept here: the recorder only drops its
// reference (an atomic decrement, no free, no lock of ours) and an entry whose count is back to 1 is taken again with
// its storage. The packets are consumed in order, so the round-robin's next entry is the oldest. Storage over
// kKeptBudget in all is let go at the next take of an entry over kSmallKeep.
class DrawDataPool {
 public:
  static constexpr size_t kMaxFree = 8192;            // entries at most (the ring holds 4,096 packets)
  static constexpr size_t kSmallKeep = 64u << 10;     // storage always kept
  static constexpr size_t kKeptBudget = 192u << 20;   // storage kept in all, beyond the small ones

  static constexpr size_t kArenaBytes = 128u << 20;   // run 56: DrawCopyArena

  std::shared_ptr<RecorderDrawData> Take() {
    std::lock_guard<std::mutex> lock(mutex_);  // the front end's threads only: never contended by the recorder
    return TakeLocked().first;
  }

  // Run 56: an entry holding the copy of the draw's data, in the arena when it has room, else in the entry's vector.
  std::shared_ptr<RecorderDrawData> TakeCopy(const uint8_t* indices, uint64_t index_bytes, const DrawStreamSpan* streams,
                                             uint32_t mask, const DrawVertexRange& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [data, index] = TakeLocked();
    std::array<uint64_t, kSnapshotStreams> start{}, bytes{};
    const uint64_t total = DrawCopyLayout(indices, index_bytes, streams, mask, r, start, bytes);
    uint8_t* to = nullptr;
    if (index != kNoIndex && total) {
      to = arena_.Allocate(size_t(total), index, data->generation, [this](const DrawCopyArena::Span& s) {
        const Entry& e = entries_[s.owner];
        if (e.data->generation != s.generation || e.data.use_count() == 1) {
          std::atomic_thread_fence(std::memory_order_acquire);  // the recorder's reads of that copy are done
          return true;
        }
        return false;
      });
    }
    if (to) {
      CopyDrawDataTo(to, total, indices, index_bytes, streams, r, start, bytes, *data);
      data->arena = to;
      ++arena_copies_;
    } else {
      CopyDrawData(indices, index_bytes, streams, mask, r, *data);
      arena_full_ += total != 0;
    }
    return data;
  }

  uint64_t arena_copies() {
    std::lock_guard<std::mutex> lock(mutex_);
    return arena_copies_;
  }
  uint64_t arena_full() {
    std::lock_guard<std::mutex> lock(mutex_);
    return arena_full_;
  }
  size_t arena_peak() {
    std::lock_guard<std::mutex> lock(mutex_);
    return arena_.in_use_peak();
  }

  uint64_t created() const { return created_.load(std::memory_order_relaxed); }
  uint64_t reused() {
    std::lock_guard<std::mutex> lock(mutex_);
    return reused_;
  }
  // Entries no packet holds.
  size_t free_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t idle = 0;
    for (const Entry& e : entries_) {
      idle += e.data.use_count() == 1;
    }
    return idle;
  }
  size_t kept_bytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return kept_;
  }

 private:
  static constexpr uint32_t kNoIndex = UINT32_MAX;
  struct Entry {
    std::shared_ptr<RecorderDrawData> data;
    size_t capacity = 0;  // its storage when last taken
  };

  // Caller holds mutex_. The entry and its index in entries_ (kNoIndex: not kept, the pool is full).
  std::pair<std::shared_ptr<RecorderDrawData>, uint32_t> TakeLocked() {
    const size_t n = entries_.size();
    for (size_t tried = 0; tried < n; ++tried) {
      const size_t at = next_;
      Entry& e = entries_[at];
      next_ = next_ + 1 == n ? 0 : next_ + 1;
      if (e.data.use_count() != 1) {
        continue;  // still in a packet
      }
      // The recorder's release (acq_rel) happened before this load saw 1: its reads of the old copy are done.
      std::atomic_thread_fence(std::memory_order_acquire);
      const size_t capacity = e.data->bytes.capacity();
      kept_ += capacity - e.capacity;  // what the last copy into it grew
      e.capacity = capacity;
      if (kept_ > kKeptBudget && capacity > kSmallKeep) {
        DrawBytes().swap(e.data->bytes);
        kept_ -= capacity;
        e.capacity = 0;
      }
      e.data->arena = nullptr;
      ++e.data->generation;  // an arena span of its last copy is free from now on
      ++reused_;
      return {e.data, uint32_t(at)};
    }
    created_.fetch_add(1, std::memory_order_relaxed);
    auto data = std::make_shared<RecorderDrawData>();
    if (n < kMaxFree) {
      entries_.push_back({data, 0});
      next_ = 0;
      return {data, uint32_t(n)};
    }
    return {data, kNoIndex};
  }

  std::mutex mutex_;
  std::vector<Entry> entries_;
  size_t next_ = 0;
  size_t kept_ = 0;
  uint64_t reused_ = 0;
  std::atomic<uint64_t> created_{0};
  DrawCopyArena arena_{kArenaBytes};
  uint64_t arena_copies_ = 0, arena_full_ = 0;
};

// The process's pool: never destroyed, so a packet still in flight at exit returns to a live pool.
inline DrawDataPool& GlobalDrawDataPool() {
  static DrawDataPool* pool = new DrawDataPool;
  return *pool;
}

}  // namespace rex::graphics::gta4_native
