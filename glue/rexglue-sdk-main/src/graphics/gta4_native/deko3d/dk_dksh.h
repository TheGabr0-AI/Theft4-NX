// gta4_native deko3d back end: what the renderer reads from a DKSH shader module's control section (uam's dksh.h).
// Header only and free of the Switch headers, so tests/deko3d compiles it on the PC.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace rex::graphics::gta4_native::dk {

// The per-warp local ("scratch") memory a deko3d queue gives every shader (DkQueueMaker::perWarpScratchMemorySize).
// deko3d defaults to 2048 bytes. 27 of the package's vertex programs (the skinned and state-machine ones) declare
// 3072 to 92672 bytes in their program headers; run 37 stage 3 stalled the GPU for seconds per work from the moment
// they were first drawn, with no message from libdeko3dd. 96 KiB is above the largest and a multiple of
// DK_PER_WARP_SCRATCH_MEM_ALIGNMENT (0x200).
inline constexpr uint32_t kQueueScratchBytes = 96u << 10;

// DkshHeader { magic, header_sz, control_sz, code_sz, programs_off, num_programs } then DkshProgramHeader[]
// { type, entrypoint, num_gprs, constbuf1_off, constbuf1_sz, per_warp_scratch_sz, ... }.
struct DkshProgram {
  uint32_t control_bytes = 0;
  uint32_t code_bytes = 0;
  uint32_t programs = 0;
  uint32_t per_warp_scratch = 0;  // of the first program
};

// False when the bytes are not a DKSH whose first program header lies inside its control section.
inline bool ParseDksh(std::span<const uint8_t> dksh, DkshProgram& out) {
  struct Header {
    uint32_t magic, header_bytes, control_bytes, code_bytes, programs_offset, programs;
  } h;
  if (dksh.size() < sizeof(h)) {
    return false;
  }
  std::memcpy(&h, dksh.data(), sizeof(h));
  if (h.magic != 0x48534B44u || h.programs == 0 || uint64_t(h.control_bytes) + h.code_bytes > dksh.size()) {
    return false;
  }
  constexpr uint32_t kScratchField = 20;  // byte offset of per_warp_scratch_sz in a program header
  if (uint64_t(h.programs_offset) + kScratchField + 4 > h.control_bytes) {
    return false;
  }
  out.control_bytes = h.control_bytes;
  out.code_bytes = h.code_bytes;
  out.programs = h.programs;
  std::memcpy(&out.per_warp_scratch, dksh.data() + h.programs_offset + kScratchField, 4);
  return true;
}

}  // namespace rex::graphics::gta4_native::dk
