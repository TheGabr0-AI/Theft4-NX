// Offline probe of run 55 (docs/switch-port/16-deko3d-devlog.md section AC), compiled by tests/deko3d/test_run55.py.
//
//   dk_run55_probe pool                                  native_draw_snapshot.h's DrawDataPool: no allocation per draw
//                                                        once warm, every copy as a fresh one, and no entry handed out
//                                                        while a "recorder" thread still holds it.
//   dk_run55_probe rules                                 dk_draw_rules.h: IsDepthOnlyDraw's truth table, DepthBiasFor
//                                                        against the Vulkan path's formula.
//   dk_run55_probe instrument <out dir> <list file>      InstrumentVertexGlsl with every vertex's position (run 55's
//                                                        console form); "INSTR <file> <values> <ok>" per file.

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dk_draw_rules.h"
#include "dk_shader_probe.h"
#include "native_draw_snapshot.h"

using namespace rex::graphics::gta4_native;

// Every allocation of the process, counted (the test's own containers included: the pool part counts around the calls
// it measures only).
static std::atomic<uint64_t> g_allocations{0};
void* operator new(size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static uint32_t Rand(std::mt19937& rng, uint32_t n) { return n ? uint32_t(rng() % n) : 0; }

int Pool() {
  std::mt19937 rng(55);
  uint64_t errors = 0;
  // 1. Contents: every recycled copy as a fresh one (run 50's check, kept).
  {
    DrawDataPool pool;
    std::vector<std::shared_ptr<RecorderDrawData>> in_flight;
    std::vector<std::vector<uint8_t>> mem(3);
    for (int round = 0; round < 20000; ++round) {
      std::array<DrawStreamSpan, kSnapshotStreams> spans{};
      uint32_t mask = 0;
      const uint32_t streams = Rand(rng, 4);
      for (uint32_t s = 0; s < streams; ++s) {
        const uint32_t stride = 4 * (1 + Rand(rng, 8));
        const uint32_t vertices = 1 + Rand(rng, round % 50 == 0 ? 40000 : 300);
        mem[s].assign(size_t(vertices) * stride, uint8_t(round + s));
        spans[s] = {mem[s].data(), mem[s].size(), stride};
        mask |= 1u << s;
      }
      const DrawVertexRange r{true, 0, 1 + Rand(rng, 64)};
      std::vector<uint8_t> idx(Rand(rng, 2) ? size_t(r.vertex_count) * 2 : 0, uint8_t(round + 1));
      auto d = pool.Take();
      errors += d.use_count() != 2;  // the pool's reference and this one
      CopyDrawData(idx.empty() ? nullptr : idx.data(), idx.size(), spans.data(), mask, r, *d);
      RecorderDrawData fresh;
      CopyDrawData(idx.empty() ? nullptr : idx.data(), idx.size(), spans.data(), mask, r, fresh);
      errors += d->used != fresh.used || d->index_bytes != fresh.index_bytes || d->stream_offset != fresh.stream_offset ||
                d->stream_bytes != fresh.stream_bytes || std::memcmp(d->bytes.data(), fresh.bytes.data(), fresh.used) != 0;
      in_flight.push_back(std::move(d));
      if (in_flight.size() > 300 || Rand(rng, 5) == 0) {
        const size_t n = 1 + Rand(rng, uint32_t(in_flight.size()));
        in_flight.erase(in_flight.begin(), in_flight.begin() + std::min(n, in_flight.size()));
      }
    }
    in_flight.clear();
    std::printf("POOL contents: made %llu, recycled %llu, idle %zu, kept %zu KiB\n",
                (unsigned long long)pool.created(), (unsigned long long)pool.reused(), pool.free_count(),
                pool.kept_bytes() >> 10);
    errors += pool.created() > 400 || pool.reused() == 0 || pool.free_count() != pool.created();
  }
  // 2. No allocation per draw once warm: the same sizes again and again (a frame's draws), released in order a ring
  // behind. Run 50's pool made a control block per take (>= 1 allocation each); run 49's two.
  uint64_t steady_allocations = 0, steady_takes = 0;
  {
    DrawDataPool pool;
    std::deque<std::shared_ptr<RecorderDrawData>> ring;
    std::vector<uint8_t> source(256 << 10, 7);
    const uint32_t sizes[8] = {48, 960, 4096, 16384, 70000, 131072, 200000, 256};
    for (int round = 0; round < 40000; ++round) {
      const bool measured = round >= 20000;
      const uint64_t before = g_allocations.load();
      auto d = pool.Take();
      const uint32_t bytes = sizes[round % 8];
      std::array<DrawStreamSpan, kSnapshotStreams> spans{};
      spans[0] = {source.data(), bytes, 4};
      CopyDrawData(nullptr, 0, spans.data(), 1, DrawVertexRange{true, 0, bytes / 4}, *d);
      const uint64_t after = g_allocations.load();
      ring.push_back(std::move(d));
      if (ring.size() > 600) {
        ring.pop_front();
      }
      if (measured) {
        steady_allocations += after - before;
        ++steady_takes;
      }
    }
    std::printf("POOL steady state: %llu allocations in %llu takes and copies (run 50: >= %llu)\n",
                (unsigned long long)steady_allocations, (unsigned long long)steady_takes,
                (unsigned long long)steady_takes);
    errors += steady_allocations != 0;
  }
  // 3. Two threads: a taken entry is never one the recorder still holds. Each copy carries its own pattern; the
  // "recorder" checks it before letting go.
  {
    DrawDataPool pool;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<std::shared_ptr<const RecorderDrawData>, uint8_t>> queue;
    std::atomic<uint64_t> bad{0}, checked{0};
    bool done = false;
    std::thread recorder([&] {
      std::mt19937 r2(7);
      for (;;) {
        std::pair<std::shared_ptr<const RecorderDrawData>, uint8_t> item;
        {
          std::unique_lock<std::mutex> lock(m);
          cv.wait(lock, [&] { return done || !queue.empty(); });
          if (queue.empty()) {
            return;
          }
          item = std::move(queue.front());
          queue.pop_front();
        }
        const RecorderDrawData& d = *item.first;
        for (size_t k = 0; k < d.used; k += 61) {
          bad += d.bytes[k] != item.second;
        }
        ++checked;
        if (Rand(r2, 4) == 0) {
          std::this_thread::yield();
        }
      }
    });
    std::vector<uint8_t> source(64 << 10);
    for (int round = 0; round < 60000; ++round) {
      const uint8_t pattern = uint8_t(round * 31 + 1);
      std::memset(source.data(), pattern, source.size());
      auto d = pool.Take();
      std::array<DrawStreamSpan, kSnapshotStreams> spans{};
      const uint32_t bytes = 64 + Rand(rng, 60000);
      spans[0] = {source.data(), bytes, 4};
      CopyDrawData(nullptr, 0, spans.data(), 1, DrawVertexRange{true, 0, bytes / 4}, *d);
      {
        std::lock_guard<std::mutex> lock(m);
        queue.emplace_back(std::move(d), pattern);
      }
      cv.notify_one();
      while (true) {  // the ring's bound (4,096 packets)
        std::lock_guard<std::mutex> lock(m);
        if (queue.size() < 4096) break;
      }
    }
    {
      std::lock_guard<std::mutex> lock(m);
      done = true;
    }
    cv.notify_one();
    recorder.join();
    std::printf("POOL threads: %llu copies checked by the recorder thread, %llu bytes wrong; made %llu\n",
                (unsigned long long)checked.load(), (unsigned long long)bad.load(),
                (unsigned long long)pool.created());
    errors += bad.load() != 0 || checked.load() != 60000;
  }
  std::printf("POOL errors %llu\n", (unsigned long long)errors);
  return 0;
}

int Rules() {
  using namespace rex::graphics::gta4_native::dk;
  uint64_t errors = 0, cases = 0;
  // Truth table: depth only exactly when every condition holds.
  for (uint32_t bits = 0; bits < 256; ++bits) {
    DepthOnlyInputs in;
    in.enabled = bits & 1;
    in.has_pixel_shader = bits & 2;
    in.coverage = bits & 4;
    in.depth_target = bits & 8;
    in.flags_valid = bits & 16;
    in.kills = bits & 32;
    in.writes_depth = bits & 64;
    in.colour_write_masks = (bits & 128) ? 0x10u : 0u;  // target 1 writes red
    const bool expected = in.enabled && in.has_pixel_shader && !in.coverage && in.depth_target && in.flags_valid &&
                          !in.kills && !in.writes_depth && !(bits & 128);
    errors += IsDepthOnlyDraw(in) != expected;
    ++cases;
  }
  // Depth bias: the Vulkan path's factors (graphics_system.cpp: x 2^24 and x 1/16), bit for bit.
  std::mt19937 rng(5);
  for (int k = 0; k < 100000; ++k) {
    float c, s;
    do {
      const uint32_t a = rng(), b = rng();
      std::memcpy(&c, &a, 4);
      std::memcpy(&s, &b, 4);
    } while (!std::isfinite(c) || !std::isfinite(s));
    uint32_t cb, sb;
    std::memcpy(&cb, &c, 4);
    std::memcpy(&sb, &s, 4);
    const DepthBiasFactors f = DepthBiasFor(cb, sb);
    const float vk_constant = c * float(UINT32_C(1) << (21 + 3));  // draw_util::kD3D10PolygonOffsetFactorFloat24
    const float vk_slope = s * (1.0f / 16.0f);                       // xenos::kPolygonOffsetScaleSubpixelUnit
    errors += std::memcmp(&f.constant, &vk_constant, 4) != 0 || std::memcmp(&f.slope, &vk_slope, 4) != 0;
    ++cases;
  }
  std::printf("RULES cases %llu errors %llu\n", (unsigned long long)cases, (unsigned long long)errors);
  return 0;
}

int Instrument(char** argv) {
  const std::string out_dir = argv[2];
  std::vector<std::string> files;
  {
    std::ifstream list(argv[3]);
    for (std::string line; std::getline(list, line);) {
      if (!line.empty()) files.push_back(line);
    }
  }
  int ok = 0, refused = 0;
  for (const std::string& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    const size_t nl = text.find('\n');
    if (text.rfind("#version", 0) == 0 && nl != std::string::npos) {
      text = text.substr(0, nl + 1) + "#define SPIRV_CROSS_CONSTANT_ID_0 0u\n" + text.substr(nl + 1);
    }
    std::vector<rex::graphics::gta4_native::dk::ProbeVariable> vars;
    const std::string out = rex::graphics::gta4_native::dk::InstrumentVertexGlsl(
        text, 64, vars, rex::graphics::gta4_native::dk::kProbePositions);
    std::string name = file.substr(file.find_last_of("/\\") + 1);
    if (out.empty()) {
      ++refused;
      std::printf("INSTR %s 0 0\n", name.c_str());
      continue;
    }
    ++ok;
    std::ofstream(out_dir + "/" + name + ".vert", std::ios::binary) << out;
    std::printf("INSTR %s %zu 1\n", name.c_str(), vars.size());
  }
  std::printf("INSTRUMENTED %d REFUSED %d\n", ok, refused);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "pool") == 0) return Pool();
  if (argc >= 2 && std::strcmp(argv[1], "rules") == 0) return Rules();
  if (argc >= 4 && std::strcmp(argv[1], "instrument") == 0) return Instrument(argv);
  std::fprintf(stderr, "usage: dk_run55_probe pool | rules | instrument <out dir> <list file>\n");
  return 2;
}
