// deko3d recorder: vertex data kept across frames, the geometry store (16-deko3d-devlog.md, run 52).
//
// Ported from nfsmw-nx's deko3d renderer (StevensND, github.com/StevensND/nfsmw-nx, app/src/nfsmw_dk_geometria.h/.cpp,
// "AlmacenGeometria"; reused with the author's permission), names in English, on our Core.
//
// Why: run 50's recorder spent 18-19 of its 32 us per draw on vertices: every draw byte-swapped its [first, first +
// count) range of every stream into the CPU-uncached upload ring, and nfsmw measured on the same kind of title that
// ~99 % of those bytes repeat (within the frame and from earlier frames).
//
// What it does: a persistent block (CPU-uncached, GPU-cached, as the ring) holds the host form of guest ranges. A range
// is identified by its host pointer, its guest byte count and its mode (here: stride, and the vertex fix-ups that
// change its host form), its content by a hash of its guest bytes (XXH3 per 64-byte line, folded). A range seen twice
// with the same hash is written into the store; from then on a lookup with the same hash binds the stored block: no
// copy, no byte swap, no ring space.
//
// Exactness: the hash is of the guest bytes when the draw is recorded, which is what the ring copy reads too. A stored
// block is never written while the GPU may read it: a changed range gets a new block, and the old one is returned
// only when every work that bound it is done (Core::WorkDone; never the work being recorded). nfsmw also trusts a
// range for the rest of an epoch once it is checked, and checks stored ranges by a rotating 1/8 sample; both are
// options here and OFF by default (trust_epoch, sampling): every lookup hashes the whole range. Run 52 measured what
// that costs: 35.8 GB hashed to avoid copying 35.2 GB, 12 content changes in 2.5 M lookups. Run 53's recorder turns
// trust_epoch on (cvar nx_dk_geometry_trust; an epoch is one work, one frame or one guest wait) and, without sampling,
// hashes a range in one XXH3 pass instead of per 64-byte line (the line hashes are only needed for the samples).
//
// Run 64 (trust_signals, cvar nx_dk_geometry_signals): run 63 hashed 188 GB in 23.7 M lookups to find 448 changes,
// because an epoch ends at every clear, resolve, hand-off and resource event, none of which writes a buffer. The title
// writes a vertex or index buffer between Lock and Unlock, and the hooks send that unlock (guest write) and the
// object's release in the command stream, in order with the draws. A range read through buffer object `owner` and
// checked in this frame is therefore bound without hashing again until Written(owner) says the object was unlocked
// after a write or released. This is the content at the draw's submit: a re-hash at record time can see bytes the
// title wrote after submitting the draw. Never applied to: the front end's submit copies (mode bit 63), a stream the
// title holds locked at the draw (the caller passes owner 0), a range reached through two different objects, a range
// not yet checked in this frame (every range is still hashed once a frame: a write that passed no Lock is found then,
// and counted apart as `changes_unsignalled`).
//
// Run 65 (trust_frames, cvar nx_dk_geometry_trust_frames): run 64 on the console counted 0 content changes without a
// signal of 371 in 868 s, and the per-frame check still hashed 14.4 M of 25.6 M lookups (a range is mostly drawn once a
// frame). A range checked through its object within the last `trust_frames` frames, the object unsignalled since, is
// bound without hashing; it is hashed again every `trust_frames` frames. A change found without a signal marks that
// object (its ranges are checked every frame from then on), and the eighth such change ends the trust across frames
// for every range: the measure that allowed it no longer holds.
//
// Not kept: ranges seen once (admitted the second time), and changing ranges (changed twice in a row: to the ring,
// unhashed, for 64 frames). When full, the ranges unused longest go first, never one used in this frame or the last.
// One thread: the recorder's.

#pragma once

#include <cstdint>

#if __has_include(<deko3d.h>)
#include <deko3d.h>
#else
using DkGpuAddr = uint64_t;  // tests/deko3d/test_run52.py builds this on the PC
#endif

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rex::graphics::gta4_native::dk {

class Core;

class GeometryStore {
 public:
  struct Entry;

  struct Result {
    Entry* entry = nullptr;
    DkGpuAddr gpu = 0;  // 0: not in the store, the caller uses the ring as before
    // Index ranges (run 53, nfsmw's vmin/vmax): the smallest and largest index the conversion found when the range was
    // stored; valid on a hit. On the admission the caller computes them and keeps them with SetRange.
    uint32_t lo = 0, hi = 0;
  };

  struct Stats {
    uint64_t lookups = 0;
    uint64_t hits = 0;            // bound from the store
    uint64_t hit_bytes = 0;       // guest bytes not copied thanks to it
    uint64_t hashed_bytes = 0;
    uint64_t admitted = 0;        // ranges written into the store
    uint64_t admitted_bytes = 0;
    uint64_t changes = 0;         // ranges whose content changed
    uint64_t volatile_lookups = 0;  // sent to the ring unhashed (changing data)
    uint64_t no_room = 0;
    uint64_t evictions = 0;
    uint64_t collisions = 0;
    uint64_t samples = 0;
    uint64_t disagreements = 0;   // a sample missed a change the whole hash saw (switches sampling off)
    uint64_t trusted_hits = 0;    // hits answered without hashing (trust_epoch: already checked in this epoch)
    uint64_t signal_hits = 0;     // hits answered without hashing (trust_signals: checked this frame, owner not written)
    uint64_t signals = 0;         // Written() calls
    uint64_t changes_unsignalled = 0;  // of `changes`: an owned range whose owner had no write signal since its check
    uint64_t frame_hits = 0;      // of `signal_hits`: checked in an earlier frame (trust_frames, run 65)
    uint64_t distrusted = 0;      // buffer objects checked every frame again after a change without a signal
  };

  struct Options {
    uint32_t mb = 96;           // 0 = off
    bool trust_epoch = false;   // nfsmw: a range checked once is trusted until the next work, frame or guest wait
    bool sampling = false;      // nfsmw: stored ranges rechecked by a rotating 1/8 of their lines
    bool trust_signals = false;  // run 64: checked once a frame, again after its buffer object's write signal
    uint32_t trust_frames = 1;   // run 65: with trust_signals, frames between two checks of an unsignalled range
  };

  GeometryStore() = default;
  GeometryStore(const GeometryStore&) = delete;
  GeometryStore& operator=(const GeometryStore&) = delete;

  // `create_block(bytes)` makes the store's memory block (CPU-uncached, GPU-cached); `work_done(work)` says whether a
  // work's GPU commands have finished; `slots` is the Core's work slot count. Tests pass plain memory and lambdas.
  template <class CreateBlock, class WorkDone>
  bool Create(const Options& options, uint32_t slots, CreateBlock&& create_block, WorkDone&& work_done) {
    options_ = options;
    slots_ = slots ? slots : 1;
    work_done_ = std::forward<WorkDone>(work_done);
    if (!options.mb) {
      return true;
    }
    total_bytes_ = uint64_t(options.mb) << 20;
    std::pair<uint8_t*, DkGpuAddr> block = create_block(uint32_t(total_bytes_));
    cpu_ = block.first;
    gpu_ = block.second;
    if (!cpu_) {
      total_bytes_ = 0;
      return false;
    }
    free_.assign(1, {0u, uint32_t(total_bytes_)});
    table_.reserve(1u << 15);
    return true;
  }
  void Clear();  // after the GPU is idle
  bool active() const { return cpu_ != nullptr; }

  // A work was opened (its number).
  void NewWork(uint64_t open_work);
  void NewFrame(uint64_t frame);
  void Forget() { ++epoch_; }  // a guest wait for the GPU
  // Run 64: the title unlocked buffer object `owner` after writing it, or released it (the command stream's order).
  void Written(uint32_t owner) {
    ++epoch_;
    ++stats_.signals;
    if (owner) {
      signals_[owner] = ++signal_serial_;
    }
  }

  // Looks the range up (`bytes` guest bytes at `data`, its host form `host_bytes` bytes). When it is admitted now,
  // write(uint8_t* target) writes its host form into the store (in order, once: the memory is CPU-uncached).
  template <class Write>
  Result Find(const uint8_t* data, uint32_t bytes, uint64_t mode, uint32_t host_bytes, Write&& write) {
    return Find(data, uint64_t(reinterpret_cast<uintptr_t>(data)), bytes, mode, host_bytes, write);
  }
  // Run 53: the range identified by `key` instead of the host pointer of its bytes: the front end's submit copies
  // live at a new host address every draw, so they are keyed by their guest address (the caller marks the mode).
  template <class Write>
  Result Find(const uint8_t* data, uint64_t key, uint32_t bytes, uint64_t mode, uint32_t host_bytes, Write&& write) {
    return Find(data, key, bytes, mode, host_bytes, 0u, write);
  }
  // Run 64: `owner` is the guest buffer object the range is read through (0: unknown, or locked at the draw: the range
  // is hashed as before).
  template <class Write>
  Result Find(const uint8_t* data, uint64_t key, uint32_t bytes, uint64_t mode, uint32_t host_bytes, uint32_t owner,
              Write&& write) {
    uint8_t* target = nullptr;
    Result r = Lookup(data, key, bytes, mode, host_bytes, owner, target);
    if (target) {
      write(target);
    }
    return r;
  }
  void SetRange(Entry* e, uint32_t lo, uint32_t hi) {
    if (e) {
      e->lo = lo;
      e->hi = hi;
    }
  }
  // The draw binds it in work `work` (the one being recorded).
  void Use(Entry* e, uint64_t work) {
    if (e) {
      e->work = work;
    }
  }

  const Stats& stats() const { return stats_; }
  uint64_t used_bytes() const { return used_bytes_; }
  uint64_t total_bytes() const { return total_bytes_; }
  size_t entries() const { return table_.size(); }
  uint32_t trust_frames() const { return options_.trust_frames; }  // 1 once the trust across frames has ended
  static constexpr uint64_t kUnsignalledLimit = 8;

  struct Entry {
    uint64_t data = 0;
    uint64_t mode = 0;
    uint64_t hash = 0;
    uint64_t epoch = 0;
    uint64_t frame = 0;
    uint64_t work = 0;            // the last work that bound its block (0: never bound)
    uint64_t volatile_until = 0;  // up to this frame it goes to the ring unhashed
    uint32_t bytes = 0;
    uint32_t offset = 0, size = 0;  // its block (size 0: none)
    uint32_t changes = 0;
    uint32_t lo = 0, hi = 0;  // index ranges: SetRange
    // Run 64: the buffer object the range was last checked through, that object's write signal and the frame then;
    // `shared`: reached through two different objects (never trusted by signal).
    uint32_t owner = 0;
    bool shared = false;
    uint64_t signal = 0;
    uint64_t checked_frame = 0;
    bool phases_valid = false;
    uint64_t phases[8] = {};
  };

  static constexpr uint64_t kVolatileFrames = 64;
  static constexpr uint32_t kPhases = 8;
  static constexpr uint64_t kGuardSamples = 20000;

  // The whole hash of a range and, with more than kPhases lines, the hash of each phase into phases[].
  static uint64_t Hashes(const uint8_t* data, uint32_t bytes, uint64_t* phases);
  static uint64_t PhaseHash(const uint8_t* data, uint32_t bytes, uint32_t phase, uint64_t& read);

 private:
  Result Lookup(const uint8_t* data, uint64_t key_address, uint32_t bytes, uint64_t mode, uint32_t host_bytes,
                uint32_t owner, uint8_t*& target);
  uint64_t SignalOf(uint32_t owner) const {
    const auto it = signals_.find(owner);
    return it != signals_.end() ? it->second : 0;
  }
  bool Reserve(uint32_t bytes, Entry& e);
  void ReleaseBlock(Entry& e);
  void Return(uint32_t offset, uint32_t size);
  bool Done(uint64_t work);
  void Evict(uint32_t needed);
  void Sweep();

  Options options_;
  uint32_t slots_ = 1;
  std::function<bool(uint64_t)> work_done_;
  uint8_t* cpu_ = nullptr;
  DkGpuAddr gpu_ = 0;
  uint64_t total_bytes_ = 0, used_bytes_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> free_;  // offset, bytes; sorted by offset
  struct Pending {
    uint32_t offset, size;
    uint64_t work;
  };
  std::deque<Pending> pending_;
  std::unordered_map<uint64_t, Entry> table_;
  struct Fast {
    uint64_t key = 0;
    Entry* entry = nullptr;
  };
  static constexpr size_t kFast = 8192;
  std::array<Fast, kFast> fast_{};
  uint64_t epoch_ = 1;
  uint64_t frame_ = 1;
  uint64_t open_work_ = 0;
  uint64_t done_up_to_ = 0;
  uint64_t swept_ = 0;
  std::unordered_map<uint32_t, uint64_t> signals_;  // buffer object -> the serial of its last write signal
  uint64_t signal_serial_ = 0;
  std::unordered_set<uint32_t> distrusted_;  // run 65: objects that changed without a signal
  Stats stats_;
};

}  // namespace rex::graphics::gta4_native::dk
