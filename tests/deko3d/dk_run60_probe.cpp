// Run 60 (devlog section AH): DrawDataHash hashes the vertex range by value (runs 49-59 hashed the struct with its padding
// bytes, so the front end and the recorder hashed equal draws differently), and dk_ab.h's "probe shaders" phase.
// Prints one line per check, "FAIL" on a wrong one.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

#include "dk_ab.h"
#include "native_draw_snapshot.h"

using namespace rex::graphics::gta4_native;
using namespace rex::graphics::gta4_native::dk;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  failures += ok ? 0 : 1;
}

// A range built in storage that held other bytes before, as a stack slot does: the padding keeps them.
const DrawVertexRange* RangeOver(unsigned char* storage, unsigned char fill, uint32_t first, uint32_t count) {
  auto* r = new (storage) DrawVertexRange;
  r->valid = true;
  r->first_vertex = first;
  r->vertex_count = count;
  // The bytes between `valid` and `first_vertex` are no member's: whatever the slot held (the constructor may or may not
  // have zeroed them; a copy from a stack temporary carries that temporary's).
  std::memset(storage + sizeof(bool), fill, offsetof(DrawVertexRange, first_vertex) - sizeof(bool));
  return r;
}

// Runs 49-59's hash of the range: the struct's bytes.
uint64_t OldRangeHash(const DrawVertexRange& r) {
  return HashBytes(0xCBF29CE484222325ull, reinterpret_cast<const uint8_t*>(&r), sizeof(r));
}

void HashTest() {
  std::vector<uint8_t> indices(96), stream(500 * 36);
  for (size_t i = 0; i < indices.size(); ++i) indices[i] = uint8_t(i * 3);
  for (size_t i = 0; i < stream.size(); ++i) stream[i] = uint8_t(i * 11 + 5);
  DrawStreamSpan spans[kSnapshotStreams] = {};
  spans[0] = {stream.data(), stream.size(), 36};
  alignas(DrawVertexRange) unsigned char a_storage[sizeof(DrawVertexRange)];
  alignas(DrawVertexRange) unsigned char b_storage[sizeof(DrawVertexRange)];
  const DrawVertexRange* a = RangeOver(a_storage, 0x00, 7, 300);
  const DrawVertexRange* b = RangeOver(b_storage, 0xA5, 7, 300);
  Check(sizeof(DrawVertexRange) > 1 + 2 * sizeof(uint32_t), "the range has padding bytes");
  Check(OldRangeHash(*a) != OldRangeHash(*b),
        "runs 49-59: the same range over different stack bytes hashed differently (the false changes)");
  Check(DrawDataHash(indices.data(), indices.size(), spans, 1, *a) ==
            DrawDataHash(indices.data(), indices.size(), spans, 1, *b),
        "run 60: the same draw hashes the same whatever the padding holds");
  Check(DrawDataPartHashes(indices.data(), indices.size(), spans, 1, *a) ==
            DrawDataPartHashes(indices.data(), indices.size(), spans, 1, *b),
        "the parts were equal all along (run 59: 4,088 changes, 0 parts)");
  const uint64_t before = DrawDataHash(indices.data(), indices.size(), spans, 1, *a);
  stream[100 * 36 + 8] ^= 0x10;  // vertex 100, inside [7, 307)
  Check(DrawDataHash(indices.data(), indices.size(), spans, 1, *a) != before, "a real byte change is still seen");
  stream[100 * 36 + 8] ^= 0x10;
  stream[400 * 36] ^= 0x10;  // vertex 400: outside the range
  Check(DrawDataHash(indices.data(), indices.size(), spans, 1, *a) == before, "a change outside the range is not");
  const DrawVertexRange* c = RangeOver(a_storage, 0x00, 8, 300);
  Check(DrawDataHash(indices.data(), indices.size(), spans, 1, *c) != before, "a different range is seen");
  indices[4] ^= 1;
  Check(DrawDataHash(indices.data(), indices.size(), spans, 1, *b) != before, "a change in the indices is seen");
}

void ScheduleTest() {
  AbWindow w;
  w.seconds = 10;
  w.frames = 45;
  w.draws = 45 * 4000;
  const uint32_t mask = AbPhaseBit(AbPhase::kBaseline) | AbPhaseBit(AbPhase::kProbeShaders);
  Check(mask == 257, "mask 257 = baseline and probe shaders");
  AbSchedule ab(1500, mask);
  ab.Step(w);
  Check(ab.Step(w) && ab.phase() == AbPhase::kBaseline, "the baseline starts");
  for (int k = 0; k < 3; ++k) ab.Step(w);
  Check(ab.ended() && ab.phase() == AbPhase::kProbeShaders, "then probe shaders");
  for (int k = 0; k < 3; ++k) ab.Step(w);
  Check(ab.ended() && ab.phase() == AbPhase::kBaseline && ab.cycle() == 1, "then the baseline of cycle 1");
  Check(std::string_view(AbPhaseName(AbPhase::kProbeShaders)) == "probe shaders" &&
            std::string_view(AbPhaseName(AbPhase::kCount)) == "?",
        "names");
  AbSchedule old(1500);
  old.Step(w);
  old.Step(w);
  int phases = 0;
  while (old.cycle() == 0) {
    for (int k = 0; k < 3; ++k) old.Step(w);
    ++phases;
  }
  Check(phases == 6, "no mask: still run 58's six");
}

}  // namespace

int main() {
  HashTest();
  ScheduleTest();
  std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
  return failures ? 1 : 0;
}
