// The renderer chosen at boot (cvar gta4_renderer): docs/switch-port/18-renut-migration.md.
#pragma once

namespace theft4::renut {

// True when gta4_renderer = "renut": the Direct3D hooks of gta4_renut_device.cpp take over and the app sets no
// emulated GPU. False: every hook runs the game's original function.
bool Selected();

}  // namespace theft4::renut
