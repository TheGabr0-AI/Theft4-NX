// switch-game: nx_gpu = deko3d (docs/switch-port/16-deko3d-devlog.md, M2).
//
// What this first deko3d build does on the console, with the null GPU keeping the game in step
// (the Liberty Recompiled renderer is not on deko3d yet, so the game's frames are not drawn):
//
//   - plume's deko3d back end (thirdparty/plume-deko3d, banjo-nx's, MIT) makes the device, the queue and a
//     swapchain on the default NWindow; libdeko3dd in the debug NRO checks every call and its errors
//     reach this log through plume's log callback;
//   - for 5 s a test pattern: the four corners red (top left), green (top right), blue (bottom left)
//     and white (bottom right) on dark grey, and in the middle a yellow square at depth 0.25 drawn first
//     with a magenta one at depth 0.75 drawn after over part of it: with the depth test the overlap
//     stays yellow;
//   - the test pattern's two shaders compiled again by the console's uam, compared byte for byte with
//     the DKSH the PC's uam made of the same text (deko3d/test_shaders.h): "identical" or the first
//     differing offset;
//   - when the SD card holds the game's shader package and DKSH cache (made on the PC from the game's
//     own shaders: tools/deko3d/gta4_deko3d_package.py, never in the NRO), the variants compiled in the
//     background by the console's uam, each compared with the PC's DKSH, for up to nx_dk_compile_s;
//   - every 10 s a "deko3d:" line: frames, errors, command and image memory, uam compiles and times,
//     the background compiles, and the draws the null GPU skipped ("rejected").

#pragma once

#include <cstdint>

namespace theft4::nx_deko3d {

using LogFn = void (*)(const char* text);
// The draws the null GPU skipped so far (rex::system::GetNullGpuStats().draws).
using RejectedDrawsFn = uint64_t (*)();

struct Options {
  const char* package_dir = "sdmc:/switch/theft4/deko3d";  // gta4_deko3d_glsl.bin, gta4_deko3d_shaders.bin
  int compile_seconds = 150;                               // background compile budget (0: none)
  int test_pattern_seconds = 5;
};

// Starts the render thread (joined by Stop). False if deko3d did not start (logged).
bool Start(LogFn log, RejectedDrawsFn rejected, const Options& options);
// Asks the render thread to end and joins it.
void Stop();

}  // namespace theft4::nx_deko3d
