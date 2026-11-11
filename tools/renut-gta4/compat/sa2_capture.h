// theft4: sa2-nx's lockstep capture as Renut's Swap asks for it (sa2-nx's src/sa2_capture.h). Not ported: no capture
// is taken (tools/renut-gta4/gta4_renut_compat.cpp); the game frame count is kept.
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::ui {
class Presenter;
struct RawImage;
}  // namespace rex::ui

// Game frames: one per D3DDevice_Swap.
extern std::atomic<uint64_t> g_sa2_game_frames;

namespace sa2::capture {

bool RenutLockstepOn();
void RenutLockstepFrame(uint64_t game_frame, const rex::ui::RawImage* image);

}  // namespace sa2::capture
