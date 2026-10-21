// gta4_native: the thin interface between the front end and the recording of the GPU work
// (docs/switch-port/16-deko3d-devlog.md, M3 B).
//
// The front end (native_front_end.h) takes the title commands of LibertyRecompAndroid's native hooks
// (title_commands.h) and decides everything that is the game's: the state of each draw (core::DecodeFixedState
// of the guest device, the same decode the Vulkan path uses), which shaders and which specialization value, the
// constants that changed, the textures of each fetch slot, the vertex streams and the index buffer, the render
// targets. What it hands over has no graphics API in it: guest handles, guest addresses, fetch constants and
// host-endian values. A recorder turns that into GPU work.
//
// Two implementations: deko3d (deko3d/dk_recorder.h), and Vulkan, whose recording is Liberty Recompiled's own
// (Gta4NativeGraphicsSystem, unchanged: it has its front end and its recorder in one class; moving it behind this
// interface is a later step, written down in the devlog). Rules both follow: everything a recorder needs from
// guest memory that the title may change before the next Present is captured by the front end (constants,
// render state, DrawPrimitiveUp vertices); vertex and index buffers and textures are read by the recorder, which
// the title waits for before reusing them (the completed-frame word, Gta4NativeGraphicsSystem's
// SignalGuestFrameComplete).

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/graphics/gta4_native/title_commands.h>

#include "core/draw_state.h"

namespace rex::graphics::gta4_native {

// The vertex shader's float registers (256) and the pixel shader's (224), as host-endian floats.
inline constexpr uint32_t kVertexConstantRegisters = 256;
inline constexpr uint32_t kPixelConstantRegisters = 224;

// Registers that changed since the previous draw of the same device: runs of 16-byte registers.
struct ConstantRun {
  uint16_t first = 0;
  uint16_t count = 0;
  uint32_t data = 0;  // index of its first float in ConstantDelta::values
};
struct ConstantDelta {
  std::vector<ConstantRun> runs;
  std::vector<float> values;
  // True when this is the whole bank (the first draw of a device): the recorder replaces its copy.
  bool whole = false;
  bool empty() const { return runs.empty(); }
};

// A guest address of vertex or index data: the vertex buffers' come from their fetch constants and may be physical
// (below 0x20000000: read through the physical view) or in a physical window (0xA0000000 and up: the virtual view,
// as Gta4NativeGraphicsSystem reads them); index buffers hold plain pointers (virtual).
inline bool IsPhysicalDataAddress(uint32_t address, bool fetch_address) {
  return fetch_address && address < 0x20000000u;
}

struct RecorderVertexStream {
  uint32_t buffer = 0;   // the guest vertex buffer object (0: unbound)
  uint32_t address = 0;  // its data (IsPhysicalDataAddress says through which view)
  bool fetch_address = false;  // the object holds a fetch constant (type 1), not a pointer (type 2)
  uint32_t size = 0;     // its bytes
  uint32_t offset = 0;   // SetVertexStream
  uint32_t stride = 0;
};

struct RecorderTexture {
  uint32_t handle = 0;               // the guest texture object (0: none)
  std::array<uint32_t, 6> fetch{};   // the device's fetch constant for that slot, host endian
};

struct RecorderVertexDeclaration {
  uint32_t handle = 0;
  uint64_t generation = 0;  // changes when the title registers it again
  std::vector<VertexElement> elements;
};

struct RecorderDraw {
  enum class Kind : uint8_t { kPrimitive, kIndexed, kUp };
  Kind kind = Kind::kPrimitive;
  uint32_t device = 0;
  uint32_t primitive_type = 0;  // xenos::PrimitiveType
  // kPrimitive: start_vertex, vertex_count. kIndexed: base_vertex, start_index, index_count (vertex_count unused).
  // kUp: vertex_count vertices of up_stride bytes in up_vertices.
  uint32_t start_vertex = 0;
  uint32_t vertex_count = 0;
  int32_t base_vertex = 0;
  uint32_t start_index = 0;
  uint32_t index_count = 0;
  uint32_t index_buffer = 0;
  uint32_t index_address = 0;
  uint32_t index_size = 0;
  bool index_32bit = false;
  bool primitive_restart = false;
  uint32_t primitive_restart_index = 0;
  std::shared_ptr<const std::vector<uint8_t>> up_vertices;
  uint32_t up_stride = 0;

  uint64_t vertex_shader = 0;  // shader hashes (0: none)
  uint64_t pixel_shader = 0;
  std::shared_ptr<const RecorderVertexDeclaration> declaration;
  std::array<RecorderVertexStream, kVertexStreamCount> streams{};
  uint32_t texture_mask = 0;  // the slots the two shaders sample
  std::array<RecorderTexture, kTextureStageCount> textures{};
  std::array<SurfaceDescriptor, kRenderTargetCount> render_targets{};
  SurfaceDescriptor depth_stencil{};

  core::FixedFunctionState fixed{};
  uint32_t vertex_booleans = 0;
  uint32_t pixel_booleans = 0;
  ConstantDelta vertex_constants;
  ConstantDelta pixel_constants;
  RenderPhase phase = RenderPhase::kUnknown;
  uint64_t sequence = 0;  // the front end's draw count
};

struct RecorderClear {
  uint32_t device = 0;
  uint32_t flags = 0;  // ClearCommand::flags
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  std::array<uint32_t, 4> color_bits{};
  uint64_t depth_bits = 0;
  uint32_t stencil = 0;
  std::array<SurfaceDescriptor, kRenderTargetCount> render_targets{};
  SurfaceDescriptor depth_stencil{};
  core::FixedFunctionState fixed{};  // the viewport and scissor of the clear
};

struct RecorderResolve {
  ResolveCommand command{};  // as the title sent it
};

struct RecorderPresent {
  PresentCommand command{};
};

// What happened to a guest resource (ResourceUnlock with a guest write, ReleaseResource).
struct RecorderResourceEvent {
  uint32_t handle = 0;
  bool released = false;
};

// Implemented by the recorders. Called on the recorder's own thread, in the title's order.
class INativeRecorder {
 public:
  virtual ~INativeRecorder() = default;
  virtual void Draw(const RecorderDraw& draw) = 0;
  virtual void Clear(const RecorderClear& clear) = 0;
  virtual void Resolve(const RecorderResolve& resolve) = 0;
  // The frame ends: the guest front buffer is shown. Returns once the frame's GPU work is submitted (the guest
  // memory it read is no longer needed).
  virtual void Present(const RecorderPresent& present) = 0;
  virtual void ResourceEvent(const RecorderResourceEvent& event) = 0;
  // Periodic report lines (every 10 s, from the recorder's thread).
  virtual void Report() = 0;
};

}  // namespace rex::graphics::gta4_native
