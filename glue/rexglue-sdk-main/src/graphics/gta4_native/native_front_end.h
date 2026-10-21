// gta4_native: the front end of a recorder (native_recorder.h), backend-neutral (docs/switch-port/16-deko3d-devlog.md,
// M3 B).
//
// It reads Liberty Recompiled's title commands the way Gta4NativeGraphicsSystem's producer does
// (ValidateAndCopyCommand, CaptureSurfaceDescriptor, the guest device layout in core/draw_state.h): at each draw
// and clear the guest device's packed state is decoded (core::DecodeFixedState), the float constants are compared
// with what the previous draw of the device had and only the registers that changed travel, the fetch constants
// and texture objects of the slots the shaders sample are read from the device, the vertex streams and the index
// buffer from their guest objects, the render targets and the depth target from the device's surface pointers.
// Nothing here creates or records GPU work.
//
// Thread: Submit is called by the title's threads under one lock (the caller's); the packets go to the recorder's
// thread in order.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

#include "native_recorder.h"

namespace rex::memory {
class Memory;
}

namespace rex::graphics::gta4_native {

using FrontEndPacket =
    std::variant<RecorderDraw, RecorderClear, RecorderResolve, RecorderPresent, RecorderResourceEvent>;

class NativeFrontEnd {
 public:
  // The texture slots a shader samples (its cache entry's usedTextureMask); UINT32_MAX when unknown (every slot is
  // captured).
  using UsedTextureMask = std::function<uint32_t(uint64_t hash)>;

  NativeFrontEnd(memory::Memory* memory, UsedTextureMask used_texture_mask);

  // One title command, after its envelope (GpuPassEnvelope) is removed. False: rejected (logged a few times).
  bool Submit(const void* command, size_t size, std::vector<FrontEndPacket>& out);

  struct Stats {
    uint64_t commands = 0, draws = 0, clears = 0, resolves = 0, presents = 0, rejected = 0;
    uint64_t constant_registers = 0;  // 16-byte registers that travelled
    uint64_t up_bytes = 0;            // DrawPrimitiveUp vertices copied
  };
  const Stats& stats() const { return stats_; }

 private:
  struct DeviceState {
    uint32_t vertex_shader = 0, pixel_shader = 0, declaration = 0, index_buffer = 0;
    std::array<RecorderVertexStream, kVertexStreamCount> streams{};
    bool constants_valid = false;
    std::array<float, kVertexConstantRegisters * 4> vertex_constants{};
    std::array<float, kPixelConstantRegisters * 4> pixel_constants{};
  };

  bool Reject(uint32_t type, const char* reason);
  SurfaceDescriptor CaptureSurface(uint32_t handle) const;
  // The data of a guest vertex or index buffer object (DecodeNativeBufferMetadata); false if it has none.
  bool BufferData(uint32_t handle, uint32_t& flags, uint32_t& address, uint32_t& size) const;
  void CaptureConstants(DeviceState& state, const uint8_t* device, ConstantDelta& vertex, ConstantDelta& pixel);
  bool CaptureDraw(uint32_t device, RecorderDraw& draw);

  memory::Memory* memory_;
  UsedTextureMask used_texture_mask_;
  std::unordered_map<uint32_t, uint64_t> shader_hashes_;  // shader object -> hash
  std::unordered_map<uint32_t, std::shared_ptr<const RecorderVertexDeclaration>> declarations_;
  std::unordered_map<uint32_t, DeviceState> devices_;
  uint64_t declaration_generation_ = 0;
  RenderPhase phase_ = RenderPhase::kUnknown;
  Stats stats_;
};

}  // namespace rex::graphics::gta4_native
