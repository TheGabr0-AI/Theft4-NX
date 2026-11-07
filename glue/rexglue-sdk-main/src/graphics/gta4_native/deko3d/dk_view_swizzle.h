// gta4_native deko3d back end: the channels a sampled view of a texture carries (docs/switch-port/16-deko3d-devlog.md,
// run 42, section M).
//
// A fetch constant's swizzle (12 bits, 3 per output channel: 0-3 = X..W of the texture, 4 = 0, 5 = 1) is composed with the
// host format's own channel order (`host`, the same 3-bit fields: an R8 texture reads as RRRR, a normal one as RGBA) into
// the swizzle of the image view. For textures read from guest memory that is right: the title's A8R8G8B8 textures carry
// 0x60A (Z, Y, X, W) because their bytes, after the 4-byte swap, are B, G, R, A; DXT textures carry the identity 0x688.
//
// A texture the GPU itself produced (a resolved render target) is different: the render target's lanes are already
// R, G, B, A, and the title's fetch of its A8R8G8B8 resolve carries the same 0x60A, which on the console meets a copy that
// put the bytes in memory order. A copy of the image keeps the lanes as they are, so the view must not apply the
// permutation: run 41's captures show the lit 3D scene with red and blue exchanged (brick walls blue, yellow road lines
// blue) while the HUD and the light accumulation are right. LRA documents the same symptom ("Wrong colors (blue instead of
// yellow)": its renderer is tuned for identity image-view swizzles, docs/android/PORTING.md, and the Vulkan path forces
// identity views on the portability and Android hosts, graphics_system.cpp "ordinary native portability views").
//
// No Switch headers here: tests/deko3d/test_view_swizzle.py compiles this file on the PC.

#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native::dk {

// What one output channel of a view reads: a channel of the image, or a constant.
inline constexpr uint8_t kViewRed = 0, kViewGreen = 1, kViewBlue = 2, kViewAlpha = 3, kViewZero = 4, kViewOne = 5;

// The fetch swizzle that means "as stored" (X, Y, Z, W).
inline constexpr uint32_t kGuestSwizzleIdentity = 0x688;

// `guest` composed with `host` (both 12 bits), for the four output channels.
inline constexpr void ComposeViewSwizzle(uint32_t guest, uint16_t host, uint8_t out[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t v = (guest >> (i * 3)) & 0x7;
    out[i] = v <= 3 ? uint8_t((host >> (v * 3)) & 0x7) : v == 4 ? kViewZero : kViewOne;
  }
}

// The A8R8G8B8 fetch swizzle: Z, Y, X, W, the exchange of red and blue for bytes in memory order.
inline constexpr uint32_t kGuestSwizzleArgbMemory = 0x60A;

// The fetch swizzle a view of a resolved COLOUR image is made with: the lanes as stored where the title's fetch is the
// memory-order exchange (0x60A, see above); any other swizzle is the title's own choice and stays (a 16-bit float or R32F
// resolve, a constant-one alpha). LRA's identity branch drops every swizzle; this is the narrower form, equal to it for
// the fetches run 41 showed. Depth images and the packed depth alias have their own rules and do not come here.
inline constexpr uint32_t ResolvedColourGuestSwizzle(uint32_t fetch_swizzle) {
  return fetch_swizzle == kGuestSwizzleArgbMemory ? kGuestSwizzleIdentity : fetch_swizzle;
}

}  // namespace rex::graphics::gta4_native::dk
