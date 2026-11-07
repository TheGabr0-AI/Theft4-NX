// Probe of run 46 (devlog section R): native_inline_vector.h and native_packet_ring.h. Host clang, no Switch headers.
//  1. InlineVector against std::vector: push_back, insert at the end, copy, move, spill and back, clear.
//  2. The ring with two threads: RecorderDraw packets (and a Present now and then) go through a small ring that fills often; the
//     consumer checks the order and every packet's constants, as the recorder reads them, in place.
//  3. Allocations per packet in the steady state, counted by a replaced operator new on each thread: the producer builds a draw
//     and its constant delta (DiffRegisters) and pushes it; the consumer reads it and lets go of what it shares. Expected 0 on
//     both threads for draws of up to 16 changed registers.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <thread>
#include <variant>
#include <vector>

#include "native_constant_diff.h"
#include "native_packet_ring.h"

using namespace rex::graphics::gta4_native;

static thread_local uint64_t t_allocations = 0;
void* operator new(size_t n) {
  ++t_allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using Packet = std::variant<RecorderDraw, RecorderPresent>;

static uint32_t InlineVectorErrors() {
  uint32_t errors = 0;
  std::mt19937 rng(46);
  for (int round = 0; round < 2000; ++round) {
    InlineVector<float, 64> v;
    std::vector<float> ref;
    const uint32_t n = rng() % (round % 10 == 0 ? 600 : 70);
    for (uint32_t i = 0; i < n; ++i) {
      const float f = float(rng() % 100000) * 0.5f;
      if (i % 5 == 0) {
        const float four[4] = {f, f + 1, f + 2, f + 3};
        v.insert(v.end(), four, four + 4);
        ref.insert(ref.end(), four, four + 4);
      } else {
        v.push_back(f);
        ref.push_back(f);
      }
    }
    auto same = [&](const InlineVector<float, 64>& a) {
      return a.size() == ref.size() && (ref.empty() || std::memcmp(a.data(), ref.data(), ref.size() * 4) == 0);
    };
    if (!same(v)) ++errors;
    if (v.spilled() != (ref.size() > 64)) ++errors;
    InlineVector<float, 64> copy(v);
    if (!same(copy) || !same(v)) ++errors;
    InlineVector<float, 64> moved(std::move(copy));
    if (!same(moved) || !copy.empty() || copy.spilled()) ++errors;
    InlineVector<float, 64> small;
    small.push_back(1.0f);
    small = std::move(moved);  // into a short one
    if (!same(small)) ++errors;
    InlineVector<float, 64> big;
    for (int i = 0; i < 100; ++i) big.push_back(float(i));
    big = small;  // a spilled one takes a copy of a short (or long) one
    if (!same(big)) ++errors;
    big.clear();
    if (!big.empty() || big.spilled()) ++errors;
  }
  return errors;
}

int main() {
  const uint32_t iv_errors = InlineVectorErrors();

  // A guest constant bank per stream and a shadow, as the front end keeps them.
  constexpr uint32_t kRegisters = 256;
  constexpr uint64_t kPackets = 2'000'000;
  PacketRing<Packet> ring(64);
  std::atomic<uint64_t> producer_steady_allocations{0}, consumer_steady_allocations{0};
  std::atomic<uint32_t> errors{0};
  uint64_t checksum_sent = 0, checksum_seen = 0;

  std::thread consumer([&] {
    uint64_t expected = 0;
    std::mt19937 rng(7);
    while (true) {
      const uint64_t before = t_allocations;
      const size_t ready = ring.Wait(std::chrono::milliseconds(2), 1 + rng() % 64);
      if (!ready && ring.stopped()) break;
      for (size_t i = 0; i < ready; ++i) {
        Packet& p = ring.Peek(i);
        if (auto* d = std::get_if<RecorderDraw>(&p)) {
          if (d->sequence != expected) errors.fetch_add(1);
          ++expected;
          // What the recorder's ApplyConstants reads: the runs and their values.
          for (const ConstantRun& run : d->vertex_constants.runs) {
            if (run.first + run.count > kRegisters) errors.fetch_add(1);
            for (uint32_t k = 0; k < run.count * 4u; ++k) {
              float f = d->vertex_constants.values.data()[run.data + k];
              uint32_t w;
              std::memcpy(&w, &f, 4);
              checksum_seen += w * (run.first * 4u + k + 1);
            }
          }
          d->up_vertices.reset();
          d->declaration.reset();
        }
      }
      ring.Release(ready);
      if (expected > 100'000) consumer_steady_allocations.fetch_add(t_allocations - before);
    }
    if (expected != kPackets) errors.fetch_add(1000);
  });

  {
    std::mt19937 rng(0x46);
    std::vector<uint8_t> guest(kRegisters * 16);
    for (auto& b : guest) b = uint8_t(rng());
    std::vector<uint32_t> shadow(kRegisters * 4);
    std::vector<float> host(kRegisters * 4);  // host-endian copy, for the checksum
    bool whole = true;
    for (uint64_t s = 0; s < kPackets; ++s) {
      // A draw changes a few registers (1-12), now and then a whole bone palette (spills), rarely nothing.
      const uint32_t mode = uint32_t(s % 97);
      const uint32_t changes = mode == 0 ? 0 : mode == 1 ? 80 : 1 + rng() % 12;
      for (uint32_t c = 0; c < changes; ++c) {
        const uint32_t r = mode == 1 ? 100 + c : rng() % kRegisters;
        for (uint32_t i = 0; i < 16; ++i) guest[r * 16 + i] = uint8_t(rng());
      }
      const uint64_t before = t_allocations;
      Packet p{std::in_place_type<RecorderDraw>};
      RecorderDraw& d = std::get<RecorderDraw>(p);
      d.sequence = s;
      DiffRegisters(guest.data(), shadow.data(), kRegisters, whole, d.vertex_constants);
      whole = false;
      for (const ConstantRun& run : d.vertex_constants.runs) {
        for (uint32_t k = 0; k < run.count * 4u; ++k) {
          float f = d.vertex_constants.values.data()[run.data + k];
          uint32_t w;
          std::memcpy(&w, &f, 4);
          checksum_sent += w * (run.first * 4u + k + 1);
        }
      }
      const bool spilled = d.vertex_constants.values.spilled() || d.vertex_constants.runs.spilled();
      if (!ring.Push(std::move(p), s % 50 == 0, 16)) {
        errors.fetch_add(1);
        break;
      }
      if (s > 100'000 && !spilled) producer_steady_allocations.fetch_add(t_allocations - before);
      if (s % 1000 == 999) {
        // A Present between draws: another alternative in the slot, then a draw again.
        if (!ring.Push(Packet{std::in_place_type<RecorderPresent>}, true, 16)) {
          errors.fetch_add(1);
          break;
        }
      }
    }
  }
  // Drain, then stop: the consumer reads what is left before it ends.
  while (ring.depth()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ring.Stop();
  consumer.join();

  // A stopped ring refuses a push, also one that waits for a slot.
  PacketRing<Packet> full(2);
  full.Push(Packet{std::in_place_type<RecorderPresent>}, false, 16);
  full.Push(Packet{std::in_place_type<RecorderPresent>}, false, 16);
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    full.Stop();
  });
  const bool refused = !full.Push(Packet{std::in_place_type<RecorderPresent>}, false, 16);
  stopper.join();

  std::printf("RING inline_vector_errors %u packets %llu errors %u checksum_equal %d full_waits %llu "
              "producer_steady_allocations %llu consumer_steady_allocations %llu stopped_push_refused %d packet_bytes %zu\n",
              iv_errors, (unsigned long long)kPackets, errors.load(), int(checksum_sent == checksum_seen),
              (unsigned long long)ring.full_waits(), (unsigned long long)producer_steady_allocations.load(),
              (unsigned long long)consumer_steady_allocations.load(), int(refused), sizeof(Packet));
  return 0;
}
