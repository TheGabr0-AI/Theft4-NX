// gta4_native: the backend-neutral front end. See native_front_end.h.

#include "native_front_end.h"

#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include <rex/graphics/gta4_native/surface_view.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include "native_alias_registration.h"
#include "native_buffer_metadata.h"
#include "native_constant_diff.h"
#include "native_resolve_clear.h"

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
  if ((draw.sequence % kDrawHashEvery) == kDrawHashEvery / 2) {
    // Run 50: the banks as this draw leaves them, for the recorder to compare with its own (half a period away from the
    // data hash of CaptureDrawData, so the two samples do not share draws).
    draw.constants_hash = {ConstantBankHash(state.vertex_constants.data(), 0, 64, true),
                           ConstantBankHash(state.vertex_constants.data(), 64, kVertexConstantRegisters - 64, true),
                           ConstantBankHash(state.pixel_constants.data(), 0, kPixelConstantRegisters, true)};
    draw.constants_hashed = true;
  }
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
  std::array<bool, kVertexStreamCount + 1> locked{};  // the streams' and (last) the index buffer's lock count is not 0
  if (draw.kind != RecorderDraw::Kind::kUp) {
    for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
      RecorderVertexStream stream = state.streams[s];
      const uint32_t handle = LoadWord(device + kStreamBufferBase + s * 4);
      stream.buffer = handle;
      uint32_t flags = 0;
      if (handle && BufferData(handle, flags, stream.address, stream.size)) {
        stream.fetch_address = (flags & 0xF) == 1;
        locked[s] = (flags & 0xF00u) != 0;  // DecodeNativeBufferMetadata's guest_locked
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
    locked[kVertexStreamCount] = (flags & 0xF00u) != 0;
  }
  for (uint32_t s = 0; s <= kVertexStreamCount; ++s) {
    draw.locked_buffers |= uint32_t(locked[s]) << s;  // run 64
  }
  if (draw.kind != RecorderDraw::Kind::kUp) {
    CaptureDrawData(draw, locked);
  }
  return true;
}

void NativeFrontEnd::PromoteDynamicBuffer(uint32_t handle) {
  std::lock_guard<std::mutex> lock(promote_mutex_);
  promoted_.push_back(handle);
  promote_pending_.store(true, std::memory_order_release);
}

void NativeFrontEnd::TakePromotions() {
  std::lock_guard<std::mutex> lock(promote_mutex_);
  for (uint32_t handle : promoted_) {
    data_stats_.promoted_buffers += promoted_buffers_.insert(handle).second;
  }
  promoted_.clear();
  promote_pending_.store(false, std::memory_order_relaxed);
}

void NativeFrontEnd::CaptureDrawData(RecorderDraw& draw, const std::array<bool, kVertexStreamCount + 1>& locked) {
  if (promote_pending_.load(std::memory_order_acquire)) {
    TakePromotions();
  }
  if (!draw.declaration) {
    return;
  }
  // The streams the declaration reads (the recorder reads a subset of them: the shader's inputs).
  uint32_t mask = 0;
  for (const VertexElement& e : draw.declaration->elements) {
    if (e.stream < kVertexStreamCount && draw.streams[e.stream].address && draw.streams[e.stream].stride) {
      mask |= 1u << e.stream;
    }
  }
  const bool indexed = draw.kind == RecorderDraw::Kind::kIndexed;
  const uint32_t width = draw.index_32bit ? 4 : 2;
  // Run 59: the first reason that applies (CopyReason).
  const bool promotions = handle_promotions_.load(std::memory_order_relaxed);
  uint8_t reason = kCopyNone;
  auto consider = [&](bool is_locked, uint32_t buffer) {
    const uint8_t r = is_locked                                             ? kCopyLocked
                      : dynamic_buffers_.contains(buffer)                   ? kCopyRewritten
                      : promotions && promoted_buffers_.contains(buffer)    ? kCopyPromoted
                                                                            : kCopyNone;
    if (r && (!reason || r < reason)) {
      reason = r;
    }
  };
  for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
    if (mask & (1u << s)) {
      consider(locked[s], draw.streams[s].buffer);
    }
  }
  if (indexed) {
    consider(locked[kVertexStreamCount], draw.index_buffer);
  }
  const bool copies = submit_copies_.load(std::memory_order_relaxed);
  // Run 59: a draw whose buffers touch a page the title rewrote under a waiting draw (VolatilePages): its range decides
  // below. The indices' bytes are known now; a stream's whole data first (cheap), its range once the indices are scanned.
  VolatilePages& pages = GlobalVolatilePages();
  bool page_suspect = false, index_on_page = false;
  if (copies && !reason && volatile_pages_.load(std::memory_order_relaxed) && pages.marked()) {
    for (uint32_t s = 0; s < kVertexStreamCount && !page_suspect; ++s) {
      const RecorderVertexStream& st = draw.streams[s];
      if ((mask & (1u << s)) && st.size > st.offset) {
        page_suspect = pages.Any(IsPhysicalDataAddress(st.address, st.fetch_address), st.address + st.offset,
                                 st.size - st.offset);
      }
    }
    if (indexed) {
      index_on_page = pages.Any(false, draw.index_address + draw.start_index * width, uint64_t(draw.index_count) * width);
      page_suspect = page_suspect || index_on_page;
    }
    data_stats_.page_suspects += page_suspect;
  }
  const bool was_locked = reason == kCopyLocked;
  bool copy = copies && reason != kCopyNone;
  if (!copy && !page_suspect && (draw.sequence % kDrawHashEvery) != 0) {
    return;
  }
  // The same reads as DkRecorder::Draw: the stream's view, its offset, the indices from start_index on.
  std::array<DrawStreamSpan, kVertexStreamCount> spans{};
  for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
    if (!(mask & (1u << s))) {
      continue;
    }
    const RecorderVertexStream& st = draw.streams[s];
    const uint8_t* base = IsPhysicalDataAddress(st.address, st.fetch_address)
                              ? memory_->TranslatePhysical<const uint8_t*>(st.address)
                              : memory_->TranslateVirtual<const uint8_t*>(st.address);
    if (base) {
      spans[s] = {base + st.offset, st.size > st.offset ? uint64_t(st.size - st.offset) : 0, st.stride};
    }
  }
  const uint8_t* indices = nullptr;
  uint64_t index_bytes = 0;
  DrawVertexRange range;
  if (indexed) {
    index_bytes = uint64_t(draw.index_count) * width;
    if (!draw.index_count || uint64_t(draw.start_index) * width + index_bytes > draw.index_size) {
      return;  // the recorder rejects it
    }
    indices = memory_->TranslateVirtual<const uint8_t*>(draw.index_address);
    if (!indices) {
      return;
    }
    indices += size_t(draw.start_index) * width;
    const IndexPolicy policy =
        IndexPolicyFor(draw.primitive_type, draw.primitive_restart, draw.primitive_restart_index, draw.index_32bit);
    range = IndexedVertexRange(indices, draw.index_count, draw.index_32bit, policy, draw.base_vertex);
  } else {
    range = {true, draw.start_vertex, draw.vertex_count};
  }
  if (!range.valid) {
    return;
  }
  if (!copy && page_suspect) {
    // Run 59: the bytes this draw reads, on a marked page or not.
    bool on_page = index_on_page;
    for (uint32_t s = 0; s < kVertexStreamCount && !on_page; ++s) {
      uint64_t start = 0, bytes = 0;
      if ((mask & (1u << s)) && StreamRangeBytes(spans[s], range, start, bytes)) {
        const RecorderVertexStream& st = draw.streams[s];
        on_page = pages.Any(IsPhysicalDataAddress(st.address, st.fetch_address),
                            st.address + st.offset + uint32_t(start), bytes);
      }
    }
    if (on_page) {
      copy = true;
      reason = kCopyPages;
    } else if ((draw.sequence % kDrawHashEvery) != 0) {
      return;
    }
  }
  draw.data_streams = mask;
  if (copy) {
    // Run 50: recycled, no allocation per draw; run 56: the bytes in the pool's arena.
    auto data = GlobalDrawDataPool().TakeCopy(indices, index_bytes, spans.data(), mask, range);
    ++data_stats_.copied_draws;
    data_stats_.copied_locked += was_locked;
    data_stats_.copied_rewritten += reason == kCopyRewritten;
    data_stats_.copied_promoted += reason == kCopyPromoted;
    data_stats_.copied_pages += reason == kCopyPages;
    data_stats_.copied_bytes += data->used;
    draw.data = std::move(data);
    draw.copy_reason = reason;
  } else {
    draw.submit_hash = DrawDataHash(indices, index_bytes, spans.data(), mask, range);
    draw.submit_parts = DrawDataPartHashes(indices, index_bytes, spans.data(), mask, range);
    draw.submit_hashed = true;
    ++data_stats_.hashed_draws;
  }
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
      // Read from the device at each draw (render state, textures, targets).
      return true;
    case CommandType::kDepthSurfaceHandoff: {
      // Passed on in the stream's order (run 44): the recorder makes the primary depth surface hold the resolved scene
      // depth. A malformed one is counted as dropped, as every one was before.
      DepthSurfaceHandoffCommand c;
      if (!Read(command, size, c) || !IsValidForwardStencilHandoffPolicy(c.stencil_policy) || !c.source_texture ||
          !c.destination.handle) {
        ++stats_.ignored_depth_handoff;
        return true;
      }
      RecorderDepthHandoff h;
      h.device = c.device;
      h.source = c.source;
      h.destination = c.destination;
      h.source_texture = c.source_texture;
      h.stencil_policy = c.stencil_policy;
      out.emplace_back(std::move(h));
      ++stats_.depth_handoffs;
      return true;
    }
    case CommandType::kRegisterVirtualResource: {
      // Only the packed depth alias is used (the recorder converts the depth the title reads as bytes); the rest of
      // the registrations (sizes of the title's scaled targets) are counted as dropped.
      RegisterVirtualResourceCommand c;
      if (Read(command, size, c) && IsPackedDepthAliasRegistration(c)) {
        out.emplace_back(RecorderResourceEvent{c.resource, false, c.packed_depth_source});
        ++stats_.packed_depth_aliases;
      } else {
        ++stats_.ignored_virtual_resource;
      }
      return true;
    }
    case CommandType::kRegisterReflectionTarget:
    case CommandType::kUpdateEnvironmentalData:
    case CommandType::kTemporalUpdate:
      // Not used by this front end yet (the depth hand-off, virtual resources, reflections, the temporal path,
      // environmental data: later stages), but counted: the report says how often the title sends them.
      ++stats_.ignored_other;
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
        // Run 49: written again after its first fill, the buffer is one the title rewrites (textures land here too:
        // harmless, draws only look buffers up).
        if (++unlocks_[c.resource] == 2) {
          dynamic_buffers_.insert(c.resource);
        }
      }
      return true;
    }
    case CommandType::kReleaseResource: {
      ReleaseResourceCommand c;
      if (!Read(command, size, c) || !c.resource) {
        return Reject(uint32_t(header.type), "release");
      }
      shader_hashes_.erase(c.resource);
      unlocks_.erase(c.resource);
      dynamic_buffers_.erase(c.resource);
      promoted_buffers_.erase(c.resource);
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
      if (r.command.flags & kResolveClearDepthBit) {
        // The depth clear of a resolve applies to the device's bound depth/stencil, as of this command.
        if (const uint8_t* device = memory_->TranslateVirtual<const uint8_t*>(r.command.device)) {
          r.depth_stencil = CaptureSurface(LoadWord(device + kDepthStencilOffset));
        }
      }
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
