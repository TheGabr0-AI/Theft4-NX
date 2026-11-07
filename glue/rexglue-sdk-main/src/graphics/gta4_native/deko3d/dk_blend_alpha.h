#pragma once

// deko3d 0.5.0's dkCmdBufBindBlendStates writes the colour destination factor into the alpha destination register
// of the target (Maxwell 3D method 0x786 + 8 * target, the sixth of the six IndependentBlend registers 0x781-0x786):
// read in the disassembly of the installed libdeko3d.a (run 48, 16-deko3d-devlog.md section T1; the DkBlendState
// bits 24-29, dstAlphaBlendFactor, are never read). Found first by SwitchWakerHD (deko3d patch 1) and Tenkaichi-3-NX
// (deko_blend_state.h); banjo-nx's plume deko3d back end (thirdparty/plume-deko3d/plume_deko3d.cpp blendWords,
// MIT, ChanseyIsTheBest) rewrites the six registers after the bind. Here only the wrong one is written again.
//
// Plain C++ without deko3d types, so the host test (tests/deko3d/test_blend_alpha.py) compiles it as it is.

#include <cstdint>

namespace rex::graphics::gta4_native::dk {

// A DkBlendFactor as the 3D engine takes it: the low five bits, and 0xC000 for the factors from 0x21 on (the "GL"
// encoding), as deko3d itself encodes the other three factors.
constexpr uint32_t DkBlendFactorWord(uint32_t factor) {
  return (factor & 0x1Fu) | ((factor & 0x20u) ? 0xC000u : 0u);
}

// The command words that write the alpha destination factor of one colour target: an increasing-mode header of one
// word on the 3D subchannel (0), then the factor.
constexpr uint32_t kBlendAlphaDstMethod = 0x786;
inline void DkBlendAlphaDstWords(uint32_t target, uint32_t dst_alpha_factor, uint32_t words[2]) {
  words[0] = (1u << 29) | (1u << 16) | (0u << 13) | ((kBlendAlphaDstMethod + 8u * target) & 0x1FFFu);
  words[1] = DkBlendFactorWord(dst_alpha_factor);
}

}  // namespace rex::graphics::gta4_native::dk
