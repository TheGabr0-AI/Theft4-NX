// Run 63 offline test (tests/deko3d/test_run63.py): dk_overwrite.h. Prints "OVERWRITE <name> ok|FAIL".
#include <cstdio>

#include "dk_overwrite.h"

using namespace rex::graphics::gta4_native::dk;

static int failures = 0;
static void Check(const char* name, bool ok) {
  std::printf("OVERWRITE %s %s\n", name, ok ? "ok" : "FAIL");
  failures += !ok;
}

int main() {
  OverwriteTracker t;
  t.NewFrame(1);
  // Nothing counted before a character drew into the surface.
  Check("before-character", !t.Note(OverwriteKind::kClear, 0x402266E0));
  const uint32_t gbuffer[4] = {0x402266E0, 0x40226770, 0x40226800, 0};
  t.SkinnedDraw(gbuffer, 4, 0x402188A0);
  t.SceneryDraw(0x402188A0, 0x402266E0);
  Check("draw-with-depth", t.Note(OverwriteKind::kDrawWithDepth, 0x402266E0, 10, 0xABC));
  Check("draw-without-depth", t.Note(OverwriteKind::kDrawWithoutDepth, 0x40226770, 11, 0xDEF));
  Check("placement", t.Note(OverwriteKind::kPlacement, 0x40226800));
  Check("depth-handoff", t.Note(OverwriteKind::kDepthHandoff, 0x402188A0));
  Check("other-surface", !t.Note(OverwriteKind::kClear, 0x40076EB0));
  Check("null-handle", !t.Note(OverwriteKind::kClear, 0));
  // After its resolve a surface is the next pass's.
  t.Resolved(0x402266E0);
  Check("after-resolve", !t.Note(OverwriteKind::kClear, 0x402266E0) && t.resolved() == 1);
  Check("still-followed", t.Note(OverwriteKind::kResolveDepthClear, 0x402188A0));
  const auto& n = t.interval();
  Check("counts", n[0] == 0 && n[1] == 1 && n[2] == 1 && n[3] == 1 && n[4] == 1 && n[5] == 1);
  // Draws testing depth are counted, not listed; the others are listed once each.
  const auto events = t.TakeNewEvents();
  Check("events", events.size() == 4 && events[0].kind == OverwriteKind::kDrawWithoutDepth && events[0].sequence == 11 &&
                      events[0].vertex_shader == 0xDEF && t.TakeNewEvents().empty());
  t.ResetInterval();
  Check("interval-reset", t.interval()[1] == 0 && t.totals()[1] == 1);
  // A new frame forgets the surfaces.
  t.NewFrame(2);
  Check("new-frame", !t.Note(OverwriteKind::kPlacement, 0x40226800));
  // Shared depth: no character on a depth of its own; then one on another depth surface.
  Check("shared-depth", t.CharacterDrawsOnOwnDepth() == 0);
  const uint32_t own[1] = {0x40300000};
  t.SkinnedDraw(own, 1, 0x40300040);
  t.SkinnedDraw(own, 1, 0x40300040);
  Check("own-depth", t.CharacterDrawsOnOwnDepth() == 2);
  Check("pairs-text", OverwriteTracker::PairsText(t.characters()) == "402188A0/402266E0 x1, 40300040/40300000 x2" &&
                          OverwriteTracker::PairsText(t.scenery()) == "402188A0/402266E0 x1");
  std::printf("OVERWRITE failures %d\n", failures);
  return failures ? 1 : 0;
}
