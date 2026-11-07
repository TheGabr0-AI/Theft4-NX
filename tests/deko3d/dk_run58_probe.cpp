// Run 58 (devlog section AF): dk_ab.h's schedule driven on the PC. Prints one line per check, "FAIL" on a wrong one.
#include <cstdio>
#include <string_view>

#include "dk_ab.h"

using namespace rex::graphics::gta4_native::dk;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  failures += ok ? 0 : 1;
}

AbWindow Window(uint64_t frames, uint64_t draws_per_frame, double busy_per_frame = 100) {
  AbWindow w;
  w.seconds = 10;
  w.frames = frames;
  w.draws = frames * draws_per_frame;
  w.gpu_busy_ms = busy_per_frame * double(frames);
  w.gpu_gap_ms = 50.0 * double(frames);
  w.materializations = 13 * frames;
  w.target_binds = 33 * frames;
  return w;
}

}  // namespace

int main() {
  AbSchedule ab(1500);
  // Loading screens and a cut-scene: nothing starts.
  Check(!ab.Step(Window(580, 1)) && !ab.started(), "a loading screen does not start the phases");
  Check(!ab.Step(Window(150, 700)) && !ab.started(), "a cut-scene (700 draws a frame) does not start them");
  Check(!ab.Step(Window(45, 4000)) && !ab.started(), "one gameplay window is not enough");
  Check(!ab.Step(Window(150, 700)) && !ab.started(), "a cut-scene resets the streak");
  Check(!ab.Step(Window(45, 4000)), "first gameplay window of the new streak");
  Check(ab.Step(Window(45, 4000)) && ab.started() && ab.phase() == AbPhase::kBaseline && !ab.ended(),
        "the second one in a row starts the baseline");

  // Baseline: window 1 settles, window 2 counted and asks for the capture, window 3 counted and ends the phase.
  Check(!ab.Step(Window(45, 4000)) && !ab.capture_now(), "baseline window 1 (settling, not counted)");
  Check(ab.totals(AbPhase::kBaseline).windows == 0, "the settling window is not counted");
  Check(!ab.Step(Window(46, 4000)) && ab.capture_now(), "baseline window 2: counted, capture asked for the last one");
  Check(!ab.Step(Window(150, 700)) && ab.phase() == AbPhase::kBaseline, "a cut-scene window does not move the phase");
  Check(ab.totals(AbPhase::kBaseline).windows == 1, "nor is it counted");
  Check(ab.Step(Window(44, 4000)) && ab.ended() && ab.ended_phase() == AbPhase::kBaseline &&
            ab.phase() == AbPhase::kKeepTargetsOff,
        "baseline window 3 ends it; keep targets off begins");
  const AbTotals& b = ab.totals(AbPhase::kBaseline);
  Check(b.windows == 2 && b.frames == 90 && b.draws == 90 * 4000, "baseline totals: 2 windows, 90 frames");
  Check(b.fps() > 4.49 && b.fps() < 4.51, "baseline fps 4.5");
  Check(b.per_frame(b.gpu_busy_ms) > 99.9 && b.per_frame(b.gpu_busy_ms) < 100.1, "baseline GPU busy 100 ms a frame");

  // The other phases in order, then the next cycle.
  const AbPhase order[] = {AbPhase::kKeepTargetsOff, AbPhase::kPlacementOff, AbPhase::kStoreOff, AbPhase::kCopiesOff,
                           AbPhase::kCullOff};
  for (size_t i = 0; i < 5; ++i) {
    bool changed = false;
    for (int k = 0; k < 3; ++k) {
      changed = ab.Step(Window(50, 4000, 90));
    }
    Check(changed && ab.ended_phase() == order[i], AbPhaseName(order[i]));
  }
  Check(ab.phase() == AbPhase::kBaseline && ab.cycle() == 1, "after cull off: the baseline of cycle 1");
  const AbTotals& c = ab.totals(AbPhase::kCopiesOff);
  Check(c.windows == 2 && c.fps() > 4.99 && c.fps() < 5.01, "copies off: 2 windows at 5 fps");
  for (int k = 0; k < 3; ++k) {
    ab.Step(Window(45, 4000));
  }
  Check(ab.totals(AbPhase::kBaseline).windows == 4, "the baseline adds up over the cycles");
  Check(std::string_view(AbPhaseName(AbPhase::kCount)) == "?", "names end at kCount");
  std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
  return failures ? 1 : 0;
}
