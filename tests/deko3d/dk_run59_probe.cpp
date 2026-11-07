// Run 59 (devlog section AG): native_draw_snapshot.h's VolatilePages and DrawDataPartHashes, dk_ab.h's phase mask, and a
// model of the race run 58 measured (the title draws from buffer objects that are new every frame, in memory it rewrites
// while the recorder is a frame behind). Prints one line per check, "FAIL" on a wrong one.
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_set>
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

void PagesTest() {
  VolatilePages p;
  Check(!p.Any(false, 0x40010000, 16) && p.marked() == 0, "pages: nothing marked at first");
  p.Mark(false, 0x40010008, 16);
  Check(p.marked() == 1, "pages: one page marked");
  Check(p.Any(false, 0x4001FFF0, 32), "pages: a range ending in the marked page");
  Check(!p.Any(false, 0x40020000, 0x10000), "pages: the next page is not marked");
  Check(!p.Any(false, 0x40000000, 0x10000), "pages: the page before is not marked");
  Check(!p.Any(true, 0x40010000, 16), "pages: the physical view is apart from the virtual one");
  Check(p.Any(false, 0x00000000, 0x80000000ull), "pages: a 2 GiB range finds it (word at a time)");
  Check(!p.Any(false, 0x40020000, 0xBFFE0000ull), "pages: a range after it up to 4 GiB does not");
  p.Mark(false, 0x40010000, 0x10000);
  Check(p.marked() == 1, "pages: a page is counted once");
  p.Mark(true, 0x0F3E0000, 0x30000);  // three pages, crossing a 64-page word (0x0F40 = word boundary)
  Check(p.marked() == 4, "pages: a range over three pages marks three");
  Check(p.Any(true, 0x0F3FFFFF, 1) && p.Any(true, 0x0F400000, 1) && p.Any(true, 0x0F40FFFF, 1),
        "pages: across the word boundary");
  Check(!p.Any(true, 0x0F410000, 1) && !p.Any(true, 0x0F3DFFFF, 1), "pages: not past either end");
  p.Mark(false, 0xFFFF0000, 0x40000);  // runs past 4 GiB: clamped
  Check(p.Any(false, 0xFFFFFFF0, 0x100) && p.marked() == 5, "pages: a range past 4 GiB is clamped");
  Check(!p.Any(false, 0x50000000, 0), "pages: an empty range never matches");
}

void PartsTest() {
  std::vector<uint8_t> indices(60), s0(100 * 36), s1(100 * 48);
  for (size_t i = 0; i < indices.size(); ++i) indices[i] = uint8_t(i % 7);
  for (size_t i = 0; i < s0.size(); ++i) s0[i] = uint8_t(i * 13);
  for (size_t i = 0; i < s1.size(); ++i) s1[i] = uint8_t(i * 7 + 1);
  DrawStreamSpan spans[kSnapshotStreams] = {};
  spans[0] = {s0.data(), s0.size(), 36};
  spans[1] = {s1.data(), s1.size(), 48};
  const DrawVertexRange r{true, 10, 50};
  const auto a = DrawDataPartHashes(indices.data(), indices.size(), spans, 3, r);
  Check(a == DrawDataPartHashes(indices.data(), indices.size(), spans, 3, r), "parts: the same bytes, the same hashes");
  s1[20 * 48 + 4] ^= 0x40;  // a morph delta of vertex 20 (inside the range)
  const auto b = DrawDataPartHashes(indices.data(), indices.size(), spans, 3, r);
  Check(b[0] == a[0] && b[1] == a[1] && b[2] != a[2] && b[3] == a[3], "parts: a change in stream 1 names stream 1 only");
  s0[5 * 36] ^= 1;  // vertex 5: outside the range [10, 60)
  const auto c = DrawDataPartHashes(indices.data(), indices.size(), spans, 3, r);
  Check(c[1] == b[1], "parts: a change outside the draw's range is not this draw's");
  indices[3] ^= 1;
  const auto d = DrawDataPartHashes(indices.data(), indices.size(), spans, 3, r);
  Check(d[0] != c[0] && d[1] == c[1] && d[2] == c[2], "parts: a change in the indices names the indices");
  Check(DrawDataPartHashes(nullptr, 0, spans, 1, r)[2] == 0, "parts: a stream not in the mask hashes as 0");
}

void ScheduleTest() {
  AbWindow w;
  w.seconds = 10;
  w.frames = 50;
  w.draws = 50 * 4000;
  w.rechecked = 400;
  w.changed = 60;
  const uint32_t mask = AbPhaseBit(AbPhase::kBaseline) | AbPhaseBit(AbPhase::kPagesOff) | AbPhaseBit(AbPhase::kPagesOnly);
  AbSchedule ab(1500, mask);
  ab.Step(w);
  Check(ab.Step(w) && ab.phase() == AbPhase::kBaseline, "mask: two gameplay windows start the baseline");
  const AbPhase order[] = {AbPhase::kBaseline, AbPhase::kPagesOff, AbPhase::kPagesOnly};
  const AbPhase next[] = {AbPhase::kPagesOff, AbPhase::kPagesOnly, AbPhase::kBaseline};
  for (int i = 0; i < 3; ++i) {
    bool changed = false;
    for (int k = 0; k < 3; ++k) {
      changed = ab.Step(w);
    }
    Check(changed && ab.ended_phase() == order[i] && ab.phase() == next[i], AbPhaseName(next[i]));
  }
  Check(ab.cycle() == 1, "mask: one cycle is baseline, pages off, pages only");
  const AbTotals& t = ab.totals(AbPhase::kPagesOff);
  Check(t.windows == 2 && t.rechecked == 800 && t.changed == 120 && t.changed_percent() > 14.9 &&
            t.changed_percent() < 15.1,
        "mask: the race counters add up (15 %)");
  AbSchedule all(1500);
  all.Step(w);
  all.Step(w);
  int phases = 0;
  while (all.cycle() == 0) {
    for (int k = 0; k < 3; ++k) all.Step(w);
    ++phases;
  }
  Check(phases == 6, "no mask: run 58's six phases");
  AbSchedule zero(1500, 0);
  zero.Step(w);
  zero.Step(w);
  for (int k = 0; k < 3; ++k) zero.Step(w);
  Check(zero.phase() == AbPhase::kKeepTargetsOff, "mask 0: run 58's order");
  Check(std::string_view(AbPhaseName(AbPhase::kPagesOnly)) == "pages only" &&
            std::string_view(AbPhaseName(AbPhase::kCount)) == "?",
        "names");
}

// The race: every frame the title makes kBuffers new buffer objects (handles never seen before) in a ring of kSlots
// 64 KiB slots of physical memory, fills them, draws each kDraws times, and goes on; the recorder reads a draw one frame
// later, when the slot may hold the next lap's bytes. 1 draw in 32 is hashed at submit and checked again when recorded.
// Run 58's policy promotes the handle of a changed draw; run 59's marks its pages. Counted: draws recorded from bytes the
// title changed after submitting them.
struct RaceResult {
  uint64_t draws = 0, wrong = 0, copied = 0;
};

RaceResult Race(bool pages_policy, int frames) {
  constexpr uint32_t kSlots = 6, kBuffers = 4, kDraws = 40, kBase = 0x0C000000;
  std::vector<uint32_t> slot_content(kSlots, 0);  // what each slot holds now (the frame and buffer that wrote it)
  std::unordered_set<uint32_t> promoted;
  VolatilePages pages;
  struct Pending {
    uint32_t slot, content;
    bool copied, sampled;
    uint32_t handle;
  };
  std::vector<Pending> last_frame;
  RaceResult r;
  uint32_t handle = 0x80000000, sequence = 0, lap = 0;
  for (int f = 0; f < frames; ++f) {
    std::vector<Pending> this_frame;
    for (uint32_t b = 0; b < kBuffers; ++b) {
      const uint32_t slot = (lap++) % kSlots;
      slot_content[slot] = uint32_t(f * 16 + b + 1);  // the title writes the new buffer
      ++handle;                                       // a new object every time
      const uint32_t address = kBase + slot * 0x10000;
      for (uint32_t k = 0; k < kDraws; ++k) {
        const bool copy = pages_policy ? pages.Any(true, address, 0x8000) : promoted.contains(handle);
        this_frame.push_back({slot, slot_content[slot], copy, (sequence++ % 32) == 0, handle});
      }
    }
    // The recorder records the previous frame's draws now.
    for (const Pending& p : last_frame) {
      ++r.draws;
      r.copied += p.copied;
      const bool changed = slot_content[p.slot] != p.content;
      if (!p.copied && changed) {
        ++r.wrong;
      }
      if (!p.copied && p.sampled && changed) {
        if (pages_policy) {
          pages.Mark(true, kBase + p.slot * 0x10000, 0x8000);
        } else {
          promoted.insert(p.handle);
        }
      }
    }
    last_frame = std::move(this_frame);
  }
  return r;
}

void RaceTest() {
  const RaceResult handles = Race(false, 300), byPage = Race(true, 300);
  std::printf("race model, 300 frames: run 58 (handles) %llu of %llu draws recorded from changed bytes; run 59 (pages) "
              "%llu of %llu (%llu copied)\n",
              (unsigned long long)handles.wrong, (unsigned long long)handles.draws, (unsigned long long)byPage.wrong,
              (unsigned long long)byPage.draws, (unsigned long long)byPage.copied);
  Check(handles.wrong * 4 > handles.draws, "race model: promoting handles never catches the next frame's new objects");
  Check(byPage.wrong * 100 < byPage.draws, "race model: marking pages stops it after the first laps");
}

}  // namespace

int main() {
  PagesTest();
  PartsTest();
  ScheduleTest();
  RaceTest();
  std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
  return failures ? 1 : 0;
}
