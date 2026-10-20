// Theft4 (docs/switch-port/16-deko3d-devlog.md, M0): what LRA 7c97c58's native hooks call in
// parts of its application this port does not take.
//
// - Touch controls (Android): not used here; the hooks' calls do nothing and the original
//   sprite draw runs.
// - Frontend sliders: LRA's DrawSliders is in gta4_frontend_slider_guest.inc, which is
//   translated game code; here the game's own routine draws them (LRA's fallback path).
// - Temporal boundaries (gta4_presentation_hooks.cpp): only temporal upscalers and frame
//   generation use them, both off; no declared boundary means the temporal path stays off.

#include <cstdint>

#include <rex/ppc/context.h>

#include "../../src/graphics/gta4_metal/temporal/title_boundary.h"
#include "gta4_frontend_hooks.h"
#include "input/context_touch_context.h"
#include "input/context_touch_radar.h"

extern "C" void __imp__sub_822551E0(PPCContext& ctx, uint8_t* base);

namespace gta4::input {
bool TouchRadarLocalViewport(uint8_t*) noexcept { return false; }
void BeginTouchHelpDraw(uint8_t*, uint32_t) noexcept {}
void ObserveTouchHelpText(uint8_t*, uint32_t) noexcept {}
void EndTouchHelpText() noexcept {}
void EndTouchHelpDraw() noexcept {}
void DrawTouchWeaponHudSprite(PPCContext& context, uint8_t* base,
                              void (*original)(PPCContext&, uint8_t*)) {
  original(context, base);
}
}  // namespace gta4::input

namespace gta4::frontend_menu {
void DrawSliders(PPCContext& ctx, uint8_t* base) { __imp__sub_822551E0(ctx, base); }
}  // namespace gta4::frontend_menu

namespace gta4::temporal_boundary {
Decision DeclaredBoundary(uint8_t*, uint32_t, uint64_t, uint32_t) { return {}; }
}  // namespace gta4::temporal_boundary
