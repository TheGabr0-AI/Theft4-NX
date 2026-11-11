// theft4: the console's mode and render height as Renut's renderer asks for them (sa2-nx's src/me/sa2_render_mode.h).
// GTA IV renders 1280x720 in both modes for now (the goal is 720p; docs/switch-port/18): tools/renut-gta4/
// gta4_renut_compat.cpp.
#pragma once

#include <cstdint>

namespace sa2::render {

// The console is docked (the Switch's operation mode; false on the PC).
bool Docked();
// The render height for a mode, in lines.
int32_t RenderHeightOf(bool docked);
// Once per presented game frame.
void OnPresented(uint64_t presented);

}  // namespace sa2::render
