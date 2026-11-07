// Run 50 probe (devlog section W): the save crash fix, the constant transport check and versions, the index fast paths.
//
// 1. FILE: core/host_file_share.h against a model of Horizon's file system, where a file open for writing is locked
//    (a second open and stat fail with EIO until it is closed). Run 49's sequence (the save created and written, opened
//    again, the VFS checking that its cached entry still exists) throws with the old check and not with the new one; the
//    second guest handle shares the open descriptor, which is closed once, with the last handle.
// 2. CONST: random draws through the front end's DiffRegisters and the recorder's ApplyConstantDelta; ConstantBankHash
//    of the front end's shadow (big-endian words) equals the recorder's bank (floats) every time; one delta dropped is
//    caught, in the region it touched (c0-c63, the palette c64-c255, pixel). Versions: each draw's copy of the bank,
//    read after the whole work was recorded (as the GPU does), equals the bank of its submit; the run-49 single block,
//    read the same way, gives a draw the bank of a later draw (what an update not ordered with the draws would show).
// 3. INDEX: the branch-free paths of ConvertIndices/ScanIndices (no restart) against the general loop over random data.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../../glue/rexglue-sdk-main/src/core/host_file_share.h"
#include "native_constant_diff.h"
#include "native_draw_snapshot.h"
#include "native_index_policy.h"

using namespace rex::graphics::gta4_native;
using rex::filesystem::HostFileIsGone;
using rex::filesystem::SharedHostFd;
using rex::filesystem::SharedHostFdTable;

namespace {

std::mt19937 rng(50);
uint32_t Rand(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); }

// --- 1. A model of Horizon: files, and the descriptors open per file with their mode.
struct FakeHorizon {
  std::set<std::string> files;
  std::map<int, std::pair<std::string, bool>> open;  // fd -> (path, write)
  int next_fd = 3;
  std::map<int, int> closes;
  bool Locked(const std::string& p) const {
    for (auto& [fd, o] : open) {
      if (o.first == p && o.second) return true;
    }
    return false;
  }
  int Open(const std::string& p, bool write, bool create) {
    if (!files.count(p)) {
      if (!create) {
        errno = ENOENT;
        return -1;
      }
      files.insert(p);
    }
    if (Locked(p) || (write && std::any_of(open.begin(), open.end(), [&](auto& o) { return o.second.first == p; }))) {
      errno = EIO;  // libnx's errno for Horizon's "target locked"
      return -1;
    }
    open[next_fd] = {p, write};
    return next_fd++;
  }
  // std::filesystem::status: ENOENT -> not found; EIO -> an error (exists() without error_code throws).
  bool Exists(const std::string& p, std::error_code& ec) const {
    ec.clear();
    if (!files.count(p)) return false;
    if (Locked(p)) {
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    return true;
  }
};
FakeHorizon* g_fs = nullptr;
void FakeClose(int fd) {
  g_fs->open.erase(fd);
  ++g_fs->closes[fd];
}

}  // namespace

int main() {
  // ---------------------------------------------------------------- 1. FILE
  uint64_t file_errors = 0;
  {
    FakeHorizon fs;
    g_fs = &fs;
    SharedHostFdTable table;
    const std::string save = "sdmc:/switch/theft4/saves/X/SGTA412/SGTA412";
    // The title creates the save for writing (run 49: file open #626, access 0x40100080).
    const int fd = fs.Open(save, true, true);
    auto first = table.Add(save, fd, true, true, FakeClose);
    // It opens the save again: the VFS checks its cached entry on the host first.
    std::error_code ec;
    const bool exists = fs.Exists(save, ec);
    const bool old_check_throws = !exists && ec;  // exists(path) without error_code: filesystem_error -> terminate
    const bool new_drops_entry = HostFileIsGone(exists, ec);
    file_errors += !old_check_throws;  // the model must reproduce run 49
    file_errors += new_drops_entry;    // the new check keeps the entry
    // The second open is refused by the host (locked) and shares the descriptor.
    const int second_fd = fs.Open(save, false, false);
    file_errors += second_fd != -1 || errno != EIO;
    auto second = table.Find(save, true, false);
    file_errors += !second || second->fd != fd;
    // The first handle closes: the descriptor stays open for the second.
    first.reset();
    file_errors += fs.closes.count(fd) != 0;
    second.reset();
    file_errors += fs.closes[fd] != 1;  // closed once, with the last handle
    file_errors += table.Find(save, false, false) != nullptr;
    // A missing file is gone; a missing folder in the path too; a present one is not.
    file_errors += !HostFileIsGone(false, std::error_code());
    file_errors += !HostFileIsGone(false, std::make_error_code(std::errc::no_such_file_or_directory));
    file_errors += !HostFileIsGone(false, std::make_error_code(std::errc::not_a_directory));
    file_errors += HostFileIsGone(true, std::error_code());
    // A read-only descriptor does not serve a write; a later read-only open keeps a writer as the one shared.
    const int r1 = fs.Open("a", false, true);
    auto ro = table.Add("a", r1, true, false, FakeClose);
    file_errors += table.Find("a", false, true) != nullptr;
    file_errors += table.Find("a", true, false) == nullptr;
    const int w1 = 100;
    auto rw = table.Add("b", w1, true, true, nullptr);
    auto ro2 = table.Add("b", 101, true, false, nullptr);
    file_errors += !table.Find("b", true, true) || table.Find("b", true, true)->fd != w1;
    // Many paths: the table does not grow without bound.
    std::vector<std::shared_ptr<SharedHostFd>> keep;
    for (int i = 0; i < 1000; ++i) {
      auto h = table.Add("f" + std::to_string(i), 1000 + i, true, false, nullptr);
      if (i % 100 == 0) keep.push_back(h);
    }
    file_errors += table.size() > 80;
  }
  std::printf("FILE errors %llu\n", (unsigned long long)file_errors);

  // ---------------------------------------------------------------- 2. CONST
  uint64_t draws = 0, hash_mismatch = 0, drops = 0, drops_caught = 0, drops_region_right = 0, version_wrong = 0,
           single_block_wrong = 0;
  {
    std::array<uint8_t, kVertexConstantRegisters * 16> guest_v{};
    std::array<uint8_t, kPixelConstantRegisters * 16> guest_p{};
    std::array<uint32_t, kVertexConstantRegisters * 4> shadow_v{};
    std::array<uint32_t, kPixelConstantRegisters * 4> shadow_p{};
    std::array<float, kVertexConstantRegisters * 4> bank_v{};
    std::array<float, kPixelConstantRegisters * 4> bank_p{};
    std::array<bool, kVertexConstantRegisters> dirty_v{};
    std::array<bool, kPixelConstantRegisters> dirty_p{};
    auto put = [](uint8_t* bank, uint32_t reg) {
      for (uint32_t c = 0; c < 4; ++c) {
        float f = float(Rand(20000)) * 0.01f - 100.0f;
        uint32_t w;
        std::memcpy(&w, &f, 4);
        w = __builtin_bswap32(w);
        std::memcpy(bank + reg * 16 + c * 4, &w, 4);
      }
    };
    struct Recorded {
      std::vector<float> version;  // the copy taken at this draw
      std::vector<float> expect;   // the bank of its submit
    };
    bool whole = true;
    for (int work = 0; work < 200; ++work) {
      std::vector<Recorded> recorded;
      std::vector<float> single_block;  // run 49: one block per work, changed in place
      std::vector<size_t> single_reads;
      for (int i = 0; i < 64; ++i) {
        // The title changes some registers: a world matrix, sometimes a bone palette, some pixel constants.
        for (int k = Rand(3); k > 0; --k) put(guest_v.data(), Rand(64));
        if (Rand(3) == 0) {
          const uint32_t bones = 1 + Rand(64);
          for (uint32_t r = 64; r < 64 + bones * 3; ++r) put(guest_v.data(), r);
        }
        for (int k = Rand(3); k > 0; --k) put(guest_p.data(), Rand(kPixelConstantRegisters));
        ConstantDelta dv, dp;
        DiffRegisters(guest_v.data(), shadow_v.data(), kVertexConstantRegisters, whole, dv);
        DiffRegisters(guest_p.data(), shadow_p.data(), kPixelConstantRegisters, whole, dp);
        whole = false;
        const std::array<uint64_t, 3> fe = {ConstantBankHash(shadow_v.data(), 0, 64, true),
                                            ConstantBankHash(shadow_v.data(), 64, kVertexConstantRegisters - 64, true),
                                            ConstantBankHash(shadow_p.data(), 0, kPixelConstantRegisters, true)};
        // One draw in 97 loses its vertex delta on the way (a model of a transport defect), once in a while.
        const bool drop = draws % 97 == 50 && !dv.runs.empty();
        bool palette_touched = false, low_touched = false;
        for (const ConstantRun& r : dv.runs) {
          palette_touched = palette_touched || r.first + r.count > 64;
          low_touched = low_touched || r.first < 64;
        }
        if (!drop) {
          ApplyConstantDelta(dv, bank_v.data(), dirty_v.data(), kVertexConstantRegisters);
        }
        ApplyConstantDelta(dp, bank_p.data(), dirty_p.data(), kPixelConstantRegisters);
        const std::array<uint64_t, 3> rec = {ConstantBankHash(bank_v.data(), 0, 64, false),
                                             ConstantBankHash(bank_v.data(), 64, kVertexConstantRegisters - 64, false),
                                             ConstantBankHash(bank_p.data(), 0, kPixelConstantRegisters, false)};
        ++draws;
        if (drop) {
          ++drops;
          const bool caught = rec != fe;
          drops_caught += caught;
          drops_region_right += caught && ((rec[0] != fe[0]) == low_touched) && ((rec[1] != fe[1]) == palette_touched) &&
                                rec[2] == fe[2];
          // Resynchronise the model as the front end would on its next whole capture.
          whole = true;
          continue;
        }
        hash_mismatch += rec != fe;
        recorded.push_back({std::vector<float>(bank_v.begin(), bank_v.end()),
                            std::vector<float>(bank_v.begin(), bank_v.end())});
        single_block.assign(bank_v.begin(), bank_v.end());
        single_reads.push_back(recorded.size() - 1);
      }
      // The GPU runs the work after it was recorded: a version still holds its draw's bank; the single block holds the
      // last one.
      for (const Recorded& r : recorded) {
        version_wrong += r.version != r.expect;
        single_block_wrong += single_block != r.expect;
      }
    }
  }
  std::printf("CONST draws %llu hash_mismatch %llu drops %llu caught %llu region_right %llu version_wrong %llu "
              "single_block_wrong %llu\n",
              (unsigned long long)draws, (unsigned long long)hash_mismatch, (unsigned long long)drops,
              (unsigned long long)drops_caught, (unsigned long long)drops_region_right,
              (unsigned long long)version_wrong, (unsigned long long)single_block_wrong);

  // ---------------------------------------------------------------- 3. INDEX
  uint64_t index_rounds = 0, index_errors = 0;
  for (int round = 0; round < 20000; ++round) {
    const bool index32 = Rand(2);
    const uint32_t count = Rand(400);  // 0 included
    std::vector<uint8_t> src(size_t(count) * (index32 ? 4 : 2) + 4);
    for (auto& b : src) b = uint8_t(Rand(256));
    const IndexPolicy fast = IndexPolicyFor(4, false, 0, index32);  // a list: no restart
    // The general loop: the same policy forced through it (restart on with a value that cannot occur: a 32-bit value
    // above 24 bits never equals a masked index; for 16 bits the policy refuses values above 0xFFFF, so compare to the
    // reference computed here).
    std::vector<uint8_t> a(src.size()), ref(src.size());
    const IndexConversion c = ConvertIndices(src.data(), count, index32, fast, a.data());
    const IndexConversion s = ScanIndices(src.data(), count, index32, fast);
    uint32_t lo = UINT32_MAX, hi = 0, masked = 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v;
      if (index32) {
        uint32_t raw;
        std::memcpy(&raw, src.data() + i * 4, 4);
        raw = __builtin_bswap32(raw);
        v = raw & kXenosIndexMask;
        masked += v != raw;
        std::memcpy(ref.data() + i * 4, &v, 4);
      } else {
        uint16_t h;
        std::memcpy(&h, src.data() + i * 2, 2);
        h = __builtin_bswap16(h);
        v = h;
        std::memcpy(ref.data() + i * 2, &h, 2);
      }
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    ++index_rounds;
    const size_t n = size_t(count) * (index32 ? 4 : 2);
    index_errors += c.lo != lo || c.hi != hi || c.masked != masked || s.lo != lo || s.hi != hi ||
                    std::memcmp(a.data(), ref.data(), n) != 0;
  }
  std::printf("INDEX rounds %llu errors %llu\n", (unsigned long long)index_rounds, (unsigned long long)index_errors);

  // ---------------------------------------------------------------- 4. POOL (native_draw_snapshot.h DrawDataPool)
  uint64_t pool_rounds = 0, pool_errors = 0;
  {
    DrawDataPool pool;
    std::vector<std::shared_ptr<RecorderDrawData>> in_flight;
    std::vector<std::vector<uint8_t>> mem(3);
    for (int round = 0; round < 20000; ++round) {
      // A draw: 0-3 streams of random size, indices or not; copied into a recycled object.
      std::array<DrawStreamSpan, kSnapshotStreams> spans{};
      uint32_t mask = 0;
      const uint32_t streams = Rand(4);
      for (uint32_t s = 0; s < streams; ++s) {
        const uint32_t stride = 4 * (1 + Rand(8));
        const uint32_t vertices = 1 + Rand(round % 50 == 0 ? 4000 : 300);
        mem[s].assign(size_t(vertices) * stride, uint8_t(round));
        spans[s] = {mem[s].data(), mem[s].size(), stride};
        mask |= 1u << s;
      }
      const uint32_t first = 0, count = 1 + Rand(64);
      const DrawVertexRange r{true, first, count};
      std::vector<uint8_t> idx(Rand(2) ? size_t(count) * 2 : 0, uint8_t(round + 1));
      auto d = pool.Take();
      CopyDrawData(idx.empty() ? nullptr : idx.data(), idx.size(), spans.data(), mask, r, *d);
      ++pool_rounds;
      // Every field as for a fresh object.
      RecorderDrawData fresh;
      CopyDrawData(idx.empty() ? nullptr : idx.data(), idx.size(), spans.data(), mask, r, fresh);
      bool bad = d->used != fresh.used || d->index_bytes != fresh.index_bytes || d->index_offset != fresh.index_offset ||
                 d->stream_offset != fresh.stream_offset || d->stream_bytes != fresh.stream_bytes ||
                 std::memcmp(d->bytes.data(), fresh.bytes.data(), fresh.used) != 0;
      pool_errors += bad;
      in_flight.push_back(std::move(d));
      // The recorder lets go of them in order, a ring's worth behind.
      if (in_flight.size() > 300 || Rand(5) == 0) {
        const size_t n = 1 + Rand(uint32_t(in_flight.size()));
        in_flight.erase(in_flight.begin(), in_flight.begin() + std::min(n, in_flight.size()));
      }
    }
    in_flight.clear();
    pool_errors += pool.free_count() > DrawDataPool::kMaxFree;
    pool_errors += pool.reused() == 0;
    pool_errors += pool.created() > 2000;  // a few hundred in flight at most, then recycled
    std::printf("POOL created %llu reused %llu\n", (unsigned long long)pool.created(), (unsigned long long)pool.reused());
  }
  std::printf("POOL rounds %llu errors %llu\n", (unsigned long long)pool_rounds, (unsigned long long)pool_errors);
  return 0;
}
