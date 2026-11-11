// The renderer chosen at boot (cvar gta4_renderer): docs/switch-port/18-renut-migration.md.
#pragma once

namespace theft4::renut {

// True when gta4_renderer = "renut": the Direct3D hooks of gta4_renut_device.cpp take over and the app sets no
// emulated GPU. False: every hook runs the game's original function.
bool Selected();

// Ends and joins the gta4_renut_capture_s thread (gta4_renut_compat.cpp), if one runs. Called before
// renut::gpu::ShutdownRenderer: the thread reads the frames the renderer serves.
void StopCaptures();

}  // namespace theft4::renut
