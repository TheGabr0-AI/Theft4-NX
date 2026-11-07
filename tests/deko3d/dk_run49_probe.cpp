// Run 49 probe (devlog section V): native_index_policy.h and native_draw_snapshot.h on the PC.
//
// 1. Index policy: ConvertIndices and ScanIndices against a reference written from Gta4NativeGraphicsSystem's rules
//    (graphics_system.cpp: ConvertGuestIndices masks a 32-bit index to xenos::kVertexIndexMask; the indexed draw turns
//    restart on only for line strips, triangle strips and fans, and for 16-bit indices only when the reset index fits;
//    it writes all ones where the masked index equals the reset index), over random draws; and the run-48 recorder's
//    rules (no mask, restart on every primitive, reset index cut to 16 bits) to count where they differ.
// 2. Snapshot: random draws over fake guest buffers; the hash at "submit" equals the hash at "record" when nothing
//    changed (no false alarm), differs when a byte inside the draw's range changed, equals when a byte outside it did;
//    the copy holds the submitted bytes after the guest memory changed.
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "native_draw_snapshot.h"

using namespace rex::graphics::gta4_native;

namespace {

std::mt19937 rng(49);

uint32_t Rand(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); }

void PutBE(uint8_t* p, uint32_t v, bool index32) {
  if (index32) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  } else {
    p[0] = uint8_t(v >> 8), p[1] = uint8_t(v);
  }
}

uint32_t GetBE(const uint8_t* p, bool index32) {
  return index32 ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]
                 : (uint32_t(p[0]) << 8) | p[1];
}

}  // namespace

int main() {
  // --- 1. index policy
  uint64_t index_errors = 0, scan_errors = 0, draws = 0, run48_differs = 0, masked_total = 0, restart_on = 0;
  for (int round = 0; round < 20000; ++round) {
    const bool index32 = Rand(2);
    const uint32_t primitive = std::array<uint32_t, 8>{1, 2, 3, 4, 5, 6, 13, 8}[Rand(8)];
    const bool enabled = Rand(2);
    const uint32_t reset = std::array<uint32_t, 4>{0xFFFF, 0xFFFFFF, 0x1234, 0x10000}[Rand(4)];
    const uint32_t count = 1 + Rand(300);
    std::vector<uint8_t> src(count * (index32 ? 4 : 2));
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = Rand(4) == 0 ? reset : Rand(5000);
      if (index32 && Rand(8) == 0) v |= uint32_t(Rand(255) + 1) << 24;  // a top byte
      if (!index32) v &= 0xFFFF;
      PutBE(src.data() + i * (index32 ? 4 : 2), v, index32);
    }
    // Reference (LRA).
    const bool strip = primitive == 3 || primitive == 5 || primitive == 6;
    const bool ref_restart = enabled && strip && (index32 || reset <= 0xFFFF);
    std::vector<uint32_t> ref(count);
    uint32_t ref_lo = UINT32_MAX, ref_hi = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = GetBE(src.data() + i * (index32 ? 4 : 2), index32);
      if (index32) v &= 0xFFFFFF;
      const bool is_restart = ref_restart && (index32 ? v == (reset & 0xFFFFFF) : uint16_t(v) == uint16_t(reset));
      ref[i] = is_restart ? (index32 ? 0xFFFFFFFFu : 0xFFFFu) : v;
      if (!is_restart) ref_lo = std::min(ref_lo, v), ref_hi = std::max(ref_hi, v);
    }
    const IndexPolicy p = IndexPolicyFor(primitive, enabled, reset, index32);
    std::vector<uint8_t> dst(src.size());
    const IndexConversion c = ConvertIndices(src.data(), count, index32, p, dst.data());
    const IndexConversion s = ScanIndices(src.data(), count, index32, p);
    ++draws;
    restart_on += p.restart;
    masked_total += c.masked;
    bool bad = p.restart != ref_restart || c.lo != ref_lo || c.hi != ref_hi;
    for (uint32_t i = 0; i < count && !bad; ++i) {
      uint32_t v;
      if (index32) {
        std::memcpy(&v, dst.data() + i * 4, 4);
      } else {
        uint16_t h;
        std::memcpy(&h, dst.data() + i * 2, 2);
        v = h;
      }
      bad = v != ref[i];
    }
    index_errors += bad;
    scan_errors += s.lo != c.lo || s.hi != c.hi;
    // Run 48's recorder: lo/hi with the unmasked value and the raw reset index on every primitive.
    uint32_t lo48 = UINT32_MAX, hi48 = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = GetBE(src.data() + i * (index32 ? 4 : 2), index32);
      if (enabled && v == reset) continue;
      lo48 = std::min(lo48, v), hi48 = std::max(hi48, v);
    }
    run48_differs += lo48 != c.lo || hi48 != c.hi;
  }
  std::printf("INDEX draws %llu errors %llu scan_errors %llu restart_on %llu masked %llu run48_range_differs %llu\n",
              (unsigned long long)draws, (unsigned long long)index_errors, (unsigned long long)scan_errors,
              (unsigned long long)restart_on, (unsigned long long)masked_total, (unsigned long long)run48_differs);

  // --- 2. snapshot and hash
  uint64_t false_alarm = 0, missed_inside = 0, alarm_outside = 0, copy_errors = 0, rounds = 0, outside_rounds = 0;
  for (int round = 0; round < 10000; ++round) {
    const uint32_t streams = 1 + Rand(3);
    std::vector<std::vector<uint8_t>> mem(streams);
    std::array<DrawStreamSpan, kSnapshotStreams> spans{};
    uint32_t mask = 0;
    for (uint32_t s = 0; s < streams; ++s) {
      const uint32_t stride = 4 * (1 + Rand(12));
      const uint32_t vertices = 64 + Rand(2000);
      const uint32_t offset = 4 * Rand(16);
      mem[s].resize(offset + size_t(vertices) * stride);
      for (auto& b : mem[s]) b = uint8_t(Rand(256));
      spans[s] = {mem[s].data() + offset, uint64_t(vertices) * stride, stride};
      mask |= 1u << s;
    }
    const bool index32 = Rand(2);
    const uint32_t count = 3 + Rand(200);
    const int32_t base = int32_t(Rand(16));
    std::vector<uint8_t> idx(count * (index32 ? 4 : 2));
    for (uint32_t i = 0; i < count; ++i) PutBE(idx.data() + i * (index32 ? 4 : 2), 8 + Rand(40), index32);
    const IndexPolicy p = IndexPolicyFor(4, false, 0, index32);
    const DrawVertexRange r = IndexedVertexRange(idx.data(), count, index32, p, base);
    const uint64_t submit = DrawDataHash(idx.data(), idx.size(), spans.data(), mask, r);
    RecorderDrawData copy;
    CopyDrawData(idx.data(), idx.size(), spans.data(), mask, r, copy);
    const std::vector<uint8_t> idx_before = idx;
    std::vector<std::vector<uint8_t>> before;
    for (uint32_t s = 0; s < streams; ++s) {
      uint64_t start, bytes;
      StreamRangeBytes(spans[s], r, start, bytes);
      before.emplace_back(spans[s].data + start, spans[s].data + start + bytes);
    }
    ++rounds;
    false_alarm += DrawDataHash(idx.data(), idx.size(), spans.data(), mask, r) != submit;
    // A byte outside the range (the vertex before first_vertex, when there is one).
    if (r.first_vertex > 0) {
      ++outside_rounds;
      const uint32_t s = Rand(streams);
      uint8_t* p0 = const_cast<uint8_t*>(spans[s].data) + uint64_t(r.first_vertex - 1) * spans[s].stride;
      *p0 ^= 0x5A;
      alarm_outside += DrawDataHash(idx.data(), idx.size(), spans.data(), mask, r) != submit;
      *p0 ^= 0x5A;
    }
    // A byte inside the range, or an index.
    if (Rand(4) == 0) {
      idx[Rand(uint32_t(idx.size()))] ^= 0x01;
    } else {
      const uint32_t s = Rand(streams);
      uint8_t* p0 = const_cast<uint8_t*>(spans[s].data) + uint64_t(r.first_vertex) * spans[s].stride +
                    Rand(r.vertex_count * spans[s].stride);
      *p0 ^= 0x80;
    }
    const DrawVertexRange r2 = IndexedVertexRange(idx.data(), count, index32, p, base);
    missed_inside += DrawDataHash(idx.data(), idx.size(), spans.data(), mask, r2) == submit;
    // The copy keeps what was submitted.
    bool bad = copy.index_bytes != idx_before.size() ||
               std::memcmp(copy.bytes.data() + copy.index_offset, idx_before.data(), idx_before.size()) != 0;
    for (uint32_t s = 0; s < streams && !bad; ++s) {
      bad = copy.stream_bytes[s] != before[s].size() ||
            std::memcmp(copy.bytes.data() + copy.stream_offset[s], before[s].data(), before[s].size()) != 0;
    }
    copy_errors += bad;
  }
  std::printf("SNAPSHOT rounds %llu false_alarms %llu missed_inside %llu outside_rounds %llu alarms_outside %llu "
              "copy_errors %llu\n",
              (unsigned long long)rounds, (unsigned long long)false_alarm, (unsigned long long)missed_inside,
              (unsigned long long)outside_rounds, (unsigned long long)alarm_outside, (unsigned long long)copy_errors);
  return 0;
}
