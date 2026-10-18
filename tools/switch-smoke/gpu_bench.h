// switch-smoke step 8: the same GPU workload through NVK (Vulkan) and deko3d
// (gpu_bench.cpp, docs/switch-port/13-deko3d-spike.md, D.2).
#pragma once

// Runs the benchmark twice in the order NVK, deko3d, NVK, deko3d, offscreen
// (no swapchain), at the game's clocks (apm 0x92220008, as cvar nx_apm_clock
// of switch-game), and logs one "gpu bench:" line per measurement and a
// summary. Returns true when both APIs ran every part.
bool RunGpuBench();
