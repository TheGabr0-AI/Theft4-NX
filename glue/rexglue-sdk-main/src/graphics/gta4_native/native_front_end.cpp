// gta4_native: the backend-neutral front end. See native_front_end.h.

#include "native_front_end.h"

#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include <rex/graphics/gta4_native/surface_view.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include "native_buffer_metadata.h"

namespace rex::graphics::gta4_native {
namespace {

// The guest device layout (Gta4NativeGraphicsSystem's, graphics_system.cpp, and core/draw_state.h).
constexpr uint32_t kVertexConstantsOffset = 0x780;
constexpr uint32_t kPixelConstantsOffset = 0x1780;
constexpr uint32_t kBooleansOffset = 0x2780;  // vertex; pixel at +0x10
constexpr uint32_t kTextureFetchBase = 0x480;
constexpr uint32_t kTextureFetchSize = 0x18;
constexpr uint32_t kTextureHandleBase = 0x30F8;
constexpr uint32_t kStreamBufferBase = 12452;
constexpr uint32_t kIndexBufferOffset = 12428;
constexpr uint32_t kRenderTargetOffset = 12432;
constexpr uint32_t kDepthStencilOffset = 12448;
constexpr uint32_t kResourceDataOffset = 24;
constexpr uint32_t kResourceSizeOffset = 28;
constexpr uint32_t kIndex32Flag = 0x80000000;
constexpr uint32_t kMaximumUpPayload = 16u << 20;

uint32_t LoadWord(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

template <typename T>
bool Read(const void* command, size_t size, T& out) {
  if (size != sizeof(T)) {
    return false;
  }
  std::memcpy(&out, command, sizeof(T));
  return out.header.size == sizeof(T);
}

// The registers of [0, count) that differ between `guest` (big-endian floats) and `shadow`, appended as runs; the
// shadow takes the new values. `whole`: every register travels.
uint32_t DiffRegisters(const uint8_t* guest, float* shadow, uint32_t count, bool whole, ConstantDelta& delta) {
  delta.whole = whole;
  uint32_t changed = 0;
  ConstantRun* run = nullptr;
  for (uint32_t r = 0; r < count; ++r) {
    uint32_t words[4];
    for (uint32_t c = 0; c < 4; ++c) {
      words[c] = LoadWord(guest + r * 16 + c * 4);
    }
    if (!whole && std::memcmp(words, shadow + r * 4, 16) == 0) {
      run = nullptr;
      continue;
    }
    std::memcpy(shadow + r * 4, words, 16);
    if (!run) {
      delta.runs.push_back({uint16_t(r), 0, uint32_t(delta.values.size())});
      run = &delta.runs.back();
    }
    ++run->count;
    const float* f = shadow + r * 4;
    delta.values.insert(delta.values.end(), f, f + 4);
    ++changed;
  }
  return changed;
}

}  // namespace

NativeFrontEnd::NativeFrontEnd(memory::Memory* memory, UsedTextureMask used_texture_mask)
    : memory_(memory), used_texture_mask_(std::move(used_texture_mask)) {}

bool NativeFrontEnd::Reject(uint32_t type, const char* reason) {
  ++stats_.rejected;
  if (stats_.rejected <= 32 || !(stats_.rejected % 4096)) {
    REXLOG_WARN("deko3d front end: command {} rejected ({}), {} so far", type, reason, stats_.rejected);
  }
  return false;
}

SurfaceDescriptor NativeFrontEnd::CaptureSurface(uint32_t handle) const {
  // Gta4NativeGraphicsSystem::CaptureSurfaceDescriptor.
  SurfaceDescriptor d;
  d.handle = handle;
  if (!handle) {
    return d;
  }
  const uint8_t* object = memory_->TranslateVirtual<const uint8_t*>(handle);
  if (!object) {
    return d;
  }
  d.flags = LoadWord(object);
  d.base = LoadWord(object + 24);
  d.address = LoadWord(object + 28);
  d.packed_dimensions = LoadWord(object + 36);
  d.format = LoadWord(object + 40);
  d.width = (std::rotl(d.packed_dimensions, 14) & 0x3FFF) + 1;
  d.height = (std::rotl(d.packed_dimensions, 29) & 0x7FFF) + 1;
  d.sample_type = DecodeSurfaceSampleType(d.base);
  return d;
}

bool NativeFrontEnd::BufferData(uint32_t handle, uint32_t& flags, uint32_t& address, uint32_t& size) const {
  const uint8_t* object = handle ? memory_->TranslateVirtual<const uint8_t*>(handle) : nullptr;
  if (!object) {
    return false;
  }
  flags = LoadWord(object);
  const auto metadata =
      DecodeNativeBufferMetadata(flags, LoadWord(object + kResourceDataOffset), LoadWord(object + kResourceSizeOffset));
  if (!metadata || !metadata->HasValidPayload(256u << 20)) {
    return false;
  }
  address = metadata->guest_address;
  size = metadata->guest_size;
  return true;
}

void NativeFrontEnd::CaptureConstants(DeviceState& state, const uint8_t* device, ConstantDelta& vertex,
                                      ConstantDelta& pixel) {
  const bool whole = !state.constants_valid;
  state.constants_valid = true;
  stats_.constant_registers +=
      DiffRegisters(device + kVertexConstantsOffset, state.vertex_constants.data(), kVertexConstantRegisters, whole,
                    vertex);
  stats_.constant_registers +=
      DiffRegisters(device + kPixelConstantsOffset, state.pixel_constants.data(), kPixelConstantRegisters, whole,
                    pixel);
}

bool NativeFrontEnd::CaptureDraw(uint32_t device_address, RecorderDraw& draw) {
  const uint8_t* device = memory_->TranslateVirtual<const uint8_t*>(device_address);
  if (!device) {
    return Reject(uint32_t(CommandType::kDrawPrimitive), "device-translation");
  }
  DeviceState& state = devices_[device_address];
  draw.device = device_address;
  draw.phase = phase_;
  draw.sequence = ++stats_.draws;
  draw.fixed = core::DecodeFixedState(std::span<const uint8_t>(device, kGuestDeviceSize));
  draw.vertex_booleans = LoadWord(device + kBooleansOffset);
  draw.pixel_booleans = LoadWord(device + kBooleansOffset + 0x10);
  CaptureConstants(state, device, draw.vertex_constants, draw.pixel_constants);
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    draw.render_targets[i] = CaptureSurface(LoadWord(device + kRenderTargetOffset + i * 4));
  }
  draw.depth_stencil = CaptureSurface(LoadWord(device + kDepthStencilOffset));

  auto find_hash = [this](uint32_t shader) -> uint64_t {
    const auto it = shader_hashes_.find(shader);
    return it != shader_hashes_.end() ? it->second : 0;
  };
  draw.vertex_shader = find_hash(state.vertex_shader);
  draw.pixel_shader = find_hash(state.pixel_shader);
  if (const auto it = declarations_.find(state.declaration); it != declarations_.end()) {
    draw.declaration = it->second;
  }

  // Textures: the slots the two shaders sample (all of them when a shader is unknown).
  uint32_t mask = 0;
  for (uint64_t hash : {draw.vertex_shader, draw.pixel_shader}) {
    if (hash) {
      mask |= used_texture_mask_ ? used_texture_mask_(hash) : UINT32_MAX;
    }
  }
  mask &= (1u << kTextureStageCount) - 1;
  draw.texture_mask = mask;
  for (uint32_t stage = 0; stage < kTextureStageCount; ++stage) {
    if (!(mask & (1u << stage))) {
      continue;
    }
    RecorderTexture& t = draw.textures[stage];
    const uint8_t* fetch = device + kTextureFetchBase + stage * kTextureFetchSize;
    for (uint32_t w = 0; w < 6; ++w) {
      t.fetch[w] = LoadWord(fetch + w * 4);
    }
    t.handle = LoadWord(device + kTextureHandleBase + stage * 4);
  }

  // Vertex streams: the buffer objects the device holds, with SetVertexStream's offset and stride.
  if (draw.kind != RecorderDraw::Kind::kUp) {
    for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
      RecorderVertexStream stream = state.streams[s];
      const uint32_t handle = LoadWord(device + kStreamBufferBase + s * 4);
      stream.buffer = handle;
      uint32_t flags = 0;
      if (handle && BufferData(handle, flags, stream.address, stream.size)) {
        stream.fetch_address = (flags & 0xF) == 1;
        draw.streams[s] = stream;
      } else {
        draw.streams[s] = {};
      }
    }
  }
  if (draw.kind == RecorderDraw::Kind::kIndexed) {
    const uint32_t handle = LoadWord(device + kIndexBufferOffset);
    uint32_t flags = 0;
    draw.index_buffer = handle;
    if (!handle || !BufferData(handle, flags, draw.index_address, draw.index_size)) {
      return Reject(uint32_t(CommandType::kDrawIndexedPrimitive), "index-buffer");
    }
    draw.index_32bit = (flags & kIndex32Flag) != 0;
  }
  return true;
}

bool NativeFrontEnd::Submit(const void* command, size_t size, std::vector<FrontEndPacket>& out) {
  if (!command || size < sizeof(CommandHeader)) {
    return Reject(0xFFFFFFFFu, "short");
  }
  CommandHeader header;
  std::memcpy(&header, command, sizeof(header));
  if (header.size != size) {
    return Reject(uint32_t(header.type), "size");
  }
  ++stats_.commands;
  switch (header.type) {
    case CommandType::kDeviceCreated:
    case CommandType::kDeviceDestroyed: {
      DeviceCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      devices_.erase(c.device);  // a new device starts with all its constants
      return true;
    }
    case CommandType::kRegisterShader: {
      RegisterShaderCommand c;
      if (!Read(command, size, c) || !c.shader || !c.hash) {
        return Reject(uint32_t(header.type), "invalid-shader");
      }
      shader_hashes_[c.shader] = c.hash;
      return true;
    }
    case CommandType::kRegisterVertexDeclaration: {
      RegisterVertexDeclarationCommand c;
      if (!Read(command, size, c) || !c.declaration || c.element_count > kMaximumVertexElementCount) {
        return Reject(uint32_t(header.type), "invalid-declaration");
      }
      auto d = std::make_shared<RecorderVertexDeclaration>();
      d->handle = c.declaration;
      d->generation = ++declaration_generation_;
      d->elements.assign(c.elements, c.elements + c.element_count);
      declarations_[c.declaration] = std::move(d);
      return true;
    }
    case CommandType::kSetRenderState:
    case CommandType::kSetTexture:
    case CommandType::kSetDepthStencil:
    case CommandType::kSetRenderTarget:
    case CommandType::kDepthSurfaceHandoff:
    case CommandType::kRegisterReflectionTarget:
    case CommandType::kRegisterVirtualResource:
    case CommandType::kUpdateEnvironmentalData:
    case CommandType::kTemporalUpdate:
      // Read from the device at each draw (render state, textures, targets), or not used by this front end yet
      // (reflections, virtual resources, the temporal path, environmental data: later stages).
      return true;
    case CommandType::kSetPixelShader:
    case CommandType::kSetVertexShader: {
      SetShaderCommand c;
      if (size != sizeof(c)) {
        return Reject(uint32_t(header.type), "size");
      }
      std::memcpy(&c, command, sizeof(c));
      DeviceState& state = devices_[c.device];
      (header.type == CommandType::kSetPixelShader ? state.pixel_shader : state.vertex_shader) = c.shader;
      return true;
    }
    case CommandType::kSetVertexDeclaration: {
      SetVertexDeclarationCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      devices_[c.device].declaration = c.declaration;
      return true;
    }
    case CommandType::kSetVertexStream: {
      SetVertexStreamCommand c;
      if (!Read(command, size, c) || c.stream >= kVertexStreamCount) {
        return Reject(uint32_t(header.type), "stream");
      }
      RecorderVertexStream& s = devices_[c.device].streams[c.stream];
      s.buffer = c.buffer;
      s.offset = c.offset;
      s.stride = c.stride;
      return true;
    }
    case CommandType::kSetIndexBuffer: {
      SetIndexBufferCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      devices_[c.device].index_buffer = c.buffer;
      return true;
    }
    case CommandType::kResourceUnlock: {
      ResourceUnlockCommand c;
      if (!Read(command, size, c) || !c.resource) {
        return Reject(uint32_t(header.type), "unlock");
      }
      if (c.access == ResourceUnlockAccess::kGuestWrite) {
        out.emplace_back(RecorderResourceEvent{c.resource, false});
      }
      return true;
    }
    case CommandType::kReleaseResource: {
      ReleaseResourceCommand c;
      if (!Read(command, size, c) || !c.resource) {
        return Reject(uint32_t(header.type), "release");
      }
      shader_hashes_.erase(c.resource);
      out.emplace_back(RecorderResourceEvent{c.resource, true});
      return true;
    }
    case CommandType::kRenderPhaseMarker: {
      RenderPhaseMarkerCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      phase_ = c.event == RenderPhaseEvent::kBegin ? c.phase : RenderPhase::kUnknown;
      return true;
    }
    case CommandType::kDrawPrimitive: {
      DrawPrimitiveCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      RecorderDraw d;
      d.kind = RecorderDraw::Kind::kPrimitive;
      d.primitive_type = c.primitive_type;
      d.start_vertex = c.start_vertex;
      d.vertex_count = c.vertex_count;
      if (!CaptureDraw(c.device, d)) {
        return false;
      }
      out.emplace_back(std::move(d));
      return true;
    }
    case CommandType::kDrawIndexedPrimitive: {
      DrawIndexedPrimitiveCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      RecorderDraw d;
      d.kind = RecorderDraw::Kind::kIndexed;
      d.primitive_type = c.primitive_type;
      d.base_vertex = c.base_vertex;
      d.start_index = c.start_index;
      d.index_count = c.index_count;
      d.primitive_restart = c.primitive_restart_enabled != 0;
      d.primitive_restart_index = c.primitive_restart_index;
      if (!CaptureDraw(c.device, d)) {
        return false;
      }
      out.emplace_back(std::move(d));
      return true;
    }
    case CommandType::kDrawPrimitiveUp: {
      DrawPrimitiveUpCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      if (c.vertex_data_size > kMaximumUpPayload || (c.vertex_data_size && !c.vertex_data)) {
        return Reject(uint32_t(header.type), "up-payload");
      }
      RecorderDraw d;
      d.kind = RecorderDraw::Kind::kUp;
      d.primitive_type = c.primitive_type;
      d.vertex_count = c.vertex_count;
      d.up_stride = c.stride;
      if (c.vertex_data_size) {
        const uint8_t* data = memory_->TranslateVirtual<const uint8_t*>(c.vertex_data);
        if (!data) {
          return Reject(uint32_t(header.type), "up-translation");
        }
        // The title rewrites this memory right after the call: copied now.
        d.up_vertices = std::make_shared<const std::vector<uint8_t>>(data, data + c.vertex_data_size);
        stats_.up_bytes += c.vertex_data_size;
      }
      if (!CaptureDraw(c.device, d)) {
        return false;
      }
      out.emplace_back(std::move(d));
      return true;
    }
    case CommandType::kClear: {
      ClearCommand c;
      if (!Read(command, size, c)) {
        return Reject(uint32_t(header.type), "size");
      }
      const uint8_t* device = memory_->TranslateVirtual<const uint8_t*>(c.device);
      if (!device) {
        return Reject(uint32_t(header.type), "device-translation");
      }
      RecorderClear r;
      r.device = c.device;
      r.flags = c.flags;
      r.left = c.left;
      r.top = c.top;
      r.right = c.right;
      r.bottom = c.bottom;
      std::copy(std::begin(c.color_bits), std::end(c.color_bits), r.color_bits.begin());
      r.depth_bits = c.depth_bits;
      r.stencil = c.stencil;
      for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
        r.render_targets[i] = CaptureSurface(LoadWord(device + kRenderTargetOffset + i * 4));
      }
      r.depth_stencil = CaptureSurface(LoadWord(device + kDepthStencilOffset));
      r.fixed = core::DecodeFixedState(std::span<const uint8_t>(device, kGuestDeviceSize));
      ++stats_.clears;
      out.emplace_back(std::move(r));
      return true;
    }
    case CommandType::kResolve: {
      RecorderResolve r;
      if (!Read(command, size, r.command) || !r.command.source.handle || !r.command.destination_texture) {
        return Reject(uint32_t(header.type), "resolve");
      }
      ++stats_.resolves;
      out.emplace_back(std::move(r));
      return true;
    }
    case CommandType::kPresent: {
      RecorderPresent p;
      if (!Read(command, size, p.command)) {
        return Reject(uint32_t(header.type), "size");
      }
      ++stats_.presents;
      out.emplace_back(std::move(p));
      return true;
    }
    default:
      return Reject(uint32_t(header.type), "type");
  }
}

}  // namespace rex::graphics::gta4_native
