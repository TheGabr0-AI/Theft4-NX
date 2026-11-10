// deko3d recorder: vertex data kept across frames. See dk_geometry_store.h (port of nfsmw-nx's nfsmw_dk_geometria.cpp).

#include "dk_geometry_store.h"

#include <xxhash.h>

#include <algorithm>

namespace rex::graphics::gta4_native::dk {

namespace {

// Blocks in 64-byte steps: fewer, larger free pieces, every block aligned for vertex fetches.
constexpr uint32_t kStep = 64;

constexpr uint32_t RoundUp(uint32_t v) { return (v + kStep - 1) & ~(kStep - 1); }

// splitmix64 of the identity: the keys are aligned host pointers, which a plain xor would cluster.
uint64_t Key(uint64_t data, uint32_t bytes, uint64_t mode) {
  uint64_t x = data ^ (uint64_t(bytes) * 0x9E3779B97F4A7C15ull) ^ (mode * 0xD1B54A32D192ED03ull);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// Content hashes in 64-byte lines: the whole range (every line, in order) and each phase p (the lines i with
// i % kPhases == p, then the first and the last), all from the same line hashes in one pass.
constexpr uint32_t kLine = 64;

uint64_t Fold(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}

uint64_t LineHash(const uint8_t* data, uint32_t bytes, uint32_t i) {
  const uint32_t start = i * kLine;
  return XXH3_64bits(data + start, std::min(kLine, bytes - start));
}

}  // namespace

uint64_t GeometryStore::Hashes(const uint8_t* data, uint32_t bytes, uint64_t* phases) {
  const uint32_t lines = (bytes + kLine - 1) / kLine;
  if (lines <= kPhases) {
    return XXH3_64bits(data, bytes);
  }
  uint64_t whole = 0x243F6A8885A308D3ull;
  for (uint32_t p = 0; p < kPhases; ++p) {
    phases[p] = 0x13198A2E03707344ull + p;
  }
  uint64_t first = 0, last = 0;
  for (uint32_t i = 0; i < lines; ++i) {
    const uint64_t h = LineHash(data, bytes, i);
    whole = Fold(whole, h);
    phases[i % kPhases] = Fold(phases[i % kPhases], h);
    if (i == 0) {
      first = h;
    }
    last = h;
  }
  for (uint32_t p = 0; p < kPhases; ++p) {
    phases[p] = Fold(Fold(phases[p], first), last);
  }
  return whole;
}

uint64_t GeometryStore::PhaseHash(const uint8_t* data, uint32_t bytes, uint32_t phase, uint64_t& read) {
  const uint32_t lines = (bytes + kLine - 1) / kLine;
  uint64_t h = 0x13198A2E03707344ull + phase;
  for (uint32_t i = phase; i < lines; i += kPhases) {
    h = Fold(h, LineHash(data, bytes, i));
    read += kLine;
  }
  read += 2 * kLine;
  return Fold(Fold(h, LineHash(data, bytes, 0)), LineHash(data, bytes, lines - 1));
}

void GeometryStore::Clear() {
  cpu_ = nullptr;
  gpu_ = 0;
  table_.clear();
  fast_.fill({});
  free_.clear();
  pending_.clear();
  signals_.clear();
  distrusted_.clear();
  used_bytes_ = 0;
  total_bytes_ = 0;
}

bool GeometryStore::Done(uint64_t work) {
  // Never the work being recorded: its slot's fence is still that slot's previous work's.
  if (work == 0 || work <= done_up_to_) {
    return true;
  }
  if (work >= open_work_) {
    return false;
  }
  // Opening work A waited for the fence of A - slots (the previous work of its slot): everything up to there is done.
  // Works finish in order: advance the mark while they are done.
  if (open_work_ > slots_) {
    done_up_to_ = std::max<uint64_t>(done_up_to_, open_work_ - slots_);
  }
  while (done_up_to_ + 1 < open_work_ && work_done_ && work_done_(done_up_to_ + 1)) {
    ++done_up_to_;
  }
  return work <= done_up_to_;
}

void GeometryStore::NewWork(uint64_t open_work) {
  open_work_ = open_work;
  ++epoch_;
  if (!active()) {
    return;
  }
  while (!pending_.empty() && Done(pending_.front().work)) {
    Return(pending_.front().offset, pending_.front().size);
    pending_.pop_front();
  }
  for (size_t i = 0; i < pending_.size();) {
    if (Done(pending_[i].work)) {
      Return(pending_[i].offset, pending_[i].size);
      pending_.erase(pending_.begin() + std::ptrdiff_t(i));
    } else {
      ++i;
    }
  }
}

void GeometryStore::NewFrame(uint64_t frame) {
  frame_ = frame;
  ++epoch_;
  if (active() && frame_ - swept_ >= 128) {
    swept_ = frame_;
    Sweep();
  }
}

GeometryStore::Result GeometryStore::Lookup(const uint8_t* data, uint64_t key_address, uint32_t bytes, uint64_t mode,
                                            uint32_t host_bytes, uint32_t owner, uint8_t*& target) {
  target = nullptr;
  if (!active() || !bytes || host_bytes > total_bytes_ / 8) {
    return {};
  }
  ++stats_.lookups;
  const uint64_t address = key_address;
  const uint64_t key = Key(address, bytes, mode);
  // A direct-mapped cache in front of the table (the map's nodes never move; Sweep, the only erase, empties it).
  Fast& fast = fast_[key & (kFast - 1)];
  Entry* found = nullptr;
  bool fresh = false;
  if (fast.entry && fast.key == key) {
    found = fast.entry;
  } else {
    auto [it, inserted] = table_.try_emplace(key);
    found = &it->second;
    fresh = inserted;
    fast = {key, found};
  }
  Entry& e = *found;
  if (!fresh && (e.data != address || e.bytes != bytes || e.mode != mode)) {
    ++stats_.collisions;
    ReleaseBlock(e);
    e = Entry{};
    fresh = true;
  }
  e.frame = frame_;
  if (fresh) {
    e.data = address;
    e.bytes = bytes;
    e.mode = mode;
  } else if (options_.trust_epoch && !(mode >> 63) && e.epoch == epoch_) {  // bit 63: never trusted (run 53)
    if (e.size) {
      ++stats_.hits;
      ++stats_.trusted_hits;
      stats_.hit_bytes += bytes;
      return {&e, gpu_ + e.offset, e.lo, e.hi};
    }
    return {};
  } else if (options_.trust_signals && owner && !(mode >> 63) && e.size && !e.shared && e.owner == owner &&
             frame_ - e.checked_frame < std::max<uint32_t>(options_.trust_frames, 1) && e.signal == SignalOf(owner) &&
             (e.checked_frame == frame_ || !distrusted_.count(owner))) {
    // Run 64: checked in this frame through this buffer object, which the title has not unlocked after a write nor
    // released since (the stream's order): the stored block is the range as the draw's submit saw it.
    e.epoch = epoch_;  // Written() starts an epoch, so the rest of this one needs no signal lookup
    ++stats_.hits;
    ++stats_.signal_hits;
    stats_.frame_hits += e.checked_frame != frame_;
    stats_.hit_bytes += bytes;
    return {&e, gpu_ + e.offset, e.lo, e.hi};
  } else if (e.volatile_until > frame_) {
    ++stats_.volatile_lookups;
    return {};
  }
  e.epoch = epoch_;
  // Run 64: this lookup hashes the range. Whether a change found below was announced (the owner's signal moved since
  // the last check), and the check's owner, signal and frame for the lookups that follow.
  const bool owned = owner != 0 && !(mode >> 63);
  const uint64_t signal = owned ? SignalOf(owner) : 0;
  const bool unannounced = owned && !fresh && !e.shared && e.owner == owner && e.signal == signal;
  if (owned) {
    e.shared = e.shared || (e.owner != 0 && e.owner != owner);
    e.owner = owner;
    e.signal = signal;
    e.checked_frame = frame_;
  }
  if (e.size && e.phases_valid && options_.sampling) {
    const uint32_t phase = uint32_t((frame_ + (e.data >> 6)) % kPhases);
    uint64_t read = 0;
    const bool sample_same = PhaseHash(data, bytes, phase, read) == e.phases[phase];
    stats_.hashed_bytes += read;
    ++stats_.samples;
    if (sample_same && stats_.samples > kGuardSamples) {
      e.changes = 0;
      ++stats_.hits;
      stats_.hit_bytes += bytes;
      return {&e, gpu_ + e.offset, e.lo, e.hi};
    }
    if (sample_same) {
      uint64_t phases[kPhases];
      if (Hashes(data, bytes, phases) == e.hash) {
        stats_.hashed_bytes += bytes;
        e.changes = 0;
        ++stats_.hits;
        stats_.hit_bytes += bytes;
        return {&e, gpu_ + e.offset, e.lo, e.hi};
      }
      ++stats_.disagreements;
      options_.sampling = false;
    }
  }
  uint64_t phases[kPhases] = {};
  // Without sampling the phase hashes are never read: one XXH3 pass over the range (run 53).
  const uint64_t hash = options_.sampling ? Hashes(data, bytes, phases) : XXH3_64bits(data, bytes);
  stats_.hashed_bytes += bytes;
  const bool with_phases = options_.sampling && (bytes + kLine - 1) / kLine > kPhases;
  if (fresh) {
    e.hash = hash;  // first sighting: the ring this time
    e.phases_valid = with_phases;
    std::copy(phases, phases + kPhases, e.phases);
    return {};
  }
  if (hash == e.hash) {
    if (with_phases && !e.phases_valid) {
      e.phases_valid = true;
      std::copy(phases, phases + kPhases, e.phases);
    }
    e.changes = 0;
    if (e.size) {
      ++stats_.hits;
      stats_.hit_bytes += bytes;
      return {&e, gpu_ + e.offset, e.lo, e.hi};
    }
    // The second sighting with the same content: into the store.
    if (!Reserve(host_bytes, e)) {
      return {};
    }
    target = cpu_ + e.offset;
    ++stats_.admitted;
    stats_.admitted_bytes += host_bytes;
    return {&e, gpu_ + e.offset, e.lo, e.hi};
  }
  // Changed: a new block next time it is stable; changing again, to the ring for a while.
  ++stats_.changes;
  if (unannounced) {
    // Run 65: this object's bytes changed with no unlock or release in the stream: its ranges every frame from now on,
    // and after kUnsignalledLimit such changes every range (the signals are not complete in this session).
    ++stats_.changes_unsignalled;
    stats_.distrusted += distrusted_.insert(owner).second;
    if (stats_.changes_unsignalled >= kUnsignalledLimit) {
      options_.trust_frames = 1;
    }
  }
  e.hash = hash;
  e.phases_valid = with_phases;
  std::copy(phases, phases + kPhases, e.phases);
  ReleaseBlock(e);
  if (++e.changes >= 2) {
    e.volatile_until = frame_ + kVolatileFrames;
  }
  return {};
}

bool GeometryStore::Reserve(uint32_t bytes, Entry& e) {
  const uint32_t size = RoundUp(bytes);
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t k = 0; k < free_.size(); ++k) {
      auto& [start, length] = free_[k];
      if (length < size) {
        continue;
      }
      e.offset = start;
      e.size = size;
      start += size;
      length -= size;
      if (!length) {
        free_.erase(free_.begin() + std::ptrdiff_t(k));
      }
      used_bytes_ += size;
      return true;
    }
    if (pass == 0) {
      Evict(size);
    }
  }
  ++stats_.no_room;
  return false;
}

void GeometryStore::Return(uint32_t offset, uint32_t size) {
  auto it = std::lower_bound(free_.begin(), free_.end(), std::make_pair(offset, 0u));
  it = free_.insert(it, {offset, size});
  if (it + 1 != free_.end() && it->first + it->second == (it + 1)->first) {
    it->second += (it + 1)->second;
    free_.erase(it + 1);
  }
  if (it != free_.begin() && (it - 1)->first + (it - 1)->second == it->first) {
    (it - 1)->second += it->second;
    free_.erase(it);
  }
}

void GeometryStore::ReleaseBlock(Entry& e) {
  if (!e.size) {
    return;
  }
  used_bytes_ -= e.size;
  if (Done(e.work)) {
    Return(e.offset, e.size);
  } else {
    pending_.push_back({e.offset, e.size, e.work});
  }
  e.size = 0;
  e.offset = 0;
  e.work = 0;
}

void GeometryStore::Evict(uint32_t needed) {
  // Blocks not used in this frame or the last, oldest first, until an eighth of the store (or what is needed) is free.
  std::vector<std::pair<uint64_t, Entry*>> candidates;
  for (auto& [key, e] : table_) {
    if (e.size && e.frame + 2 <= frame_) {
      candidates.emplace_back(e.frame, &e);
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const uint64_t goal = std::max<uint64_t>(needed, total_bytes_ / 8);
  uint64_t freed = 0;
  for (const auto& [frame, e] : candidates) {
    if (freed >= goal) {
      break;
    }
    freed += e->size;
    ++stats_.evictions;
    ReleaseBlock(*e);
    e->epoch = 0;
  }
  for (size_t i = 0; i < pending_.size();) {
    if (Done(pending_[i].work)) {
      Return(pending_[i].offset, pending_[i].size);
      pending_.erase(pending_.begin() + std::ptrdiff_t(i));
    } else {
      ++i;
    }
  }
}

void GeometryStore::Sweep() {
  fast_.fill({});
  for (auto it = table_.begin(); it != table_.end();) {
    if (!it->second.size && it->second.frame + 256 < frame_) {
      it = table_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace rex::graphics::gta4_native::dk
