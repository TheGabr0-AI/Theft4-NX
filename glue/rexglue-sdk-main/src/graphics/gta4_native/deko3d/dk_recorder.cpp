// gta4_native deko3d back end: the recorder. See dk_recorder.h.

#include "dk_recorder.h"

#include <string_view>
#include <functional>
#include <switch.h>

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/xmemory.h>

#include <xxhash.h>

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "../core/geometry.h"
#include "../native_constant_diff.h"
#include "../native_fixed_function_policy.h"
#include "../native_sampler_lod_bias.h"
#include "../native_shader_booleans.h"
#include "../native_resolve_clear.h"
#include "../native_stencil_volume_policy.h"
#include "dk_blend_alpha.h"
#include "dk_draw_rules.h"
#include "dk_rect_list.h"
#include "dk_shader_probe.h"
#include "dk_skin_check.h"

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include "dk_anatomy.h"
#include "dk_png.h"
#include "dk_vertex_convert.h"
#include "dk_view_swizzle.h"
#include "dk_xenos.h"

namespace rex::graphics::gta4_native::dk {
namespace {

constexpr uint32_t kSlots = 3;
constexpr uint32_t kVertexBlockBytes = kVertexConstantRegisters * 16;  // 4096
constexpr uint32_t kPixelBlockBytes = 4096;  // 224 registers, and up to 256 for the shaders that declare them
constexpr uint32_t kSharedBlockBytes = 1024;  // g_DkShared[64] (XenosRecomp's GTA4_DEKO3D)
constexpr uint32_t kMaxVerticesPerDraw = 1u << 20;
constexpr uint64_t kFramesUnusedToRelease = 120;
constexpr uint64_t kTextureLimitBytes = 256ull << 20;
// Run 47 filled the 8,192 image descriptors at ~415 s with 195 MB of textures (about 20 KB each, so the byte limit
// never released one) and every later view got descriptor 0, the black texture. Run 48: 32,768 descriptors (1 MB of
// table) and, below kDescriptorReserve free ones, unused textures are released as for the byte limit.
constexpr uint32_t kImageDescriptors = 32768;
constexpr uint32_t kDescriptorReserve = 2048;
constexpr uint32_t kAlphaTestCapability = 0x702;
// Shared constant offsets (core::SharedConstants, shader_common.h's GTA4_DEKO3D macros).
constexpr uint32_t kSharedBooleans = 0x210;
constexpr uint32_t kSharedHalfPixel = 0x228;
constexpr uint32_t kSharedClipPlane = 0x230;
constexpr uint32_t kSharedClipPlaneEnabled = 0x240;
constexpr uint32_t kSharedAlphaThreshold = 0x244;
constexpr uint32_t kSharedAlphaToMask = 0x2E0;
constexpr uint32_t kSharedAlphaToMaskSamples = 0x2E4;
constexpr uint32_t kSharedFragmentScale = 0x2E8;
constexpr uint32_t kSharedLodBias = 752;  // GetTextureLodBias(SLOT): 752 + 4 * SLOT (run 53)

// Skip reasons (the report counts draws by these).
enum : uint32_t {
  kSkipStage1 = 1,
  kSkip3dBeforeStage3 = 2,
  kSkipNoVertexShader = 10,
  kSkipNoPixelShader = 11,
  kSkipCoverage = 12,
  kSkipShaderPending = 13,
  kSkipShaderMissing = 14,
  kSkipNoTargets = 20,
  kSkipEmptyViewport = 21,
  kSkipVertexFormat = 30,
  kSkipVertexData = 31,
  kSkipIndexData = 32,
  kSkipPrimitive = 33,
  kSkipRectIndexed = 34,
  kSkipUploadFull = 40,
  kSkipTooManyVertices = 41,
};

constexpr uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

uint32_t ToU32(float f) { return std::bit_cast<uint32_t>(f); }

DkBlendFactor BlendFactor(uint32_t f) {
  switch (f) {
    case 1: return DkBlendFactor_One;
    case 4: return DkBlendFactor_SrcColor;
    case 5: return DkBlendFactor_InvSrcColor;
    case 6: return DkBlendFactor_SrcAlpha;
    case 7: return DkBlendFactor_InvSrcAlpha;
    case 8: return DkBlendFactor_DstColor;
    case 9: return DkBlendFactor_InvDstColor;
    case 10: return DkBlendFactor_DstAlpha;
    case 11: return DkBlendFactor_InvDstAlpha;
    case 12: return DkBlendFactor_ConstColor;
    case 13: return DkBlendFactor_InvConstColor;
    case 14: return DkBlendFactor_ConstAlpha;
    case 15: return DkBlendFactor_InvConstAlpha;
    case 16: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_Zero;
  }
}

DkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 1: return DkBlendOp_Sub;
    case 2: return DkBlendOp_Min;
    case 3: return DkBlendOp_Max;
    case 4: return DkBlendOp_RevSub;
    default: return DkBlendOp_Add;
  }
}

// Xenos compare functions and stencil operations are deko3d's minus one.
DkCompareOp CompareOp(uint32_t f) { return DkCompareOp((f & 7) + 1); }
DkStencilOp StencilOp(uint32_t op) { return DkStencilOp((op & 7) + 1); }

template <class T>
T Zeroed() {
  T t;
  std::memset(&t, 0, sizeof(t));
  return t;
}

template <class T>
bool Same(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// The fetch constant's swizzle through the host format's channels (nfsmw_dk_texturas.cpp's Swizzle).
constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
void ViewSwizzle(uint32_t guest, uint16_t host, DkImageSwizzle out[4]) {
  uint8_t code[4];
  ComposeViewSwizzle(guest, host, code);  // dk_view_swizzle.h
  for (uint32_t i = 0; i < 4; ++i) {
    out[i] = code[i] == kViewZero  ? DkImageSwizzle_Zero
             : code[i] == kViewOne ? DkImageSwizzle_One
                                   : DkImageSwizzle(DkImageSwizzle_Red + code[i]);
  }
}

// A guest render target's format (Gta4NativeGraphicsSystem's ConvertSurfaceFormat; D24FS8 as 32-bit float depth with
// stencil, as Vulkan's D32_SFLOAT_S8_UINT there).
DkImageFormat SurfaceFormat(uint32_t raw, bool depth) {
  if (depth) {
    return DkImageFormat_ZF32_X24S8;
  }
  switch (raw) {
    case 0x18280186: return DkImageFormat_RGBA8_Unorm;      // A8R8G8B8
    case 0x1A2201BF: return DkImageFormat_RGBA16_Float;     // A16B16G16R16F
    case 0x2DA2ABA4: return DkImageFormat_R32_Float;        // R32F
    case 0x2D22AB9F:
    case 0x2D20AB8D: return DkImageFormat_RG16_Float;       // G16R16F
    default:
      break;
  }
  switch (raw & 0x3F) {  // the Xenos texture format in the low bits
    case 6: return DkImageFormat_RGBA8_Unorm;
    case 7: return DkImageFormat_RGB10A2_Unorm;
    case 31: return DkImageFormat_RG16_Float;
    case 32: return DkImageFormat_RGBA16_Float;
    case 36: return DkImageFormat_R32_Float;
    default: return DkImageFormat_None;
  }
}

// A resolve destination's host format from its fetch constant's format.
DkImageFormat ResolveFormat(uint32_t format, DkImageFormat source) {
  switch (format) {
    case 6:
    case 50: return DkImageFormat_RGBA8_Unorm;
    case 7:
    case 54: return DkImageFormat_RGB10A2_Unorm;
    case 31: return DkImageFormat_RG16_Float;
    case 32: return DkImageFormat_RGBA16_Float;
    case 36: return DkImageFormat_R32_Float;
    case 22:
    case 23: return DkImageFormat_ZF32_X24S8;
    default: return source;
  }
}

uint32_t BytesPerTexel(DkImageFormat f) {
  switch (f) {
    case DkImageFormat_RGBA16_Float: return 8;
    case DkImageFormat_ZF32_X24S8: return 8;
    default: return 4;
  }
}

// A vertex element type (D3DDECLTYPE as the title packs it) as deko3d's attribute for a shader input of that
// numeric type (Gta4NativeGraphicsSystem's ConvertVertexElementFormat and GetCompatibleVertexFormat). `fix`: what
// the upload must change in the data: 1 the 16-bit components' halves, 2 DEC3N's w, 3 R and B of a D3DCOLOR read
// as integers.
bool VertexAttribute(uint32_t type, uint8_t numeric, DkVtxAttribState& a, uint32_t& fix) {
  fix = 0;
  a.isBgra = 0;
  const bool uint_in = numeric == 2, sint_in = numeric == 1;
  auto f32 = [&](DkVtxAttribSize s) {
    a.size = s;
    a.type = uint_in ? DkVtxAttribType_Uint : sint_in ? DkVtxAttribType_Sint : DkVtxAttribType_Float;
    return true;
  };
  switch (type) {
    case 0x2C83A4: return f32(DkVtxAttribSize_1x32);
    case 0x2C23A5: return f32(DkVtxAttribSize_2x32);
    case 0x2A23B9: return f32(DkVtxAttribSize_3x32);
    case 0x1A23A6: return f32(DkVtxAttribSize_4x32);
    case 0x182886:  // D3DCOLOR
      a.size = DkVtxAttribSize_4x8;
      if (uint_in) {
        a.type = DkVtxAttribType_Uint;
        fix = 3;
      } else {
        a.type = DkVtxAttribType_Unorm;
        a.isBgra = 1;
      }
      return true;
    case 0x1A2286:
    case 0x1A2386:
      a.size = DkVtxAttribSize_4x8;
      a.type = DkVtxAttribType_Uint;
      return true;
    case 0x1A2086:
    case 0x1A2186:
      a.size = DkVtxAttribSize_4x8;
      a.type = uint_in ? DkVtxAttribType_Uint : DkVtxAttribType_Unorm;
      return true;
    case 0x2C2359:
    case 0x1A235A:
      a.size = type == 0x2C2359 ? DkVtxAttribSize_2x16 : DkVtxAttribSize_4x16;
      a.type = uint_in ? DkVtxAttribType_Uint : sint_in ? DkVtxAttribType_Sint : DkVtxAttribType_Sscaled;
      fix = 1;
      return true;
    case 0x2C2159:
    case 0x1A215A:
      a.size = type == 0x2C2159 ? DkVtxAttribSize_2x16 : DkVtxAttribSize_4x16;
      a.type = sint_in ? DkVtxAttribType_Sint : DkVtxAttribType_Snorm;
      fix = 1;
      return true;
    case 0x2C2059:
    case 0x1A205A:
      a.size = type == 0x2C2059 ? DkVtxAttribSize_2x16 : DkVtxAttribSize_4x16;
      a.type = uint_in ? DkVtxAttribType_Uint : DkVtxAttribType_Unorm;
      fix = 1;
      return true;
    case 0x2C235F:
    case 0x1A2360:
      if (uint_in || sint_in) {
        return false;
      }
      a.size = type == 0x2C235F ? DkVtxAttribSize_2x16 : DkVtxAttribSize_4x16;
      a.type = DkVtxAttribType_Float;
      fix = 1;
      return true;
    case 0x2C82A1:
    case 0x2A2287:
    case 0x2A2187:
    case 0x2A2190:
    case 0x2A2390:
      a.size = DkVtxAttribSize_1x32;
      a.type = DkVtxAttribType_Uint;
      return true;
    case 0x1A2187:  // DEC3N: the upload sets its implicit w = 1 in the two high bits
      a.size = DkVtxAttribSize_10_10_10_2;
      a.type = DkVtxAttribType_Snorm;
      fix = 2;
      return true;
    default:
      return false;
  }
}

bool PrimitiveOf(uint32_t type, DkPrimitive& p) {
  switch (type) {
    case 1: p = DkPrimitive_Points; return true;
    case 2: p = DkPrimitive_Lines; return true;
    case 3: p = DkPrimitive_LineStrip; return true;
    case 4: p = DkPrimitive_Triangles; return true;
    case 5: p = DkPrimitive_TriangleFan; return true;
    case 6: p = DkPrimitive_TriangleStrip; return true;
    case 12: p = DkPrimitive_LineLoop; return true;
    case 13: p = DkPrimitive_Quads; return true;
    case 14: p = DkPrimitive_QuadStrip; return true;
    case 15: p = DkPrimitive_Polygon; return true;
    default: return false;
  }
}

// The output pass: a full-screen triangle sampling the guest front buffer (texture coordinates scaled to the part
// of the image the front buffer covers), with the gamma of nx_dk_gamma.
constexpr char kOutputVs[] = R"(#version 460
layout(std140, binding = 0) uniform Output { vec4 scale_gamma; } u;
layout(location = 0) out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
  gl_Position = vec4(p, 0.0, 1.0);
  v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) * u.scale_gamma.xy;
}
)";
constexpr char kOutputPs[] = R"(#version 460
layout(std140, binding = 0) uniform Output { vec4 scale_gamma; } u;
layout(binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() {
  vec3 c = textureLod(source, v_uv, 0.0).rgb;
  if (u.scale_gamma.z != 1.0) c = pow(max(c, vec3(0.0)), vec3(u.scale_gamma.z));
  o = vec4(c, 1.0);
}
)";
constexpr char kEmptyPs[] = "#version 460\nvoid main() {}\n";

}  // namespace

// ---- Construction

DkRecorder::DkRecorder(Device& device, memory::Memory* memory, ShaderPackage& package, Compiler& compiler,
                       RecorderOptions options)
    : device_(device), memory_(memory), package_(package), compiler_(compiler), options_(std::move(options)) {
  ab_keep_targets_ = options_.keep_targets;
  ab_placement_ = options_.placement;
  ab_ = AbSchedule(1500, options_.ab_phase_mask);
}

DkRecorder::~DkRecorder() {
  if (core_.WorkOpen()) {
    CloseWork();
  }
  if (core_.device()) {
    core_.WaitIdle();
  }
  if (capture_writer_.joinable()) {
    capture_writer_.join();
  }
  if (anatomy_writer_.joinable()) {
    anatomy_writer_.join();
  }
  swapchain_.Destroy();
  shaders_.clear();
  geometry_.Clear();
  if (geometry_block_) {
    device_.DestroyBlock(geometry_block_);
    geometry_block_ = nullptr;
  }
  core_.Destroy();
}

bool DkRecorder::Initialize() {
  CoreConfig config;
  config.slots = kSlots;
  // Run 50: 64 (was 32): the constant versions add up to 8 KB per draw (run 49 ended with 1,245 MiB of heap headroom).
  config.upload_mb = 64;
  config.command_kb = 2048;
  config.pre_kb = 512;
  config.code_mb = 48;
  config.image_chunk_mb = 32;
  config.image_descriptors = kImageDescriptors;
  config.sampler_descriptors = 1024;
  if (!core_.Create(device_, config)) {
    return false;
  }
  descriptors_to_free_.resize(kSlots);
  slot_stamps_.resize(kSlots);
  // Run 62: the stage counters' reports after the pass stamps in the same block (64 KiB: 48,000 bytes of stamps, room
  // for 32 sampled draws a work in each of the three slots).
  stage_layout_ = StageCounterLayout::Fit(kSlots * kStampsPerSlot * 16, core_.config().query_kb << 10, kSlots, 32);
  stage_slot_samples_.resize(kSlots);
  {
    GeometryStore::Options g;
    g.mb = options_.geometry_mb;
    g.trust_epoch = options_.geometry_trust;
    const bool made = geometry_.Create(
        g, kSlots,
        [this](uint32_t bytes) -> std::pair<uint8_t*, DkGpuAddr> {
          geometry_block_ = device_.CreateBlock(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                                "geometry store");
          if (!geometry_block_) {
            return {nullptr, 0};
          }
          return {static_cast<uint8_t*>(dkMemBlockGetCpuAddr(geometry_block_)), dkMemBlockGetGpuAddr(geometry_block_)};
        },
        [this](uint64_t work) { return core_.WorkDone(work); });
    REXLOG_INFO("deko3d: geometry store (run 52, nfsmw-nx's): {} MiB, trust per epoch {} (run 53){}",
                options_.geometry_mb, options_.geometry_trust ? "on" : "off",
                made ? "" : ", NOT created: every draw's vertices go to the upload ring");
  }
  if (!swapchain_.Create(device_, options_.output_width, options_.output_height, 1)) {
    REXLOG_ERROR("deko3d: the swapchain was not created");
    return false;
  }
  if (!CreateOutputShaders()) {
    return false;
  }
  EnsureWork();
  if (!CreateEmpties()) {
    return false;
  }
  CloseWork();
  if (options_.capture_seconds > 0) {
    mkdir(options_.capture_dir.c_str(), 0777);
    capture_next_ns_ = NowNs() + uint64_t(options_.capture_seconds) * 1'000'000'000ull;
  }
  report_ns_ = NowNs();
  REXLOG_INFO("deko3d: recorder ready: output {}x{}, stage {} (nx_dk_stage), captures every {} s in {}",
              options_.output_width, options_.output_height, options_.stage ? options_.stage() : 0,
              options_.capture_seconds, options_.capture_dir);
  return true;
}

bool DkRecorder::CreateOutputShaders() {
  struct Own {
    DkShader* shader;
    bool vertex;
    const char* text;
    const char* name;
  };
  static DkShader empty_ps;
  const Own own[] = {{&output_vs_, true, kOutputVs, "output (vertex)"},
                     {&output_ps_, false, kOutputPs, "output (pixel)"},
                     {&empty_ps, false, kEmptyPs, "empty pixel shader"}};
  for (const Own& o : own) {
    const Compiler::Result r = compiler_.Compile(o.vertex, o.text);
    if (!r.ok || !core_.LoadShader(*o.shader, r.dksh, o.name)) {
      REXLOG_ERROR("deko3d: the renderer's shader '{}' did not compile: {}", o.name, r.log);
      return false;
    }
    REXLOG_INFO("deko3d: {} compiled by the console's uam in {:.1f} ms", o.name, double(r.ns) / 1e6);
  }
  // The empty pixel shader is kept as the "no pixel shader" variant (depth-only draws).
  shaders_[ShaderKey{0, 0, false}] = std::make_unique<DkShader>(empty_ps);
  output_valid_ = true;

  // The packed depth alias pass (dk_depth_alias.h). Not fatal: without it an A8R8G8B8 fetch of a resolved depth keeps
  // getting the depth image, as before.
  const Compiler::Result alias_vs = compiler_.Compile(true, kPackedDepthAliasVs);
  const Compiler::Result alias_ps = compiler_.Compile(false, kPackedDepthAliasPs);
  alias_valid_ = alias_vs.ok && alias_ps.ok && core_.LoadShader(alias_vs_, alias_vs.dksh, "packed depth alias (vertex)") &&
                 core_.LoadShader(alias_ps_, alias_ps.dksh, "packed depth alias (pixel)");
  if (alias_valid_) {
    REXLOG_INFO("deko3d: packed depth alias pass compiled by the console's uam in {:.1f} ms",
                double(alias_vs.ns + alias_ps.ns) / 1e6);
  } else {
    REXLOG_ERROR("deko3d: the packed depth alias pass did not compile (A8R8G8B8 fetches of a resolved depth stay "
                 "wrong): {} {}",
                 alias_vs.log, alias_ps.log);
  }

  // The scene depth hand-off's pixel shader (dk_depth_handoff.h), with the alias pass's vertex shader. Not fatal: without it
  // the title's primary depth surface keeps not receiving the scene depth, as before run 44.
  const Compiler::Result handoff_ps = compiler_.Compile(false, kSceneDepthHandoffPs);
  handoff_valid_ = alias_valid_ && handoff_ps.ok && core_.LoadShader(handoff_ps_, handoff_ps.dksh, "scene depth hand-off (pixel)");
  if (handoff_valid_) {
    REXLOG_INFO("deko3d: scene depth hand-off pass compiled by the console's uam in {:.1f} ms", double(handoff_ps.ns) / 1e6);
  } else {
    REXLOG_ERROR("deko3d: the scene depth hand-off pass did not compile (the primary depth surface stays without the "
                 "scene depth): {}",
                 handoff_ps.log);
  }

  // Run 57: the EDRAM placement conversion (dk_placement.h), with the alias pass's vertex shader. Not fatal: without it a
  // surface that shares another's placement only gets its content when the views are identical (an image copy).
  const Compiler::Result placement_ps = compiler_.Compile(false, kPlacementPs);
  placement_valid_ = alias_valid_ && placement_ps.ok && core_.LoadShader(placement_ps_, placement_ps.dksh, "EDRAM placement (pixel)");
  if (placement_valid_) {
    REXLOG_INFO("deko3d: EDRAM placement pass compiled by the console's uam in {:.1f} ms", double(placement_ps.ns) / 1e6);
  } else {
    REXLOG_ERROR("deko3d: the EDRAM placement pass did not compile (views of another sample layout stay empty): {}",
                 placement_ps.log);
  }
  return true;
}

// ---- EDRAM placements (run 57, dk_placement.h)

void DkRecorder::ClaimPlacement(const SurfaceDescriptor& d, DkImageEntry& image, bool depth) {
  if (!options_.placement) {
    return;
  }
  GuestSurfaceView view;
  if (!DecodeGuestSurfaceView(d, depth, view)) {
    return;
  }
  const uint64_t serial = ++placement_serial_;
  placement_[GetGuestPlacementKey(view)] = {SurfaceKey{d.handle, d.format, d.width, d.height, depth}, image.serial, view,
                                            serial};
  image.placement_serial = serial;
}

void DkRecorder::PrepareSurfaceContent(const SurfaceDescriptor& d, DkImageEntry& image, bool depth) {
  if (!options_.placement || depth) {
    return;  // colour only, as the Vulkan path (it refuses depth views)
  }
  GuestSurfaceView view;
  if (!DecodeGuestSurfaceView(d, false, view)) {
    return;
  }
  const auto it = placement_.find(GetGuestPlacementKey(view));
  if (it == placement_.end() || image.placement_serial == it->second.serial) {
    return;  // nobody else wrote the placement, or this image holds its current content already
  }
  PlacementOwner& o = it->second;
  const auto found = surfaces_.find(o.surface);
  if (found == surfaces_.end() || !found->second.live || found->second.serial != o.image_serial) {
    ++placement_stats_.stale;  // the owner was released or made again: its content is gone
    placement_.erase(it);
    return;
  }
  DkImageEntry& owner = found->second;
  if (&owner == &image) {
    image.placement_serial = o.serial;
    return;
  }
  const PlacementCopy plan = PlanPlacementCopy(o.view, owner.width, owner.height, view, image.width, image.height);
  const bool copy = plan.valid && plan.same_layout && owner.format == image.format;
  if (!plan.valid || owner.depth || image.depth || (!copy && !placement_valid_)) {
    ++placement_stats_.refused;
    if (placement_logged_.size() < 24 && placement_logged_.insert((uint64_t(o.surface.handle) << 32) | d.handle).second) {
      REXLOG_INFO("deko3d: EDRAM placement of {:08X} ({}x{}, samples {}) held by {:08X} ({}x{}, samples {}): not "
                  "materialized (sizes do not fit the sample space, a depth, or no conversion pass)",
                  d.handle, image.width, image.height, uint32_t(view.msaa_samples), o.surface.handle, owner.width,
                  owner.height, uint32_t(o.view.msaa_samples));
    }
    return;
  }
  EnsureWork();
  if (copy) {
    BeforeTransfer();
    DkImageView from, to;
    dkImageViewDefaults(&from, &owner.image);
    dkImageViewDefaults(&to, &image.image);
    const DkImageRect rect = {0, 0, 0, image.width, image.height, 1};
    dkCmdBufCopyImage(core_.commands(), &from, &rect, &to, &rect, 0);
    ++placement_stats_.copies;
  } else {
    // The view and the constants first (an upload may move on to the next work slot), then the pass.
    const uint32_t source_view = ViewOf(owner, 0x688, kSwizzleRGBA);
    const int32_t factor[4] = {int32_t(plan.reduce_x), int32_t(plan.reduce_y), int32_t(PlacementShift(plan.expand_x)),
                               int32_t(PlacementShift(plan.expand_y))};
    const Upload ubo = Reserve(sizeof(factor), DK_UNIFORM_BUF_ALIGNMENT);
    if (!source_view || !ubo) {
      ++placement_stats_.refused;
      return;
    }
    std::memcpy(ubo.cpu, factor, sizeof(factor));
    Before3D();
    DkCmdBuf cmd = core_.commands();
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);  // the owner's draws are done before it is read
    ++barriers_;
    DkImageView target;
    dkImageViewDefaults(&target, &image.image);
    dkCmdBufBindRenderTarget(cmd, &target, nullptr);
    const DkViewport viewport = {0.0f, 0.0f, float(image.width), float(image.height), 0.0f, 1.0f};
    const DkScissor all = {0, 0, image.width, image.height};
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    dkCmdBufSetScissors(cmd, 0, &all, 1);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    DkColorState cs;
    dkColorStateDefaults(&cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = false;
    ds.depthWriteEnable = false;
    dkCmdBufBindRasterizerState(cmd, &rs);
    dkCmdBufBindColorState(cmd, &cs);
    dkCmdBufBindColorWriteState(cmd, &cw);
    dkCmdBufBindDepthStencilState(cmd, &ds);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    const DkShader* pair[2] = {&alias_vs_, &placement_ps_};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, sizeof(factor));
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(source_view, 0));
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    ++barriers_;
    ForgetState();  // the targets, shaders and state are the draw's to emit again
    ++placement_stats_.conversions;
  }
  if (placement_logged_.size() < 24 && placement_logged_.insert((uint64_t(o.surface.handle) << 32) | d.handle).second) {
    REXLOG_INFO("deko3d: EDRAM placement of {:08X} ({}x{}, samples {}) materialized from {:08X} ({}x{}, samples {}) by "
                "{} (reduce {}x{}, expand {}x{})",
                d.handle, image.width, image.height, uint32_t(view.msaa_samples), o.surface.handle, owner.width,
                owner.height, uint32_t(o.view.msaa_samples), copy ? "an image copy" : "the conversion pass",
                plan.reduce_x, plan.reduce_y, plan.expand_x, plan.expand_y);
  }
  // As the Vulkan path: the destination holds the content now and becomes the placement's owner, same serial.
  image.placement_serial = o.serial;
  o.surface = SurfaceKey{d.handle, d.format, d.width, d.height, false};
  o.image_serial = image.serial;
  o.view = view;
}

// ---- Works and barriers

void DkRecorder::EnsureWork() {
  if (core_.WorkOpen()) {
    return;
  }
  core_.BeginWork(++work_);
  geometry_.NewWork(work_);
  // The descriptors freed the last time this slot was recorded: its fence has just been waited for.
  auto& freed = descriptors_to_free_[work_ % kSlots];
  free_descriptors_.insert(free_descriptors_.end(), freed.begin(), freed.end());
  freed.clear();
  HarvestStamps(uint32_t(work_ % kSlots));
  HarvestStageCounters(uint32_t(work_ % kSlots));
  BeginWorkState();
  StampPass(0, 0, 0, 0, 0);  // the start of the work: what the GPU did before it is another work's or idle
}

// ---- GPU time per pass (run 44)

void DkRecorder::StampPass(uint32_t kind, uint32_t first, uint32_t second, uint32_t width, uint32_t height) {
  if (!core_.WorkOpen() || slot_stamps_.size() != kSlots) {
    return;
  }
  const uint32_t slot = uint32_t(work_ % kSlots);
  std::vector<Stamp>& stamps = slot_stamps_[slot];
  if (stamps.size() >= kStampsPerSlot - (kind == 5 ? 0 : 1)) {  // the last one is the end of the work's
    return;
  }
  const uint64_t key = (uint64_t(kind) << 60) ^ (uint64_t(first) << 28) ^ (uint64_t(second) * 0x9E3779B1ull) ^
                       (uint64_t(width) << 14) ^ height;
  uint32_t label;
  if (const auto it = label_ids_.find(key); it != label_ids_.end()) {
    label = it->second;
  } else {
    static const char* const kNames[] = {"start of a work", "targets", "resolve", "alias", "hand-off", "end of a work"};
    label = uint32_t(labels_.size());
    labels_.push_back({fmt::format("{} {:08X} {:08X} {}x{}", kNames[kind < 6 ? kind : 5], first, second, width, height)});
    label_ids_.emplace(key, label);
  }
  dkCmdBufReportCounter(core_.commands(), DkCounter_Timestamp,
                        core_.QueriesGpu() + (uint64_t(slot) * kStampsPerSlot + stamps.size()) * 16);
  stamps.push_back({label, 0});
}

void DkRecorder::HarvestStageCounters(uint32_t slot) {
  // Run 62: the slot's fence has been waited for (BeginWork), so its reports are final.
  if (slot >= stage_slot_samples_.size()) {
    return;
  }
  std::vector<StageSample>& samples = stage_slot_samples_[slot];
  // The phase now running (a sample is at most two works old: the phase it was drawn in, but at a switch).
  const size_t phase = options_.ab_phases && ab_.started() ? size_t(ab_.phase()) : size_t(AbPhase::kCount);
  for (uint32_t i = 0; i < samples.size(); ++i) {
    StageSample& s = samples[i];
    for (uint32_t c = 0; c < kStageCounters; ++c) {
      s.value[c] = StageReportValue(core_.QueriesCpu() + stage_layout_.Offset(slot, i, c));
    }
    stage_phase_[phase].Add(s);
    stage_totals_.Add(s);
  }
  samples.clear();
}

void DkRecorder::LogStageCounters(const char* what, const StageReport& r) {
  for (uint32_t c = 0; c < kStageClasses; ++c) {
    const StageTotals& t = r.totals(StageClass(c));
    REXLOG_INFO("deko3d: run 62 stage counters, {}, {}: {} draws sampled ({} short at assembly), triangles expected "
                "{}; input primitives {} ({:.3f} of expected), clipper input {} ({:.3f} of input), clipper output {} "
                "({:.3f} of clipper input), fragment invocations {} ({:.1f} a triangle out), samples passed {} ({:.3f} a "
                "fragment)",
                what, StageClassName(StageClass(c)), t.draws, t.assembly_short, t.expected, t.value[0], t.assembled(),
                t.value[1], t.reached_clipper(), t.value[2], t.left_clipper(), t.value[3], t.fragments_per_triangle(),
                t.value[4], t.samples_per_fragment());
  }
  REXLOG_INFO("deko3d: run 62 stage counters, {}: verdict: {}", what,
              StageReport::Verdict(r.totals(StageClass::kSkinnedGbuffer), r.totals(StageClass::kStaticGbuffer)));
}

void DkRecorder::HarvestStamps(uint32_t slot) {
  if (slot >= slot_stamps_.size()) {
    return;
  }
  std::vector<Stamp>& stamps = slot_stamps_[slot];
  if (stamps.size() < 2) {
    stamps.clear();
    return;
  }
  const uint8_t* base = core_.QueriesCpu() + size_t(slot) * kStampsPerSlot * 16;
  std::vector<uint64_t> t(stamps.size());
  for (size_t i = 0; i < stamps.size(); ++i) {
    uint64_t q[2];
    std::memcpy(q, base + i * 16, sizeof(q));
    // The report is a value and a timestamp; the timestamp is read from the second word, the first when that is empty.
    t[i] = dkTimestampToNs(q[1] ? q[1] : q[0]);
    if (gpu_.raw_logged < 4 && i < 2) {
      ++gpu_.raw_logged;
      REXLOG_INFO("deko3d: GPU report {} of a work: words {:016X} {:016X} -> {} ns", i, q[0], q[1], t[i]);
    }
  }
  const uint64_t work_ns = t.back() >= t.front() ? t.back() - t.front() : 0;
  if (gpu_.have_last && t.front() > gpu_.last_end_ns) {
    gpu_.gap_ns += t.front() - gpu_.last_end_ns;
  }
  gpu_.last_end_ns = t.back();
  gpu_.have_last = true;
  gpu_.busy_ns += work_ns;
  gpu_.max_work_ns = std::max(gpu_.max_work_ns, work_ns);
  ++gpu_.works;
  for (size_t i = 0; i + 1 < stamps.size(); ++i) {
    if (t[i + 1] < t[i]) {
      continue;
    }
    PassLabel& l = labels_[stamps[i].label];
    l.ns += t[i + 1] - t[i];
    ++l.passes;
    l.draws += stamps[i].draws;
    if (t[i + 1] - t[i] > l.max_ns) {
      l.max_ns = t[i + 1] - t[i];
      l.max_draws = stamps[i].draws;
      l.max_vs = stamps[i].vertex_shader;
      l.max_ps = stamps[i].pixel_shader;
    }
  }
  // A work that took long: the passes that took most of it, once the report has said how many there were.
  if (work_ns > 250'000'000ull && gpu_.slow_logged < 12) {
    ++gpu_.slow_logged;
    std::vector<size_t> order;
    for (size_t i = 0; i + 1 < stamps.size(); ++i) {
      order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return (t[a + 1] > t[a] ? t[a + 1] - t[a] : 0) > (t[b + 1] > t[b] ? t[b + 1] - t[b] : 0);
    });
    std::string top;
    for (size_t k = 0; k < order.size() && k < 4; ++k) {
      const size_t i = order[k];
      top += fmt::format("{}{} {:.1f} ms ({} draws", k ? "; " : "", labels_[stamps[i].label].text,
                         double(t[i + 1] > t[i] ? t[i + 1] - t[i] : 0) / 1e6, stamps[i].draws);
      if (stamps[i].draws) {
        top += fmt::format(", first: vertex shader {:016X} pixel shader {:016X}", stamps[i].vertex_shader,
                           stamps[i].pixel_shader);
      }
      top += ")";
    }
    REXLOG_INFO("deko3d: slow work: the GPU took {:.0f} ms over {} passes; the longest: {}", double(work_ns) / 1e6,
                stamps.size() - 1, top);
  }
  stamps.clear();
}

void DkRecorder::BeginWorkState() {
  DkCmdBuf cmd = core_.commands();
  transfer_since_barrier_ = false;
  work3d_since_barrier_ = true;
  // What an earlier submission sampled or wrote is done before this one's draws; the caches forget what the CPU
  // rewrote in this slot's memory.
  dkCmdBufBarrier(cmd, DkBarrier_Fragments,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
  dkCmdBufTiledCacheOp(cmd, DkTiledCacheOp_Disable);
  core_.BindDescriptors();
  static const DkViewportSwizzle kIdentity = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                               DkSwizzle_PositiveW};
  dkCmdBufSetViewportSwizzles(cmd, 0, &kIdentity, 1);
  ForgetState();
}

void DkRecorder::CloseWork() {
  if (!core_.WorkOpen()) {
    return;
  }
  // The fence is released by the 3D engine: a copy engine transfer still running is waited for first.
  if (transfer_since_barrier_) {
    dkCmdBufBarrier(core_.commands(), DkBarrier_Full, DkInvalidateFlags_Image);
    ++barriers_;
    transfer_since_barrier_ = false;
  }
  StampPass(5, 0, 0, 0, 0);
  core_.EndWork();
}

void DkRecorder::SwitchSlot() {
  ++slot_switches_;
  CloseWork();
  EnsureWork();
}

void DkRecorder::ForgetState() {
  e_ = Emitted{};
  shared_work_ = UINT64_MAX;
}

void DkRecorder::BeforeTransfer() {
  if (work3d_since_barrier_) {
    dkCmdBufBarrier(core_.commands(), DkBarrier_Full, DkInvalidateFlags_Image);
    ++barriers_;
    work3d_since_barrier_ = false;
  }
  transfer_since_barrier_ = true;
}

void DkRecorder::Before3D() {
  if (transfer_since_barrier_) {
    dkCmdBufBarrier(core_.commands(), DkBarrier_Full, DkInvalidateFlags_Image);
    ++barriers_;
    transfer_since_barrier_ = false;
  }
  work3d_since_barrier_ = true;
}

Upload DkRecorder::Reserve(uint32_t bytes, uint32_t alignment) {
  EnsureWork();
  Upload u = core_.Reserve(bytes, alignment);
  if (!u && bytes + alignment <= (core_.config().upload_mb << 20)) {
    SwitchSlot();
    u = core_.Reserve(bytes, alignment);
  }
  return u;
}

// ---- Images

bool DkRecorder::CreateImage(DkImageEntry& image, DkImageType type, DkImageFormat format, uint32_t width,
                             uint32_t height, uint32_t layers, uint32_t levels, uint32_t flags, const char* what) {
  if (!width || !height || width > 16384 || height > 16384 || format == DkImageFormat_None) {
    REXLOG_WARN("deko3d: {} of {}x{} (format {}): impossible", what, width, height, int(format));
    return false;
  }
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, core_.device());
  m.type = type;
  m.flags = flags;
  m.format = format;
  m.dimensions[0] = width;
  m.dimensions[1] = height;
  m.dimensions[2] = type == DkImageType_Cubemap ? 6 : (type == DkImageType_3D || type == DkImageType_2DArray ? layers : 1);
  m.mipLevels = std::max(levels, 1u);
  const bool compressed = format >= DkImageFormat_RGB_BC1 && format <= DkImageFormat_RG_BC5_Snorm;
  FixTileSize(m, compressed ? (height + 3) / 4 : height);
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &m);
  const uint64_t bytes = dkImageLayoutGetSize(&layout);
  if (bytes >= (1ull << 31)) {
    return false;
  }
  image.memory = core_.ReserveImage(uint32_t(bytes), dkImageLayoutGetAlignment(&layout));
  if (!image.memory) {
    return false;
  }
  dkImageInitialize(&image.image, &layout, image.memory.block, image.memory.offset);
  image.type = type;
  image.format = format;
  image.width = width;
  image.height = height;
  image.layers = type == DkImageType_Cubemap ? 6 : layers;
  image.levels = m.mipLevels;
  image.depth = format == DkImageFormat_Z24S8 || format == DkImageFormat_ZF32_X24S8;
  image.live = true;
  image.serial = ++serial_;
  return true;
}

void DkRecorder::DestroyImage(DkImageEntry& image) {
  if (!image.live) {
    return;
  }
  ReleaseDescriptorsOf(image.serial);
  core_.ReleaseImageLater(image.memory);
  image = DkImageEntry{};
}

DkImageEntry* DkRecorder::Surface(const SurfaceDescriptor& d, bool depth) {
  if (!d.handle || !d.width || !d.height) {
    return nullptr;
  }
  const DkImageFormat format = SurfaceFormat(d.format, depth);
  if (format == DkImageFormat_None) {
    static std::unordered_set<uint32_t> logged;
    if (logged.insert(d.format).second) {
      REXLOG_WARN("deko3d: render target format {:08X} not supported yet (surface {:08X} {}x{})", d.format, d.handle,
                  d.width, d.height);
    }
    return nullptr;
  }
  const SurfaceKey key{d.handle, d.format, d.width, d.height, depth};
  if (auto it = surfaces_.find(key); it != surfaces_.end()) {
    return &it->second;
  }
  DkImageEntry image;
  const uint32_t flags = depth ? DkImageFlags_UsageRender : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
  if (!CreateImage(image, DkImageType_2D, format, d.width, d.height, 1, 1, flags,
                   depth ? "depth target" : "render target")) {
    return nullptr;
  }
  REXLOG_INFO("deko3d: {} {:08X}: {}x{}, guest format {:08X}, host format {}", depth ? "depth target" : "render target",
              d.handle, d.width, d.height, d.format, int(format));
  DkImageEntry& created = surfaces_.emplace(key, image).first->second;
  // A new target starts as the Vulkan path's: colour 0, depth 1 and stencil 0.
  EnsureWork();
  Before3D();
  DkCmdBuf cmd = core_.commands();
  DkImageView view;
  dkImageViewDefaults(&view, &created.image);
  const DkScissor all = {0, 0, created.width, created.height};
  if (depth) {
    dkCmdBufBindRenderTargets(cmd, nullptr, 0, &view);
    dkCmdBufSetScissors(cmd, 0, &all, 1);
    dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
  } else {
    dkCmdBufBindRenderTarget(cmd, &view, nullptr);
    dkCmdBufSetScissors(cmd, 0, &all, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
  }
  ForgetState();
  return &created;
}

bool DkRecorder::BindTargets(const std::array<SurfaceDescriptor, kRenderTargetCount>& colors,
                             const SurfaceDescriptor& depth, uint32_t color_mask, bool use_depth, uint32_t& width,
                             uint32_t& height) {
  std::array<uint32_t, kRenderTargetCount + 1> key{};
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    key[i] = (color_mask & (1u << i)) ? colors[i].handle : 0;
  }
  key[kRenderTargetCount] = use_depth ? depth.handle : 0;
  if (e_.targets_valid && e_.targets == key) {
    width = e_.width;
    height = e_.height;
    return width && height;
  }
  // The images first: creating one records its first clear and forgets the emitted state.
  std::array<DkImageEntry*, kRenderTargetCount> images{};
  DkImageEntry* depth_image = nullptr;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (key[i]) {
      images[i] = Surface(colors[i], false);
    }
  }
  if (key[kRenderTargetCount]) {
    depth_image = Surface(depth, true);
  }
  // Run 57: a colour target that shares another surface's EDRAM placement starts with that content (unless the title
  // is about to clear all of it), and every bound target becomes its placement's owner.
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (images[i]) {
      if (!full_clear_bind_) {
        PrepareSurfaceContent(colors[i], *images[i], false);
      }
      ClaimPlacement(colors[i], *images[i], false);
    }
  }
  if (depth_image) {
    ClaimPlacement(depth, *depth_image, true);
  }
  Before3D();
  {
    // A new pass for the GPU-time labels: its first colour target (or the depth), its depth, its size.
    uint32_t first = key[kRenderTargetCount];
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      if (key[i]) {
        first = key[i];
        break;
      }
    }
    const uint32_t w = images[0] ? images[0]->width : depth_image ? depth_image->width : 0;
    const uint32_t h = images[0] ? images[0]->height : depth_image ? depth_image->height : 0;
    StampPass(1, first, key[kRenderTargetCount], w, h);
  }
  DkImageView views[kRenderTargetCount];
  const DkImageView* bound[kRenderTargetCount] = {};
  uint32_t count = 0;
  width = UINT32_MAX;
  height = UINT32_MAX;
  uint32_t widest = 0, tallest = 0;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (!images[i]) {
      continue;
    }
    widest = std::max(widest, images[i]->width);
    tallest = std::max(tallest, images[i]->height);
    // Target ids are the shader's output locations: a gap is bound to the next present image (logged once).
    if (count != i) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_WARN("deko3d: colour targets with a gap (target {} bound at {}): outputs may land elsewhere", i,
                    count);
      }
    }
    dkImageViewDefaults(&views[count], &images[i]->image);
    bound[count] = &views[count];
    ++count;
    width = std::min(width, images[i]->width);
    height = std::min(height, images[i]->height);
  }
  DkImageView depth_view;
  if (depth_image) {
    dkImageViewDefaults(&depth_view, &depth_image->image);
    width = std::min(width, depth_image->width);
    height = std::min(height, depth_image->height);
  }
  if (!count && !depth_image) {
    e_.targets_valid = false;
    width = height = 0;
    return false;
  }
  if (depth_image) {
    widest = std::max(widest, depth_image->width);
    tallest = std::max(tallest, depth_image->height);
  }
  if (widest != width || tallest != height) {
    ++gaps_.extent_mismatch;
  }
  dkCmdBufBindRenderTargets(core_.commands(), bound, count, depth_image ? &depth_view : nullptr);
  ++target_binds_;
  e_.valid = false;  // the scissor and viewport of the new targets are emitted again
  e_.targets = key;
  e_.targets_valid = true;
  e_.width = width;
  e_.height = height;
  return true;
}

DkImageEntry* DkRecorder::ResolvedTexture(uint32_t handle, uint32_t base, bool* by_base, uint32_t* object) {
  if (by_base) {
    *by_base = false;
  }
  if (auto it = resolved_.find(handle); handle && it != resolved_.end() && it->second.written) {
    if (object) {
      *object = handle;
    }
    return &it->second.image;
  }
  if (auto b = resolved_by_base_.find(base & 0x1FFFF000u); base && b != resolved_by_base_.end()) {
    if (auto it = resolved_.find(b->second); it != resolved_.end() && it->second.written) {
      if (by_base) {
        *by_base = true;
      }
      if (object) {
        *object = b->second;
      }
      return &it->second.image;
    }
  }
  return nullptr;
}

// ---- The packed depth alias (dk_depth_alias.h)

DkImageEntry* DkRecorder::PackedDepthAlias(uint32_t object) {
  const auto source_it = resolved_.find(object);
  if (!alias_valid_ || source_it == resolved_.end() || !source_it->second.written || !source_it->second.image.depth) {
    ++alias_stats_.failed;
    return nullptr;
  }
  Resolved& source = source_it->second;
  DepthAlias& alias = aliases_[object];
  const uint32_t width = source.image.width, height = source.image.height;
  if (alias.image.live && (alias.image.width != width || alias.image.height != height)) {
    DestroyImage(alias.image);
    alias.source_writes = UINT64_MAX;
  }
  if (!alias.image.live) {
    if (!CreateImage(alias.image, DkImageType_2D, DkImageFormat_RGBA8_Unorm, width, height, 1, 1,
                     DkImageFlags_UsageRender, "packed depth alias")) {
      aliases_.erase(object);
      ++alias_stats_.failed;
      return nullptr;
    }
    REXLOG_INFO("deko3d: packed depth alias of resolved depth {:08X}: {}x{}, guest format {} ({} aliases)", object,
                width, height, source.guest_format, aliases_.size());
  }
  if (alias.source_serial == source.image.serial && alias.source_writes == source.writes) {
    ++alias_stats_.reused;
    return &alias.image;
  }

  // The views and the constants first (an upload may move on to the next work slot), then the pass.
  const uint32_t depth_view = ViewOf(source.image, 0x688, kSwizzleRRRR);
  const uint32_t stencil_view = ViewOf(source.image, 0x688, kSwizzleRRRR, true);
  const uint32_t data[4] = {source.guest_format == kFetchFormat24_8Float ? 1u : 0u, 0, 0, 0};
  const Upload ubo = Reserve(sizeof(data), DK_UNIFORM_BUF_ALIGNMENT);
  if (!depth_view || !stencil_view || !ubo) {
    ++alias_stats_.failed;
    return nullptr;
  }
  std::memcpy(ubo.cpu, data, sizeof(data));
  Before3D();  // the resolve's copy ends before the depth is sampled
  DkCmdBuf cmd = core_.commands();
  DkImageView target;
  dkImageViewDefaults(&target, &alias.image.image);
  dkCmdBufBindRenderTarget(cmd, &target, nullptr);
  const DkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  const DkScissor all = {0, 0, width, height};
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  dkCmdBufSetScissors(cmd, 0, &all, 1);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cw);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* pair[2] = {&alias_vs_, &alias_ps_};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, sizeof(data));
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(depth_view, 0));
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, dkMakeTextureHandle(stencil_view, 0));
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);  // written before any draw samples it
  ++barriers_;
  ForgetState();  // the targets, shaders and state are the draw's to emit again
  alias.source_serial = source.image.serial;
  alias.source_writes = source.writes;
  if (++alias_stats_.converted <= 4) {
    REXLOG_INFO("deko3d: packed depth alias {:08X} converted ({}x{}, depth format {}, {} resolves so far)", object, width,
                height, source.guest_format, source.writes);
  }
  return &alias.image;
}

// ---- The scene depth hand-off (dk_depth_handoff.h)

void DkRecorder::DepthHandoff(const RecorderDepthHandoff& h) {
  geometry_.Forget();  // run 53: a new epoch for the geometry store (the title may have waited for the GPU)
  ++handoff_.commands;
  EnsureWork();
  const auto reject = [&](HandoffVerdict verdict, const char* detail) {
    ++handoff_.rejected[uint32_t(verdict) & 7];
    if (handoff_.logged++ < 8) {
      REXLOG_WARN("deko3d: depth hand-off of {:08X} into {:08X} not done: {} (verdict {})", h.source_texture,
                  h.destination.handle, detail, uint32_t(verdict));
    }
  };
  HandoffInputs in;
  in.policy = h.stencil_policy;
  in.pass_available = handoff_valid_;
  in.source_surface = h.source.handle != 0;
  DkImageEntry* destination = h.destination.handle ? Surface(h.destination, true) : nullptr;
  in.destination_surface = destination != nullptr;
  const auto source_it = resolved_.find(h.source_texture);
  Resolved* source = source_it != resolved_.end() ? &source_it->second : nullptr;
  in.source_written = source && source->written && source->image.depth;
  if (source) {
    in.source_width = source->image.width;
    in.source_height = source->image.height;
    in.source_guest_format = source->guest_format;
  }
  if (destination) {
    in.destination_width = destination->width;
    in.destination_height = destination->height;
  }
  in.same_image = false;  // a resolved texture and a surface are never the same image here (the Vulkan path compares VkImages)
  const HandoffVerdict verdict = PlanDepthHandoff(in);
  if (verdict != HandoffVerdict::kDone) {
    reject(verdict, verdict == HandoffVerdict::kSourceUnwritten ? "the resolved depth was not written"
                    : verdict == HandoffVerdict::kExtent        ? "the sizes differ"
                    : verdict == HandoffVerdict::kNoSurface     ? "a surface could not be made"
                                                                : "see the verdict");
    if (handoff_.logged <= 8) {
      REXLOG_WARN("deko3d: depth hand-off rejected: source {}x{} guest format {}, destination {}x{}, source surface {:08X}",
                  in.source_width, in.source_height, in.source_guest_format, in.destination_width, in.destination_height,
                  h.source.handle);
    }
    return;
  }
  handoff_destination_ = h.destination.handle;

  // The views and the constants first (an upload may move on to the next work slot), then the pass.
  const uint32_t depth_view = ViewOf(source->image, 0x688, kSwizzleRRRR);
  const uint32_t data[4] = {source->guest_format == kFetchFormat24_8Float ? 1u : 0u, 0, 0, 0};
  const Upload ubo = Reserve(sizeof(data), DK_UNIFORM_BUF_ALIGNMENT);
  if (!depth_view || !ubo) {
    reject(HandoffVerdict::kNoSurface, "no view or constants for the source");
    return;
  }
  std::memcpy(ubo.cpu, data, sizeof(data));
  const uint32_t width = destination->width, height = destination->height;
  StampPass(4, h.source_texture, h.destination.handle, width, height);
  Before3D();  // the resolve's copy ends before the depth is sampled
  DkCmdBuf cmd = core_.commands();
  DkImageView target;
  dkImageViewDefaults(&target, &destination->image);
  dkCmdBufBindRenderTargets(cmd, nullptr, 0, &target);
  const DkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  const DkScissor all = {0, 0, width, height};
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  dkCmdBufSetScissors(cmd, 0, &all, 1);
  // What an earlier draw wrote into the surface is finished before it is cleared (the Vulkan path has a barrier here).
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
  // The destination starts as the Vulkan path's load ops: depth 0 and the "no scene" stencil, the covered pixels replace it.
  dkCmdBufClearDepthStencil(cmd, true, 0.0f, 0xFF, kForwardEmptySceneStencil);
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cw;
  dkColorWriteStateDefaults(&cw);
  DkDepthStencilState ds = Zeroed<DkDepthStencilState>();
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = true;
  ds.depthWriteEnable = true;
  ds.depthCompareOp = DkCompareOp_Always;
  ds.stencilTestEnable = true;
  ds.stencilFrontFailOp = ds.stencilBackFailOp = DkStencilOp_Keep;
  ds.stencilFrontPassOp = ds.stencilBackPassOp = DkStencilOp_Replace;
  ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = DkStencilOp_Keep;
  ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;
  dkCmdBufBindRasterizerState(cmd, &rs);
  dkCmdBufBindColorState(cmd, &cs);
  dkCmdBufBindColorWriteState(cmd, &cw);
  dkCmdBufBindDepthStencilState(cmd, &ds);
  dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, 0xFF, uint8_t(kForwardCoveredSceneStencil), 0xFF);
  dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
  const DkShader* pair[2] = {&alias_vs_, &handoff_ps_};
  dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, sizeof(data));
  dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(depth_view, 0));
  dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);  // written before anything samples or tests it
  ++barriers_;
  ForgetState();  // the targets, shaders and state are the draw's to emit again
  // The hardware now holds stencil write mask, reference and compare mask 0xFF: no packed value a draw sends equals this,
  // so the next stencil draw always sends its own.
  e_.stencil[0] = e_.stencil[1] = 0xFFFFFFFFu;
  if (++handoff_.done <= 3) {
    REXLOG_INFO("deko3d: depth hand-off {:08X} (guest format {}, {}x{}) from the surface {:08X} (sample type {}) into the depth "
                "surface {:08X} (sample type {}): cleared to depth 0 and stencil {:#x}, covered pixels written with stencil {:#x}",
                h.source_texture, source->guest_format, width, height, h.source.handle, h.source.sample_type,
                h.destination.handle, h.destination.sample_type, kForwardEmptySceneStencil, kForwardCoveredSceneStencil);
  }
}

// ---- Clears

void DkRecorder::Clear(const RecorderClear& c) {
  geometry_.Forget();  // run 53: a new epoch for the geometry store (the title may have waited for the GPU)
  ++clears_;
  EnsureWork();
  if ((c.flags & 0x30) && clears_logged_.size() < 24 &&
      clears_logged_.insert((uint64_t(c.depth_stencil.handle) << 32) ^ (uint64_t(c.flags) << 8) ^ c.stencil ^
                            (c.depth_bits * 0x9E3779B97F4A7C15ull))
          .second) {
    REXLOG_INFO("deko3d: title clear of depth/stencil {:08X} ({}x{}): flags {:X} depth {} stencil {}, rectangle {},{} {},{}",
                c.depth_stencil.handle, c.depth_stencil.width, c.depth_stencil.height, c.flags,
                std::bit_cast<double>(c.depth_bits), c.stencil, c.left, c.top, c.right, c.bottom);
  }
  const uint32_t colors = c.flags & 0xF;
  const bool depth = (c.flags & 0x30) != 0;
  uint32_t width = 0, height = 0;
  // Run 57: a clear of every pixel of each colour target needs no EDRAM placement content first (the Vulkan path's
  // gta4_native_clear_skips_materialization).
  bool full = true;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if ((colors & (1u << i)) && c.render_targets[i].handle) {
      full = full && c.left <= 0 && c.top <= 0 && c.right >= int32_t(c.render_targets[i].width) &&
             c.bottom >= int32_t(c.render_targets[i].height);
    }
  }
  full_clear_bind_ = full;
  const bool bound = BindTargets(c.render_targets, c.depth_stencil, colors, depth, width, height);
  full_clear_bind_ = false;
  if (!bound) {
    return;
  }
  const int32_t left = std::clamp(c.left, 0, int32_t(width));
  const int32_t top = std::clamp(c.top, 0, int32_t(height));
  const int32_t right = std::clamp(c.right, left, int32_t(width));
  const int32_t bottom = std::clamp(c.bottom, top, int32_t(height));
  if (right <= left || bottom <= top) {
    return;
  }
  DkCmdBuf cmd = core_.commands();
  const DkScissor scissor = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
  dkCmdBufSetScissors(cmd, 0, &scissor, 1);  // clears respect the scissor
  uint32_t index = 0;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (!(colors & (1u << i)) || !c.render_targets[i].handle) {
      continue;
    }
    dkCmdBufClearColorFloat(cmd, index++, DkColorMask_RGBA, std::bit_cast<float>(c.color_bits[0]),
                            std::bit_cast<float>(c.color_bits[1]), std::bit_cast<float>(c.color_bits[2]),
                            std::bit_cast<float>(c.color_bits[3]));
  }
  if (depth && c.depth_stencil.handle) {
    dkCmdBufClearDepthStencil(cmd, (c.flags & 0x10) != 0, float(std::bit_cast<double>(c.depth_bits)),
                              (c.flags & 0x20) ? 0xFF : 0, uint8_t(c.stencil));
  }
  e_.valid = false;  // the scissor is the clear's
}

// ---- Resolves (copies; B2)

void DkRecorder::Resolve(const RecorderResolve& r) {
  geometry_.Forget();  // run 53: a new epoch for the geometry store (the title may have waited for the GPU)
  ++resolves_;
  const ResolveCommand& c = r.command;
  EnsureWork();
  const bool depth = (c.flags & 7) == 4;
  DkImageEntry* source = Surface(c.source, depth);
  if (!source) {
    return;
  }
  // Run 57: a surface the title resolves without ever drawing into it (run 56: GTA IV's whole luminance chain) holds
  // what another surface of its EDRAM placement wrote.
  if (!depth) {
    PrepareSurfaceContent(c.source, *source, false);
    GuestSurfaceView view;
    if (options_.placement && !source->placement_serial && DecodeGuestSurfaceView(c.source, false, view) &&
        placement_logged_.size() < 48 && placement_logged_.insert(0xFFFF0000ull << 32 | c.source.handle).second) {
      // Never drawn into and no owner of its placement: the owners at the same EDRAM base say why (a near miss).
      std::string near;
      for (const auto& [k, o] : placement_) {
        if (k.placement_base_tiles == view.placement_base_tiles && !k.depth) {
          near += fmt::format(" {:08X}(pitch {} samples {}x{} msaa {})", o.surface.handle, k.sample_pitch, k.sample_width,
                              k.sample_height, uint32_t(o.view.msaa_samples));
        }
      }
      REXLOG_INFO("deko3d: run 57 resolve of {:08X} ({}x{}) that nothing drew into: EDRAM base {} tiles, pitch {} samples "
                  "{}x{} msaa {}; owners at that base:{}",
                  c.source.handle, source->width, source->height, view.placement_base_tiles, view.sample_pitch,
                  view.sample_width, view.sample_height, uint32_t(view.msaa_samples), near.empty() ? " none" : near);
    }
  }
  const std::array<uint32_t, 6> f = {c.destination_fetch[0], c.destination_fetch[1], c.destination_fetch[2],
                                     c.destination_fetch[3], c.destination_fetch[4], c.destination_fetch[5]};
  const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
  const uint32_t width = (f[2] & 0x1FFF) + 1;
  const uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t guest_format = f[1] & 0x3F;
  const DkImageFormat format = ResolveFormat(guest_format, source->format);
  Resolved& destination = resolved_[c.destination_texture];
  if (!destination.image.live || destination.image.width != width || destination.image.height != height ||
      destination.image.format != format) {
    DestroyImage(destination.image);
    destination.written = false;  // a new image holds nothing yet (a hand-off or an alias must not read it as written)
    destination.writes = 0;
    const bool dest_depth = format == DkImageFormat_ZF32_X24S8;
    const uint32_t flags =
        dest_depth ? DkImageFlags_UsageRender : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    if (!CreateImage(destination.image, DkImageType_2D, format, width, height, 1, 1, flags, "resolved texture")) {
      resolved_.erase(c.destination_texture);
      return;
    }
    REXLOG_INFO("deko3d: resolved texture {:08X} at {:08X}: {}x{}, guest format {}, host format {}",
                c.destination_texture, base, width, height, guest_format, int(format));
    ++texture_generation_;
  }
  destination.base = base;
  destination.guest_format = guest_format;
  destination.fetch = f;
  resolved_by_base_[base] = c.destination_texture;

  // The rectangle: the source rectangle (the whole surface without one), at the destination point.
  int32_t x0 = 0, y0 = 0, x1 = int32_t(source->width), y1 = int32_t(source->height);
  if (c.source_rectangle_valid) {
    x0 = std::clamp(c.source_rectangle.left, 0, int32_t(source->width));
    y0 = std::clamp(c.source_rectangle.top, 0, int32_t(source->height));
    x1 = std::clamp(c.source_rectangle.right, x0, int32_t(source->width));
    y1 = std::clamp(c.source_rectangle.bottom, y0, int32_t(source->height));
  }
  const int32_t dx = c.destination_point_valid ? std::max(c.destination_point.x, 0) : 0;
  const int32_t dy = c.destination_point_valid ? std::max(c.destination_point.y, 0) : 0;
  const uint32_t w = uint32_t(std::min(x1 - x0, int32_t(width) - dx));
  const uint32_t h = uint32_t(std::min(y1 - y0, int32_t(height) - dy));
  if (x1 > x0 && y1 > y0 && dx < int32_t(width) && dy < int32_t(height) && w && h) {
    StampPass(2, c.source.handle, c.destination_texture, w, h);
    BeforeTransfer();
    DkImageView from, to;
    dkImageViewDefaults(&from, &source->image);
    dkImageViewDefaults(&to, &destination.image.image);
    const DkImageRect a = {uint32_t(x0), uint32_t(y0), 0, w, h, 1};
    const DkImageRect b = {uint32_t(dx), uint32_t(dy), 0, w, h, 1};
    if (source->format == destination.image.format) {
      dkCmdBufCopyImage(core_.commands(), &from, &a, &to, &b, 0);
    } else if (!source->depth && !destination.image.depth) {
      dkCmdBufBlitImage(core_.commands(), &from, &a, &to, &b, DkBlitFlag_FilterNearest, 0);
    } else {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_WARN("deko3d: resolve between depth and colour formats ({} -> {}) not done yet", int(source->format),
                    int(destination.image.format));
      }
    }
    destination.written = true;
    ++destination.writes;
    ++resolves_done_;
    e_.targets_valid = false;  // the next draw binds its targets again: the GPU-time label of the copy ends here
    if (anatomy_frame_) {
      AnatomyCapture(destination.image, c.source.handle, c.destination_texture, depth ? "depth" : "colour");
    }
  }

  // The clears that come with the resolve (bits 8 and 9), on the source surface.
  const bool clear_color = (c.flags & (1u << 8)) != 0;
  const bool clear_depth = (c.flags & (1u << 9)) != 0;
  if (!depth && (int32_t(c.flags) >> 26) != 0) {
    ++gaps_.resolve_exponent;
    // Which exponents the title uses, into which formats (a few lines, once each): the value the run needs to say
    // whether the brightness of the HDR resolves is a candidate.
    const int32_t exponent = int32_t(c.flags) >> 26;
    if (exponents_logged_.size() < 16 && exponents_logged_.insert(uint32_t(exponent & 0xFF) << 8 | guest_format).second) {
      REXLOG_INFO("deko3d: resolve into {:08X} (guest format {}, host format {}) with colour exponent {} (flags {:08X}): "
                  "not applied",
                  c.destination_texture, guest_format, int(format), exponent, c.flags);
    }
  }
  const ResolveClearPlan plan = PlanResolveClears(c.flags, c.parameters_valid != 0);
  // A colour resolve with bit 9 clears the device's depth/stencil, which is not the resolved surface (the title's end of
  // the deferred phase, flags 0x300): the Vulkan path does it (RecordResolveClears); this recorder counted it as "the
  // other aspect" 467 times in run 40 and did not. What stays unrepresentable (bit 8 on a depth resolve, or a clear
  // with a format override) is still only counted.
  gaps_.resolve_cross_clear += plan.unrepresentable_color || (clear_depth && !depth && !plan.clear_device_depth);
  if (plan.clear_device_depth) {
    DkImageEntry* target = Surface(r.depth_stencil, true);
    if (!target) {
      ++resolve_clears_.depth_missing;
    } else {
      // The resolve's rectangle, on the depth surface (the whole surface when the resolve names none).
      int32_t cx0 = 0, cy0 = 0, cx1 = int32_t(target->width), cy1 = int32_t(target->height);
      if (c.source_rectangle_valid) {
        cx0 = std::clamp(c.source_rectangle.left, 0, int32_t(target->width));
        cy0 = std::clamp(c.source_rectangle.top, 0, int32_t(target->height));
        cx1 = std::clamp(c.source_rectangle.right, cx0, int32_t(target->width));
        cy1 = std::clamp(c.source_rectangle.bottom, cy0, int32_t(target->height));
      }
      if (cx1 > cx0 && cy1 > cy0) {
        Before3D();
        DkCmdBuf cmd = core_.commands();
        DkImageView view;
        dkImageViewDefaults(&view, &target->image);
        dkCmdBufBindRenderTargets(cmd, nullptr, 0, &view);
        const DkScissor rect = {uint32_t(cx0), uint32_t(cy0), uint32_t(cx1 - cx0), uint32_t(cy1 - cy0)};
        dkCmdBufSetScissors(cmd, 0, &rect, 1);
        const float depth_value = float(std::bit_cast<double>(c.clear_depth_bits));
        dkCmdBufClearDepthStencil(cmd, true, depth_value, 0xFF, uint8_t(c.clear_stencil));
        ForgetState();
        if (++resolve_clears_.depth_done <= 6) {
          REXLOG_INFO("deko3d: resolve {:08X} flags {:08X} clears the device's depth/stencil {:08X} ({}x{}) to depth {} "
                      "stencil {}, rectangle {},{} {}x{}",
                      c.destination_texture, c.flags, r.depth_stencil.handle, target->width, target->height, depth_value,
                      c.clear_stencil & 0xFF, cx0, cy0, cx1 - cx0, cy1 - cy0);
        }
      }
    }
  } else if (clear_depth && !depth && c.parameters_valid) {
    ++resolve_clears_.override_skipped;
  }
  if (clear_color || clear_depth) {
    Before3D();
    DkCmdBuf cmd = core_.commands();
    DkImageView view;
    dkImageViewDefaults(&view, &source->image);
    if (depth) {
      dkCmdBufBindRenderTargets(cmd, nullptr, 0, &view);
    } else {
      dkCmdBufBindRenderTarget(cmd, &view, nullptr);
    }
    const DkScissor all = {0, 0, source->width, source->height};
    dkCmdBufSetScissors(cmd, 0, &all, 1);
    if (depth && clear_depth) {
      dkCmdBufClearDepthStencil(cmd, true, float(std::bit_cast<double>(c.clear_depth_bits)), 0xFF,
                                uint8_t(c.clear_stencil));
    } else if (!depth && clear_color) {
      dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, std::bit_cast<float>(c.clear_color_bits[0]),
                              std::bit_cast<float>(c.clear_color_bits[1]), std::bit_cast<float>(c.clear_color_bits[2]),
                              std::bit_cast<float>(c.clear_color_bits[3]));
    }
    ForgetState();
  }
}

// ---- Resources

void DkRecorder::ResourceEvent(const RecorderResourceEvent& event) {
  geometry_.Forget();  // run 53: a new epoch for the geometry store (the title may have waited for the GPU)
  if (event.packed_depth_source) {
    if (alias_sources_.size() < 256) {
      const auto [it, added] = alias_sources_.emplace(event.handle, event.packed_depth_source);
      it->second = event.packed_depth_source;
      alias_stats_.registered += added;
      if (added && alias_stats_.registered <= 8) {
        REXLOG_INFO("deko3d: packed depth alias {:08X} registered by the title: A8R8G8B8 view of the resolved depth "
                    "{:08X}",
                    event.handle, event.packed_depth_source);
      }
    }
    return;
  }
  if (event.released) {
    alias_sources_.erase(event.handle);
  }
  if (!event.released) {
    dirty_handles_.insert(event.handle);
    return;
  }
  dirty_handles_.erase(event.handle);
  if (auto it = resolved_.find(event.handle); it != resolved_.end()) {
    EnsureWork();
    if (auto b = resolved_by_base_.find(it->second.base); b != resolved_by_base_.end() && b->second == event.handle) {
      resolved_by_base_.erase(b);
    }
    DestroyImage(it->second.image);
    resolved_.erase(it);
    if (auto a = aliases_.find(event.handle); a != aliases_.end()) {
      DestroyImage(a->second.image);
      aliases_.erase(a);
    }
    ++texture_generation_;
  }
}

// ---- Run 54: the title's texture locks (the Vulkan path's ReadbackTextureToGuest on deko3d)

void DkRecorder::TextureLock(const RecorderTextureLock& l) {
  geometry_.Forget();
  ++lock_stats_.locks;
  const uint64_t start_ns = NowNs();
  TextureLockResult result{};
  auto complete = [&](bool ok) {
    lock_stats_.ns += NowNs() - start_ns;
    if (l.wait) {
      l.wait->Complete(ok, result);
    }
  };
  const TextureLockCommand& c = l.command;
  const auto it = resolved_.find(c.texture);
  if (it == resolved_.end() || !it->second.written || !it->second.image.live) {
    // Not produced by the GPU (or never written yet): the guest copy is the texture, as on the Vulkan path.
    ++lock_stats_.not_gpu;
    complete(true);
    return;
  }
  Resolved& r = it->second;
  auto log_once = [&](const char* why) {
    if (lock_logged_.size() < 24 && lock_logged_.insert(c.texture ^ (uint32_t(std::hash<std::string_view>{}(why)) << 1)).second) {
      REXLOG_INFO("deko3d: texture lock {:08X} (level {}, array {}, dimension {}): {}; resolved image {}x{} host format "
                  "{}, guest format {}, base {:08X}",
                  c.texture, c.level, c.array_index, uint32_t(c.dimension), why, r.image.width, r.image.height,
                  int(r.image.format), r.guest_format, r.base);
    }
  };
  if (c.level || c.array_index || c.dimension != TextureLockDimension::k2D || r.image.depth) {
    ++lock_stats_.unsupported;
    log_once("not read back (a level, array slice, dimension or depth the Vulkan path refuses too)");
    complete(false);
    return;
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
  static_assert(sizeof(fetch) == sizeof(r.fetch));
  std::memcpy(&fetch, r.fetch.data(), sizeof(fetch));
  rex::graphics::TextureInfo info;
  const rex::graphics::FormatInfo* format_info = rex::graphics::TextureInfo::Prepare(fetch, &info) ? info.format_info() : nullptr;
  const uint32_t bytes_per_block = format_info ? format_info->bytes_per_block() : 0;
  uint32_t mip_width = 0, mip_height = 0;
  if (format_info) {
    info.GetMipSize(0, &mip_width, &mip_height);
  }
  if (!format_info || format_info->type == rex::graphics::FormatType::kCompressed || bytes_per_block != BytesPerTexel(r.image.format) ||
      !mip_width || !mip_height || mip_width > r.image.width || mip_height > r.image.height ||
      (bytes_per_block & (bytes_per_block - 1))) {
    ++lock_stats_.layout_mismatch;
    log_once("NOT read back: the fetch constant's layout does not match the resolved image");
    complete(false);
    return;
  }
  const uint32_t bytes = mip_width * mip_height * bytes_per_block;
  if (lock_bytes_ < bytes) {
    lock_bytes_ = (bytes + 0xFFFF) & ~0xFFFFu;
    lock_block_ = core_.CreateReadbackBlock(lock_bytes_, "texture lock read-back");
    if (!lock_block_) {
      lock_bytes_ = 0;
      complete(false);
      return;
    }
  }
  // Every draw before the lock in the stream is in the open work: the copy goes after them, the work is submitted,
  // and this thread waits for the GPU (the title waits for this lock anyway).
  dkMemBlockFlushCpuCache(lock_block_, 0, bytes);
  EnsureWork();
  BeforeTransfer();
  DkImageView view;
  dkImageViewDefaults(&view, &r.image.image);
  const DkImageRect rect = {0, 0, 0, mip_width, mip_height, 1};
  const DkCopyBuf to = {dkMemBlockGetGpuAddr(lock_block_), mip_width * bytes_per_block,
                        mip_width * bytes_per_block * mip_height};
  dkCmdBufCopyImageToBuffer(core_.commands(), &view, &rect, &to, 0);
  CloseWork();
  core_.WaitIdle();
  dkMemBlockFlushCpuCache(lock_block_, 0, bytes);
  const uint8_t* source = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(lock_block_));
  // Into guest memory as the Vulkan path writes it: the mip's guest location, tiled or linear, each block swapped by
  // the fetch's endianness.
  uint32_t packed_x = 0, packed_y = 0;
  const uint32_t guest_mip_address = info.GetMipLocation(0, &packed_x, &packed_y, true);
  uint8_t* guest_mip = memory_->TranslatePhysical<uint8_t*>(guest_mip_address);
  if (!guest_mip) {
    ++lock_stats_.layout_mismatch;
    log_once("NOT read back: no guest memory at the mip's address");
    complete(false);
    return;
  }
  const rex::graphics::TextureExtent guest_extent = info.GetMipExtent(0, true);
  const uint32_t log2 = uint32_t(std::countr_zero(bytes_per_block));
  for (uint32_t y = 0; y < mip_height; ++y) {
    for (uint32_t x = 0; x < mip_width; ++x) {
      const uint32_t dx = packed_x + x, dy = packed_y + y;
      const int32_t offset = info.is_tiled
                                 ? rex::graphics::texture_util::GetTiledOffset2D(int32_t(dx), int32_t(dy), guest_extent.block_pitch_h, log2)
                                 : int32_t((dy * guest_extent.block_pitch_h + dx) * bytes_per_block);
      if (offset < 0) {
        continue;
      }
      rex::graphics::texture_conversion::CopySwapBlock(info.endianness, guest_mip + uint32_t(offset),
                                        source + (size_t(y) * mip_width + x) * bytes_per_block, bytes_per_block);
    }
  }
  ++lock_stats_.copied;
  lock_stats_.bytes += bytes;
  result.copied_to_guest = 1;
  log_once("read back from the GPU into guest memory");
  complete(true);
}

// ---- Textures (nfsmw_dk_texturas.cpp's PrepararTextura without the sampled rechecks, the per-frame budget and the
// cubemaps of resolved faces)

bool DkRecorder::FormatOf(uint32_t format, TextureFormat& f) {
  switch (format) {
    case 2:
    case 8: f = {DkImageFormat_R8_Unorm, 1, 1, 0, kSwizzleRRRR}; return true;
    case 10: f = {DkImageFormat_RG8_Unorm, 1, 2, 2, kSwizzleRGGG}; return true;
    case 6:
    case 50: f = {DkImageFormat_RGBA8_Unorm, 1, 4, 4, kSwizzleRGBA}; return true;
    case 7:
    case 54: f = {DkImageFormat_RGB10A2_Unorm, 1, 4, 4, kSwizzleRGBA}; return true;
    case 18:
    case 51: f = {DkImageFormat_RGBA_BC1, 4, 8, 2, kSwizzleRGBA}; return true;
    case 19:
    case 52: f = {DkImageFormat_RGBA_BC2, 4, 16, 2, kSwizzleRGBA}; return true;
    case 20:
    case 53: f = {DkImageFormat_RGBA_BC3, 4, 16, 2, kSwizzleRGBA}; return true;
    case 49: f = {DkImageFormat_RG_BC5_Unorm, 4, 16, 2, kSwizzleRGGG}; return true;
    case 59: f = {DkImageFormat_R_BC4_Unorm, 4, 8, 2, kSwizzleRRRR}; return true;
    case 24: f = {DkImageFormat_R16_Unorm, 1, 2, 2, kSwizzleRRRR}; return true;
    case 25: f = {DkImageFormat_RG16_Unorm, 1, 4, 2, kSwizzleRGGG}; return true;
    case 26: f = {DkImageFormat_RGBA16_Unorm, 1, 8, 2, kSwizzleRGBA}; return true;
    case 30: f = {DkImageFormat_R16_Float, 1, 2, 2, kSwizzleRRRR}; return true;
    case 31: f = {DkImageFormat_RG16_Float, 1, 4, 2, kSwizzleRGGG}; return true;
    case 32: f = {DkImageFormat_RGBA16_Float, 1, 8, 2, kSwizzleRGBA}; return true;
    case 36: f = {DkImageFormat_R32_Float, 1, 4, 4, kSwizzleRRRR}; return true;
    case 37: f = {DkImageFormat_RG32_Float, 1, 8, 4, kSwizzleRGGG}; return true;
    case 38: f = {DkImageFormat_RGBA32_Float, 1, 16, 4, kSwizzleRGBA}; return true;
    default: return false;
  }
}

uint32_t DkRecorder::ReserveImageDescriptor() {
  if (!free_descriptors_.empty()) {
    const uint32_t r = free_descriptors_.back();
    free_descriptors_.pop_back();
    return r;
  }
  if (next_descriptor_ < core_.config().image_descriptors) {
    return next_descriptor_++;
  }
  return 0;
}

void DkRecorder::ReleaseDescriptorsOf(uint64_t serial) {
  const auto it = views_by_serial_.find(serial);
  if (it == views_by_serial_.end()) {
    return;
  }
  auto& later = descriptors_to_free_[work_ % kSlots];
  for (uint64_t key : it->second) {
    if (const auto v = views_.find(key); v != views_.end()) {
      later.push_back(v->second);
      views_.erase(v);
    }
  }
  views_by_serial_.erase(it);
}

void DkRecorder::WriteImageDescriptor(uint32_t slot, const DkImageView& view) {
  EnsureWork();
  DkImageDescriptor d;
  dkImageDescriptorInitialize(&d, &view, false, false);
  // In the pre-list: ordered before every draw of this submission (a freed slot is reused only after its fence).
  dkCmdBufPushData(core_.PreCommands(), core_.ImageDescriptors() + DkGpuAddr(slot) * sizeof(d), &d, sizeof(d));
}

uint32_t DkRecorder::ViewOf(const DkImageEntry& image, uint32_t guest_swizzle, uint16_t host_swizzle, bool stencil) {
  const uint64_t key =
      (image.serial << 24) ^ (uint64_t(guest_swizzle & 0xFFF) << 12) ^ host_swizzle ^ (stencil ? 1ull << 63 : 0);
  if (const auto it = views_.find(key); it != views_.end()) {
    return it->second;
  }
  const uint32_t slot = ReserveImageDescriptor();
  if (!slot) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("deko3d: image descriptor table full");
    }
    return 0;
  }
  DkImageView v;
  dkImageViewDefaults(&v, &image.image);
  ViewSwizzle(guest_swizzle, host_swizzle, v.swizzle);
  if (image.depth) {
    v.dsSource = stencil ? DkDsSource_Stencil : DkDsSource_Depth;
  }
  WriteImageDescriptor(slot, v);
  views_.emplace(key, slot);
  views_by_serial_[image.serial].push_back(key);
  return slot;
}

uint32_t DkRecorder::EmptyOf(uint8_t shader_type) const {
  switch (shader_type) {
    case 2: return 1;  // 2D array
    case 3: return 2;  // 3D
    case 4: return 3;  // cube
    default: return 0;
  }
}

bool DkRecorder::CreateEmpties() {
  static constexpr DkImageType kTypes[4] = {DkImageType_2D, DkImageType_2DArray, DkImageType_3D, DkImageType_Cubemap};
  for (uint32_t i = 0; i < 4; ++i) {
    DkImageEntry& e = empties_[i];
    if (!CreateImage(e, kTypes[i], DkImageFormat_RGBA8_Unorm, 1, 1, 1, 1, 0, "empty texture")) {
      return false;
    }
    const Upload u = Reserve(4 * 6, 256);
    if (!u) {
      return false;
    }
    std::memset(u.cpu, 0, 4 * 6);
    DkImageView view;
    dkImageViewDefaults(&view, &e.image);
    const DkCopyBuf from = {u.gpu, 4, 4};
    const DkImageRect rect = {0, 0, 0, 1, 1, i == 3 ? 6u : 1u};
    dkCmdBufCopyBufferToImage(core_.PreCommands(), &from, &view, &rect, 0);
    WriteImageDescriptor(i, view);
  }
  // Sampler 0: bilinear, clamped (the output pass and the empties).
  DkSampler s;
  dkSamplerDefaults(&s);
  s.minFilter = s.magFilter = DkFilter_Linear;
  s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  dkCmdBufPushData(core_.PreCommands(), core_.SamplerDescriptors(), &d, sizeof(d));
  next_descriptor_ = 8;
  return true;
}

uint32_t DkRecorder::SamplerOf(const std::array<uint32_t, 6>& f, bool point) {
  const uint32_t filters = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
  const uint32_t mips = (f[4] >> 2) & 0xFF;       // minimum in 0-3, maximum in 4-7
  const uint32_t key = ((f[0] >> 10) & 0x1FF) | (filters << 9) | (mips << 21) | ((f[5] & 0x3) << 29) |
                       (point ? 0x80000000u : 0u);
  if (const auto it = samplers_.find(key); it != samplers_.end()) {
    return it->second;
  }
  if (next_sampler_ >= core_.config().sampler_descriptors) {
    return 0;
  }
  // The halfway clamps as edge clamps, the mirrored ones as mirror-to-edge (nfsmw's, from its Vulkan path).
  static constexpr DkWrapMode kModes[8] = {
      DkWrapMode_Repeat,      DkWrapMode_MirroredRepeat,    DkWrapMode_ClampToEdge,   DkWrapMode_MirrorClampToEdge,
      DkWrapMode_ClampToEdge, DkWrapMode_MirrorClampToEdge, DkWrapMode_ClampToBorder, DkWrapMode_MirrorClampToEdge};
  DkSampler s;
  dkSamplerDefaults(&s);
  s.magFilter = ((f[3] >> 19) & 0x3) == 1 && !point ? DkFilter_Linear : DkFilter_Nearest;
  s.minFilter = ((f[3] >> 21) & 0x3) == 1 && !point ? DkFilter_Linear : DkFilter_Nearest;
  const uint32_t mip = (filters >> 4) & 0x3;
  s.mipFilter = mip == 1 && !point ? DkMipFilter_Linear : DkMipFilter_Nearest;
  s.wrapMode[0] = kModes[(f[0] >> 10) & 0x7];
  s.wrapMode[1] = kModes[(f[0] >> 13) & 0x7];
  s.wrapMode[2] = kModes[(f[0] >> 16) & 0x7];
  s.lodClampMin = float(mips & 0xF);
  s.lodClampMax = mip == 2 ? s.lodClampMin + 0.25f : std::max(s.lodClampMin, float((f[4] >> 6) & 0xF));
  if ((f[5] & 0x3) == 1) {  // opaque white; otherwise transparent black
    for (auto& c : s.borderColor) {
      c.value_f = 1.0f;
    }
  }
  const uint32_t slot = next_sampler_++;
  DkSamplerDescriptor d;
  dkSamplerDescriptorInitialize(&d, &s);
  EnsureWork();
  dkCmdBufPushData(core_.PreCommands(), core_.SamplerDescriptors() + DkGpuAddr(slot) * sizeof(d), &d, sizeof(d));
  samplers_.emplace(key, slot);
  return slot;
}

bool DkRecorder::ReadLevel(uint64_t address, bool tiled, uint32_t pitch_blocks, uint64_t row_bytes,
                           const TextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                           uint32_t bx_host, uint8_t* out) {
  if (!bx || !by) {
    return true;
  }
  const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
  const uint64_t extent =
      tiled ? uint64_t(std::max<int64_t>(
                  TiledOffset2D(int32_t((ox + bx + 31) & ~31u), int32_t((oy + by + 31) & ~31u), pitch_blocks, log2), 0))
            : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
  if (address + extent > 0x20000000ull) {
    return false;
  }
  const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
  if (!tiled) {
    for (uint32_t y = 0; y < by; ++y) {
      std::memcpy(out + size_t(y) * bx_host * tf.bytes, source + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes,
                  size_t(bx) * tf.bytes);
    }
    return true;
  }
  switch (log2) {
    case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, out); break;
    case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, out); break;
    case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, out); break;
    case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, out); break;
    default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, out); break;
  }
  return true;
}

uint32_t DkRecorder::PrepareTexture(const RecorderTexture& t, uint8_t shader_type, uint32_t& width_out,
                                    uint32_t& height_out, bool& point) {
  const std::array<uint32_t, 6>& f = t.fetch;
  width_out = height_out = 0;
  point = false;
  if ((f[0] & 0x3) != 2) {  // not a texture fetch constant
    return EmptyOf(shader_type);
  }
  const uint32_t dimension = (f[5] >> 9) & 0x3;
  const bool cube = dimension == 3;
  const bool volume = dimension == 2;
  if (dimension == 0 || shader_type == 2) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("deko3d: 1D textures and 2D arrays not supported yet: an empty texture is used");
    }
    return EmptyOf(shader_type);
  }
  if ((shader_type == 3) != volume || (shader_type == 4) != cube) {
    return EmptyOf(shader_type);
  }
  const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
  const uint32_t base = (f[1] >> 12) << 12;
  if (!cube && !volume) {
    // The title's own A8R8G8B8 alias of a resolved depth, by its texture object (run 39: found by address the alias
    // fetch carries a 32x32 placeholder size and a size test cannot hold; the registration names the depth exactly).
    if (const auto registered = alias_sources_.find(t.handle); registered != alias_sources_.end()) {
      const uint32_t alias_format = f[1] & 0x3F;
      const auto source = resolved_.find(registered->second);
      if (source == resolved_.end() || !source->second.written || !source->second.image.depth) {
        ++alias_stats_.source_missing;
      } else {
        alias_stats_.other_format += alias_format != kFetchFormat8888 && alias_format != kFetchFormat8888As16;
        if (DkImageEntry* alias = PackedDepthAlias(registered->second)) {
          ++alias_stats_.by_registration;
          width_out = alias->width;
          height_out = alias->height;
          point = true;
          return ViewOf(*alias, swizzle, kSwizzleRGBA);
        }
      }
    }
    bool by_base = false;
    uint32_t object = 0;
    DkImageEntry* resolved = ResolvedTexture(t.handle, base & 0x1FFFFFFF, &by_base, &object);
    bool packed_fetch = false;
    if (resolved) {
      const uint32_t fetch_format = f[1] & 0x3F;
      const bool mismatch = resolved->depth != IsDepthFetchFormat(fetch_format);
      if (by_base) {
        ++gaps_.base_lookups;
        gaps_.base_type_mismatch += mismatch;
      }
      packed_fetch = IsPackedDepthFetch(resolved->depth, fetch_format, (f[2] & 0x1FFF) + 1,
                                        ((f[2] >> 13) & 0x1FFF) + 1, resolved->width, resolved->height);
      // Run 48: found only by its address, of the other type and not the depth's A8R8G8B8 view: the title's own
      // texture that shares the address (run 47: handle 40079DA0, 32x32 A8R8G8B8 at the depth's base, 63,886 fetches;
      // on the PC the title fills it from the CPU, LRA pc.log `unlock handle=40079DA0 access=1 range=0+4096`, and LRA
      // looks textures up by handle only). Read from guest memory below, as before the depth existed.
      if (options_.mixup_from_memory && by_base && mismatch && !packed_fetch) {
        ++gaps_.mixup_from_memory;
        ++alias_stats_.other_mismatch;
        if (mismatch_logged_.size() < 16 && mismatch_logged_.insert(t.handle).second) {
          REXLOG_INFO("deko3d: depth/colour mix-up: fetch handle {:08X} base {:08X} format {} {}x{} swizzle {:03X} "
                      "shares the address of resolved object {:08X} {}x{} {}: read from guest memory "
                      "(nx_dk_mixup_from_memory)",
                      t.handle, base, fetch_format, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, swizzle, object,
                      resolved->width, resolved->height, resolved->depth ? "depth" : "colour");
        }
        resolved = nullptr;
      }
    }
    if (resolved) {
      const uint32_t fetch_format = f[1] & 0x3F;
      const bool mismatch = resolved->depth != IsDepthFetchFormat(fetch_format);
      // The title's A8R8G8B8 view of a resolved depth: the packed bytes, not the depth as a float.
      if (packed_fetch) {
        if (DkImageEntry* alias = PackedDepthAlias(object)) {
          width_out = alias->width;
          height_out = alias->height;
          point = true;  // bytes of one number: never filtered
          return ViewOf(*alias, swizzle, kSwizzleRGBA);
        }
      } else if (mismatch) {
        ++alias_stats_.other_mismatch;
        // What these are (run 39 could only count them): the first 16 distinct fetch handles.
        if (mismatch_logged_.size() < 16 && mismatch_logged_.insert(t.handle).second) {
          REXLOG_INFO("deko3d: depth/colour mix-up: fetch handle {:08X} base {:08X} format {} {}x{} swizzle {:03X} -> "
                      "resolved object {:08X} {}x{} {} (guest format {}, found {}, {} alias registrations)",
                      t.handle, base, fetch_format, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, swizzle, object,
                      resolved->width, resolved->height, resolved->depth ? "depth" : "colour",
                      resolved_.count(object) ? resolved_[object].guest_format : 0, by_base ? "by base" : "by handle",
                      alias_sources_.size());
        }
      }
      width_out = resolved->width;
      height_out = resolved->height;
      if (resolved->depth) {
        point = true;
        return ViewOf(*resolved, swizzle, kSwizzleRRRR);
      }
      // A colour the GPU produced keeps its lanes as stored: the fetch's A8R8G8B8 swizzle (0x60A) is for memory order
      // and would exchange red and blue (run 41; dk_view_swizzle.h); other swizzles are applied as before. Which
      // fetches come here is logged once each.
      const uint32_t colour_swizzle = ResolvedColourGuestSwizzle(swizzle);
      ++gaps_.resolved_colour_fetches;
      gaps_.resolved_colour_stored += colour_swizzle != swizzle;
      if (swizzles_logged_.size() < 16 && swizzles_logged_.insert((swizzle << 8) | (uint32_t(resolved->format) & 0xFF)).second) {
        REXLOG_INFO("deko3d: resolved colour {:08X} fetched as format {} with swizzle {:03X}: {} (host format {})",
                    object, fetch_format, swizzle,
                    colour_swizzle != swizzle ? "viewed as stored" : "swizzle applied as fetched", int(resolved->format));
      }
      return ViewOf(*resolved, colour_swizzle, kSwizzleRGBA);
    }
  }
  const uint32_t format = f[1] & 0x3F;
  TextureFormat tf;
  if (!FormatOf(format, tf)) {
    static std::unordered_set<uint32_t> logged;
    if (logged.insert(format).second) {
      REXLOG_WARN("deko3d: texture format {} not supported yet: an empty texture is used", format);
    }
    return EmptyOf(shader_type);
  }
  const uint32_t width = volume ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
  const uint32_t height = volume ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t depth = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
  if (volume) {
    // 3D textures come later (B3/B4); the empty one keeps the draw. Run 48: counted.
    ++gaps_.volume_fetches;
    return EmptyOf(shader_type);
  }
  const uint32_t layers = cube ? 6 : 1;
  const uint32_t blocks_x = (width + tf.block - 1) / tf.block;
  const uint32_t blocks_y = (height + tf.block - 1) / tf.block;
  const uint32_t pitch_blocks = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);
  const uint64_t face_stride =
      (uint64_t((pitch_blocks + 31) & ~31u) * tf.bytes * ((blocks_y + 31) & ~31u) + 4095) & ~uint64_t(4095);
  const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
  const uint64_t key = XXH3_64bits(keys, sizeof(keys));

  // Mip levels: no mip address, no mips; the maximum clamped to the size; without the base, or with a minimum above
  // 0, the base is not read (GetSubresourcesFromFetchConstant).
  const bool tiled = (f[0] >> 31) & 0x1;
  const uint64_t base_address = uint64_t(base) & 0x1FFFFFFF;
  const uint64_t mip_address = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
  const uint32_t packed = ((f[5] >> 11) & 0x1) ? PackedLevel(width, height) : UINT32_MAX;
  uint32_t max_level = 0;
  bool read_base = true;
  if (mip_address != 0) {
    const uint32_t size_max = Log2Floor(std::max(width, height));
    uint32_t min_level = std::min((f[4] >> 2) & 0xFu, size_max);
    max_level = std::max(std::min((f[4] >> 6) & 0xFu, size_max), min_level);
    if (max_level != 0) {
      if (base_address == 0) {
        min_level = std::max(min_level, 1u);
      }
      read_base = min_level == 0;
    }
  }
  struct MipRegion {
    uint64_t offset = 0, row_bytes = 0, stride = 0;
    uint32_t pitch_blocks = 0;
  };
  std::array<MipRegion, 16> regions{};
  uint64_t mips_extent = 0;
  if (max_level != 0) {
    const uint32_t last = packed == 0 ? 0 : std::min(max_level, packed);
    for (uint32_t s = packed == 0 ? 0 : 1; s <= last; ++s) {
      MipRegion& region = regions[s];
      const uint32_t row_texels = std::max(std::bit_ceil(width) >> s, 1u);
      const uint32_t rows_texels = std::max(std::bit_ceil(height) >> s, 1u);
      region.pitch_blocks = ((row_texels + tf.block - 1) / tf.block + 31) & ~31u;
      region.row_bytes = uint64_t(region.pitch_blocks) * tf.bytes;
      if (!tiled) {
        region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
      }
      const uint64_t rows_blocks = uint64_t(((rows_texels + tf.block - 1) / tf.block + 31) & ~31u);
      region.stride = (region.row_bytes * rows_blocks + 4095) & ~uint64_t(4095);
      region.offset = mips_extent;
      mips_extent += region.stride * layers;
    }
    if (mip_address + mips_extent > 0x20000000ull) {
      max_level = 0;
      mips_extent = 0;
      read_base = true;
    }
  }
  uint32_t base_ox = 0, base_oy = 0;
  if (packed == 0) {
    PackedOffset(width, height, tf.block, 0, base_ox, base_oy);
  }

  Texture& texture = textures_[key];
  if (!texture.image.live) {
    const uint32_t width_host = tf.block > 1 ? (width + 3) & ~3u : width;
    const uint32_t height_host = tf.block > 1 ? (height + 3) & ~3u : height;
    const uint32_t levels = std::min(max_level + 1, Log2Floor(std::max(width_host, height_host)) + 1);
    if (!CreateImage(texture.image, cube ? DkImageType_Cubemap : DkImageType_2D, tf.format, width_host, height_host,
                     1, levels, 0, "texture")) {
      textures_.erase(key);
      return EmptyOf(shader_type);
    }
    for (uint32_t n = 0; n < levels; ++n) {
      texture.bytes += uint64_t((std::max(width_host >> n, 1u) + tf.block - 1) / tf.block) *
                       ((std::max(height_host >> n, 1u) + tf.block - 1) / tf.block) * tf.bytes * layers;
    }
    texture_bytes_ += texture.bytes;
    if (++textures_created_ <= 48) {
      REXLOG_INFO("deko3d: texture {:08X} {}x{} format {} {} pitch {} swizzle {:03X}{}; {} levels", base, width,
                  height, format, tiled ? "tiled" : "linear", ((f[0] >> 22) & 0x1FF) << 5, swizzle,
                  cube ? " (cube)" : "", levels);
    }
  }
  const uint32_t slot = ViewOf(texture.image, swizzle, tf.host_swizzle);
  width_out = texture.image.width;
  height_out = texture.image.height;
  const uint64_t frame = frames_.load();
  if (dirty_handles_.erase(t.handle)) {
    texture.next = 0;  // the title wrote it: checked now
  }
  if (texture.frame == frame || (texture.has_content && frame < texture.next)) {
    return slot;
  }
  texture.frame = frame;
  // The guest bytes are fingerprinted before untiling; unchanged, nothing to do (and checked less often).
  const uint32_t log2_block = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
  const uint32_t read_x = blocks_x + base_ox, read_y = blocks_y + base_oy;
  const uint64_t layer_extent =
      tiled ? uint64_t(std::max<int64_t>(
                  TiledOffset2D(int32_t((read_x - 1) & ~31u), int32_t((read_y - 1) & ~31u), pitch_blocks, log2_block),
                  0)) +
                  (log2_block == 0 ? 0xA00u : log2_block == 1 ? 0xC00u : (0x400u << log2_block))
            : uint64_t(std::max(pitch_blocks, read_x)) * tf.bytes * (read_y - 1) + uint64_t(read_x) * tf.bytes;
  const uint64_t extent = (layers - 1) * face_stride + layer_extent;
  if (base_address + extent <= 0x20000000ull) {
    uint64_t hash = XXH3_64bits(memory_->TranslatePhysical(uint32_t(base_address)), size_t(extent));
    if (mips_extent) {
      hash = XXH3_64bits_withSeed(memory_->TranslatePhysical(uint32_t(mip_address)), size_t(mips_extent), hash);
    }
    if (texture.has_content && hash == texture.raw_hash) {
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
      return slot;
    }
    texture.raw_hash = hash;
  }
  // Every level, layer after layer, untiled and in host byte order.
  const DkImageEntry& img = texture.image;
  std::array<size_t, 16> level_offset{}, layer_bytes{};
  size_t data_bytes = 0;
  for (uint32_t n = 0; n < img.levels; ++n) {
    const uint32_t bx = (std::max(img.width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by = (std::max(img.height >> n, 1u) + tf.block - 1) / tf.block;
    level_offset[n] = data_bytes;
    layer_bytes[n] = size_t(bx) * by * tf.bytes;
    data_bytes += layer_bytes[n] * layers;
  }
  scratch_.assign(data_bytes, 0);
  const uint32_t bx_host = (img.width + tf.block - 1) / tf.block;
  for (uint32_t c = 0; c < (read_base ? layers : 0u); ++c) {
    if (!ReadLevel(base_address + c * face_stride, tiled, pitch_blocks,
                   uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf, base_ox, base_oy, blocks_x, blocks_y,
                   bx_host, scratch_.data() + layer_bytes[0] * c)) {
      return texture.has_content ? slot : EmptyOf(shader_type);
    }
  }
  for (uint32_t n = 1; n < img.levels; ++n) {
    const uint32_t s = packed == 0 ? 0 : std::min(n, packed);
    uint32_t ox = 0, oy = 0;
    if (n >= packed) {
      PackedOffset(width, height, tf.block, n, ox, oy);
    }
    const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t bx_level = (std::max(img.width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by_level = (std::max(img.height >> n, 1u) + tf.block - 1) / tf.block;
    bool outside = false;
    for (uint32_t c = 0; c < layers && !outside; ++c) {
      outside = !ReadLevel(mip_address + regions[s].offset + c * regions[s].stride, tiled, regions[s].pitch_blocks,
                           regions[s].row_bytes, tf, ox, oy, std::min(bx_guest, bx_level), std::min(by_guest, by_level),
                           bx_level, scratch_.data() + level_offset[n] + layer_bytes[n] * c);
    }
    if (outside) {
      break;
    }
  }
  const uint32_t order = (f[1] >> 6) & 0x3;
  if (order && (tf.unit == 2 || tf.unit == 4)) {
    SwapTextureBytes(scratch_.data(), scratch_.size(), tf.unit, order);
  }
  const Upload upload = Reserve(uint32_t(data_bytes), 256);
  if (!upload) {
    return texture.has_content ? slot : EmptyOf(shader_type);
  }
  std::memcpy(upload.cpu, scratch_.data(), data_bytes);
  DkCmdBuf pre = core_.PreCommands();
  for (uint32_t n = 0; n < img.levels; ++n) {
    const uint32_t bx = (std::max(img.width >> n, 1u) + tf.block - 1) / tf.block;
    const uint32_t by = (std::max(img.height >> n, 1u) + tf.block - 1) / tf.block;
    DkImageView view;
    dkImageViewDefaults(&view, &img.image);
    view.mipLevelOffset = uint8_t(n);
    const uint32_t row = bx * tf.bytes;
    const DkCopyBuf from = {upload.gpu + level_offset[n], row, row * by};
    const DkImageRect rect = {0, 0, 0, std::max(img.width >> n, 1u), std::max(img.height >> n, 1u), layers};
    dkCmdBufCopyBufferToImage(pre, &from, &view, &rect, 0);
  }
  texture.has_content = true;
  texture.interval = 1;
  texture.next = frame + 1;
  ++textures_uploaded_;
  bytes_uploaded_ += data_bytes;
  return slot;
}

void DkRecorder::ReleaseTexturesIfNeeded() {
  // Free descriptors: never handed out plus given back (those waiting for a fence are not counted: they come back).
  const uint32_t limit = core_.config().image_descriptors;
  const uint32_t free = limit - std::min(limit, next_descriptor_) + uint32_t(free_descriptors_.size());
  uint32_t descriptors_to_release = free < kDescriptorReserve ? kDescriptorReserve * 2 : 0;
  if (texture_bytes_ <= kTextureLimitBytes && !descriptors_to_release) {
    return;
  }
  const uint64_t target = kTextureLimitBytes / 4 * 3;
  const uint64_t frame = frames_.load();
  EnsureWork();
  for (auto it = textures_.begin(); it != textures_.end() && (texture_bytes_ > target || descriptors_to_release);) {
    Texture& t = it->second;
    if (t.frame != UINT64_MAX && t.frame + kFramesUnusedToRelease < frame) {
      if (descriptors_to_release) {
        --descriptors_to_release;
        ++gaps_.descriptor_evictions;
      }
      texture_bytes_ -= t.bytes;
      DestroyImage(t.image);
      it = textures_.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- Shaders

const DkShader* DkRecorder::ShaderFor(const PackageShader& shader, uint32_t value, bool& pending) {
  pending = false;
  const ShaderKey key{shader.hash, value, shader.late};
  if (const auto it = shaders_.find(key); it != shaders_.end()) {
    return it->second.get();
  }
  if (compiling_.count(key)) {
    pending = true;
    return nullptr;
  }
  uint32_t blob = 0xFFFFFFFFu;
  for (const PackageShader::Variant& v : shader.variants) {
    if (v.value == value) {
      blob = v.blob;
      break;
    }
  }
  char name[64];
  std::snprintf(name, sizeof(name), "%s %016" PRIX64 "%s %X", shader.pixel ? "ps" : "vs", shader.hash,
                shader.late ? " late" : "", value);
  if (blob != 0xFFFFFFFFu) {
    const std::vector<uint8_t> dksh = package_.ReadBlob(blob);
    auto loaded = std::make_unique<DkShader>();
    if (!dksh.empty() && core_.LoadShader(*loaded, dksh, name)) {
      ++shaders_from_pc_;
      return (shaders_[key] = std::move(loaded)).get();
    }
    shaders_[key] = nullptr;
    ++shaders_failed_;
    return nullptr;
  }
  // The PC had no DKSH for it: the console's uam compiles it in the background (B3); its draws are skipped until
  // it is ready.
  const std::string text = package_.VariantText(shader, value);
  if (text.empty()) {
    shaders_[key] = nullptr;
    ++shaders_failed_;
    REXLOG_WARN("deko3d: {}: no DKSH and no GLSL in the package; its draws are skipped", name);
    return nullptr;
  }
  compiling_.insert(key);
  pending = true;
  REXLOG_INFO("deko3d: {}: not compiled on the PC; the console's uam compiles it in the background", name);
  std::weak_ptr<Mailbox> mailbox = mailbox_;
  compiler_.Queue(!shader.pixel, text, [mailbox, key](Compiler::Result r) {
    if (const auto m = mailbox.lock()) {
      std::lock_guard<std::mutex> lock(m->mutex);
      if (r.ok) {
        m->done.emplace_back(key, std::move(r.dksh));
      } else {
        m->failed.emplace_back(key, r.log.substr(0, 300));
      }
    }
  });
  return nullptr;
}

// Run 56: a pruned vertex shader variant (PackageShader::kVariant*), from the package's DKSH only: never queued for the
// console's uam (5-30 s a variant), never counted as a failure; none = nullptr, remembered.
const DkShader* DkRecorder::PrunedShaderFor(const PackageShader& shader, uint32_t value) {
  const ShaderKey key{shader.hash, value, shader.late};
  if (const auto it = shaders_.find(key); it != shaders_.end()) {
    return it->second.get();
  }
  for (const PackageShader::Variant& v : shader.variants) {
    if (v.value != value || v.blob == 0xFFFFFFFFu) {
      continue;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "vs %016" PRIX64 " %X (pruned)", shader.hash, value);
    const std::vector<uint8_t> dksh = package_.ReadBlob(v.blob);
    auto loaded = std::make_unique<DkShader>();
    if (!dksh.empty() && core_.LoadShader(*loaded, dksh, name)) {
      ++pruned_loaded_;
      return (shaders_[key] = std::move(loaded)).get();
    }
    break;
  }
  shaders_[key] = nullptr;
  return nullptr;
}

void DkRecorder::CollectCompiles() {
  std::vector<std::pair<ShaderKey, std::vector<uint8_t>>> done;
  std::vector<std::pair<ShaderKey, std::string>> failed;
  {
    std::lock_guard<std::mutex> lock(mailbox_->mutex);
    done.swap(mailbox_->done);
    failed.swap(mailbox_->failed);
  }
  for (auto& [key, dksh] : done) {
    compiling_.erase(key);
    auto shader = std::make_unique<DkShader>();
    if (core_.LoadShader(*shader, dksh, "uam variant")) {
      ++shaders_compiled_;
      shaders_[key] = std::move(shader);
    } else {
      shaders_[key] = nullptr;
      ++shaders_failed_;
    }
  }
  for (auto& [key, log] : failed) {
    compiling_.erase(key);
    shaders_[key] = nullptr;
    ++shaders_failed_;
    REXLOG_WARN("deko3d: the console's uam failed on {:016X} value {:X}: {}", key.hash, key.value, log);
  }
}

// ---- Draws

void DkRecorder::ShaderMissing(uint64_t hash, bool pixel) {
  // Run 61: name the shader (0 = the title bound none), so a skipped draw is traced to its container
  // (tools/deko3d/gta4_fxc_shaders.py's manifest lists every one the game's files hold). 64 hashes at most.
  if (missing_shaders_logged_.size() < 64 && missing_shaders_logged_.insert(hash ^ (pixel ? 1ull << 63 : 0)).second) {
    REXLOG_WARN("deko3d: run 61 shader not in the package: {} shader {:016X} (draw {}); its draws are skipped",
                pixel ? "pixel" : "vertex", hash, draws_);
  }
}

bool DkRecorder::DrawRejected(uint32_t reason, const char* text) {
  ++skipped_[reason];
  if (skipped_logged_.emplace(reason, true).second) {
    REXLOG_WARN("deko3d: draws skipped: {} (reason {}; counted in the report)", text, reason);
  }
  return false;
}

void DkRecorder::ApplyConstants(const RecorderDraw& d) {
  DeviceConstants& c = constants_[d.device];
  const bool vertex = ApplyConstantDelta(d.vertex_constants, c.vertex.data(), c.vertex_dirty.data(),
                                         kVertexConstantRegisters);
  const bool pixel = ApplyConstantDelta(d.pixel_constants, c.pixel.data(), c.pixel_dirty.data(),
                                        kPixelConstantRegisters);
  c.any_dirty = c.any_dirty || vertex || pixel;
  c.vertex_changed = c.vertex_changed || vertex;
  c.pixel_changed = c.pixel_changed || pixel;
  if (d.constants_hashed) {
    // Run 50: the banks the GPU will be given for this draw against the front end's at submit.
    ++gaps_.constants_checked;
    const std::array<uint64_t, 3> mine = {
        ConstantBankHash(c.vertex.data(), 0, 64, false),
        ConstantBankHash(c.vertex.data(), 64, kVertexConstantRegisters - 64, false),
        ConstantBankHash(c.pixel.data(), 0, kPixelConstantRegisters, false)};
    bool differ = false;
    for (uint32_t i = 0; i < 3; ++i) {
      if (mine[i] != d.constants_hash[i]) {
        ++gaps_.constants_differ[i];
        differ = true;
      }
    }
    if (differ && constants_logged_ < 16) {
      ++constants_logged_;
      REXLOG_WARN("deko3d: draw {} (vertex shader {:016X}): the recorder's constant banks differ from the front end's "
                  "(vertex c0-c63 {}, c64-c255 {}, pixel {})",
                  d.sequence, d.vertex_shader, mine[0] != d.constants_hash[0], mine[1] != d.constants_hash[1],
                  mine[2] != d.constants_hash[2]);
    }
  }
}

void DkRecorder::PushConstants(DkCmdBuf cmd, uint32_t device, bool vertex_used, bool pixel_used) {
  DeviceConstants& c = constants_[device];
  if (options_.constant_versions) {
    // Run 50: every change gets its own block, filled by the CPU now and never written again, so a draw reads exactly
    // the bank of its own submit whatever the GPU runs before or after it (the Vulkan path's versioned constant arena,
    // graphics_system.cpp NativeConstantArenaAlignment). Up to run 49 one block per work was updated inline with
    // dkCmdBufPushConstants, before the block was bound again after an internal pass. The draw reserved the room for
    // both blocks (Draw: `needed`).
    const bool new_work = c.work != work_;
    if (new_work || c.vertex_changed) {
      const Upload v = core_.Reserve(kVertexBlockBytes, DK_UNIFORM_BUF_ALIGNMENT);
      if (!v) {
        return;
      }
      std::memcpy(v.cpu, c.vertex.data(), kVertexBlockBytes);
      c.vertex_block = v.gpu;
      c.vertex_changed = false;
      ++gaps_.vertex_versions;
    }
    if (new_work || c.pixel_changed) {
      const Upload p = core_.Reserve(kPixelBlockBytes, DK_UNIFORM_BUF_ALIGNMENT);
      if (!p) {
        return;
      }
      std::memcpy(p.cpu, c.pixel.data(), kPixelConstantRegisters * 16);
      std::memset(p.cpu + kPixelConstantRegisters * 16, 0, kPixelBlockBytes - kPixelConstantRegisters * 16);
      c.pixel_block = p.gpu;
      c.pixel_changed = false;
      ++gaps_.pixel_versions;
    }
    c.work = work_;
    c.vertex_dirty.fill(false);
    c.pixel_dirty.fill(false);
    c.any_dirty = false;
  } else if (c.work != work_) {
    // A new work: the blocks start as the banks are now (the GPU has not started this work: CPU writes). The space
    // was checked by the draw, so no slot switch happens here.
    const Upload v = core_.Reserve(kVertexBlockBytes, DK_UNIFORM_BUF_ALIGNMENT);
    const Upload p = core_.Reserve(kPixelBlockBytes, DK_UNIFORM_BUF_ALIGNMENT);
    if (!v || !p) {
      return;
    }
    std::memcpy(v.cpu, c.vertex.data(), kVertexBlockBytes);
    std::memcpy(p.cpu, c.pixel.data(), kPixelConstantRegisters * 16);
    std::memset(p.cpu + kPixelConstantRegisters * 16, 0, kPixelBlockBytes - kPixelConstantRegisters * 16);
    c.vertex_block = v.gpu;
    c.pixel_block = p.gpu;
    c.work = work_;
    c.vertex_dirty.fill(false);
    c.pixel_dirty.fill(false);
    c.any_dirty = false;
  } else if (c.any_dirty) {
    // The registers that changed since, pushed inline in the command list (in order with the draws), in runs.
    auto push = [&](bool* dirty, uint32_t registers, DkGpuAddr block, uint32_t size, const float* bank) {
      for (uint32_t r = 0; r < registers;) {
        if (!dirty[r]) {
          ++r;
          continue;
        }
        uint32_t e = r + 1;
        while (e < registers && dirty[e]) {
          ++e;
        }
        dkCmdBufPushConstants(cmd, block, size, r * 16u, (e - r) * 16u, bank + r * 4);
        std::fill(dirty + r, dirty + e, false);
        ++constant_pushes_;
        r = e;
      }
    };
    push(c.vertex_dirty.data(), kVertexConstantRegisters, c.vertex_block, kVertexBlockBytes, c.vertex.data());
    push(c.pixel_dirty.data(), kPixelConstantRegisters, c.pixel_block, kPixelBlockBytes, c.pixel.data());
    c.any_dirty = false;
  }
  if (vertex_used && e_.ubo[0] != c.vertex_block) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, c.vertex_block, kVertexBlockBytes);
    e_.ubo[0] = c.vertex_block;
  }
  if (pixel_used && e_.ubo[1] != c.pixel_block) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, c.pixel_block, kPixelBlockBytes);
    e_.ubo[1] = c.pixel_block;
  }
}

void DkRecorder::Draw(const RecorderDraw& d) {
  ++draws_;
  ApplyConstants(d);  // also for the draws skipped below: the banks stay in step with the front end
  const uint64_t start_ns = NowNs();
  const int stage = options_.stage ? options_.stage() : 2;
  if (stage < 2) {
    DrawRejected(kSkipStage1, "nx_dk_stage 1: draws off");
    return;
  }
  const bool is3d = d.fixed.depth_enable && d.depth_stencil.handle;
  if (is3d && stage < 3) {
    DrawRejected(kSkip3dBeforeStage3, "draws with a depth test before nx_dk_stage 3");
    return;
  }

  // --- Shaders and the specialization value (Gta4NativeGraphicsSystem's GetOrCreatePipeline rule)
  const PackageShader* vs = d.vertex_shader ? package_.Find(d.vertex_shader, false) : nullptr;
  if (!vs) {
    ShaderMissing(d.vertex_shader, false);
    DrawRejected(kSkipNoVertexShader, "vertex shader not in the package");
    return;
  }
  const bool alpha_test = d.fixed.alpha_test_enable && d.fixed.alpha_function != 7;
  const bool coverage = alpha_test || (d.fixed.alpha_to_mask & 0x100);
  const PackageShader* ps = nullptr;
  uint32_t value = 0;
  if (d.pixel_shader) {
    const PackageShader* early = package_.Find(d.pixel_shader, false);
    const PackageShader* late = package_.Find(d.pixel_shader, true);
    if (!early) {
      ShaderMissing(d.pixel_shader, true);
      DrawRejected(kSkipNoPixelShader, "pixel shader not in the package");
      return;
    }
    const bool capable = (early->specialization_mask & kAlphaTestCapability) == kAlphaTestCapability;
    const bool use_late = coverage && capable && late;
    if (coverage && !use_late) {
      DrawRejected(kSkipCoverage, "alpha test or alpha-to-mask on a shader without the late variant (LRA rejects them)");
      return;
    }
    ps = use_late ? late : early;
    if (use_late && alpha_test) {
      value = 0x2 | ((d.fixed.alpha_function & 0x7) << 8);
    }
  }
  const uint32_t vs_value = (vs->specialization_mask & kAlphaTestCapability) == kAlphaTestCapability ? value : 0;
  bool pending = false;
  const DkShader* vs_shader = ShaderFor(*vs, vs_value, pending);
  const DkShader* ps_shader = nullptr;
  if (vs_shader && ps) {
    ps_shader = ShaderFor(*ps, value, pending);
  } else if (vs_shader) {
    ps_shader = shaders_[ShaderKey{0, 0, false}].get();  // no pixel shader: depth only
  }
  if (!vs_shader || !ps_shader) {
    DrawRejected(pending ? kSkipShaderPending : kSkipShaderMissing,
                 pending ? "a shader variant still compiling on the console's uam" : "a shader without DKSH");
    return;
  }

  // --- Vertex input: each input of the vertex shader from the declaration's element at its location
  std::array<DkVtxAttribState, 16> attribs;
  for (DkVtxAttribState& a : attribs) {
    a = Zeroed<DkVtxAttribState>();
    a.isFixed = 1;
    a.size = DkVtxAttribSize_4x32;
    a.type = DkVtxAttribType_Float;
  }
  uint32_t attrib_count = 0;
  struct StreamUse {
    uint32_t stream = 0;
    uint32_t stride = 0;
    InlineVector<std::pair<uint32_t, uint32_t>, 8> fixes;  // (offset in the vertex, fix kind and element type << 8)
  };
  // Run 46: one list for every draw of this (the recorder's) thread, emptied here: no allocation per draw once it has grown.
  static thread_local std::vector<StreamUse> used;
  used.clear();
  const bool up = d.kind == RecorderDraw::Kind::kUp;
  // Run 49: an input without an element reads a record of zeros, per instance, as the Vulkan path's default binding
  // (graphics_system.cpp, kDefaultVertexBinding: GetDefaultVertexFormat over zeroed memory) instead of a deko3d fixed
  // attribute, whose value deko3d does not document. Its buffer comes after the streams.
  bool zero_record = false;
  std::array<bool, 16> zero_input{};
  // Run 49: the inputs that carry the skinning (BLENDWEIGHT at Xenos location 19, BLENDINDICES at 18), for the check below.
  const VertexElement* skin_weights = nullptr;
  const VertexElement* skin_indices = nullptr;
  for (const PackageShader::Input& input : vs->inputs) {
    if (input.uam_location >= 16) {
      continue;
    }
    attrib_count = std::max<uint32_t>(attrib_count, input.uam_location + 1u);
    if (!d.declaration) {
      ++gaps_.missing_inputs;
      zero_input[input.uam_location] = zero_record = true;
      continue;
    }
    const VertexElement* element = nullptr;
    for (const VertexElement& e : d.declaration->elements) {
      if (core::ConvertVertexUsageToLocation(e.usage, e.usage_index) == input.xenos_location) {
        element = &e;
        break;
      }
    }
    if (!element) {
      ++gaps_.missing_inputs;
      // Run 48: which inputs (run 47: 21,892, rising in the cut-scene with the torn characters): the first 32.
      if (missing_inputs_logged_.size() < 32 &&
          missing_inputs_logged_.insert(vs->hash ^ (uint64_t(input.xenos_location) << 56)).second) {
        std::string have;
        for (const VertexElement& e : d.declaration->elements) {
          have += fmt::format("{}u{}.{}@{}:{:06X}", have.empty() ? "" : " ", e.usage, e.usage_index,
                              core::ConvertVertexUsageToLocation(e.usage, e.usage_index), e.type);
        }
        REXLOG_INFO("deko3d: vertex shader {:016X} input at Xenos location {} (uam location {}) has no element; the "
                    "declaration has (usage u index @location:type) {}",
                    vs->hash, input.xenos_location, input.uam_location, have);
      }
      zero_input[input.uam_location] = zero_record = true;
      continue;
    }
    if (input.xenos_location == 19) {
      skin_weights = element;
    } else if (input.xenos_location == 18) {
      skin_indices = element;
    }
    DkVtxAttribState a = Zeroed<DkVtxAttribState>();
    uint32_t fix = 0;
    if (!VertexAttribute(element->type, input.numeric, a, fix)) {
      static std::unordered_set<uint32_t> logged;
      if (logged.insert(element->type).second) {
        REXLOG_WARN("deko3d: vertex element type {:06X} (numeric {}) not supported", element->type, input.numeric);
      }
      DrawRejected(kSkipVertexFormat, "a vertex element type not supported yet");
      return;
    }
    const uint32_t stream = up ? 0 : element->stream;
    uint32_t buffer = 0;
    while (buffer < used.size() && used[buffer].stream != stream) {
      ++buffer;
    }
    if (buffer == used.size()) {
      const uint32_t stride = up ? d.up_stride : (stream < kVertexStreamCount ? d.streams[stream].stride : 0);
      if (!stride || (!up && (stream >= kVertexStreamCount || !d.streams[stream].address))) {
        DrawRejected(kSkipVertexData, "a vertex stream without data or stride");
        return;
      }
      used.push_back({stream, stride, {}});
    }
    a.bufferId = buffer;
    a.isFixed = 0;
    a.offset = element->offset;
    attribs[input.uam_location] = a;
    if (fix) {
      used[buffer].fixes.push_back({element->offset, fix | (element->type << 8)});
    }
  }
  const uint32_t zero_buffer = uint32_t(used.size());
  if (zero_record) {
    for (uint32_t l = 0; l < 16; ++l) {
      if (zero_input[l]) {
        DkVtxAttribState& a = attribs[l];
        a = Zeroed<DkVtxAttribState>();
        a.bufferId = zero_buffer;
        a.isFixed = 0;
        a.offset = 0;
        a.size = DkVtxAttribSize_4x32;
        a.type = DkVtxAttribType_Float;
        ++gaps_.zero_inputs;
      }
    }
  }

  // --- Primitive, index range and vertex range
  DkPrimitive primitive = DkPrimitive_Triangles;
  const bool rect_list = d.primitive_type == 8;
  if (!rect_list && !PrimitiveOf(d.primitive_type, primitive)) {
    DrawRejected(kSkipPrimitive, "a primitive type not supported yet");
    return;
  }
  const bool indexed = d.kind == RecorderDraw::Kind::kIndexed;
  if (rect_list && indexed) {
    DrawRejected(kSkipRectIndexed, "indexed rectangle lists");
    return;
  }
  uint32_t first_vertex = 0, vertex_count = 0;
  const uint8_t* index_source = nullptr;
  const uint8_t* index_guest = nullptr;  // the indices in guest memory (the submit hash is checked against them)
  uint32_t index_min = 0;
  DkGpuAddr index_gpu = 0;  // run 53: the indices bound from the geometry store
  GeometryStore::Entry* index_entry = nullptr;
  // Run 49 (native_index_policy.h): Xenos's 24-bit indices and its strip-only restart, as the Vulkan path; the scan and the
  // conversion are one pass into index_scratch_, which the upload below copies into the ring.
  const IndexPolicy index_policy =
      IndexPolicyFor(d.primitive_type, indexed && d.primitive_restart, d.primitive_restart_index, d.index_32bit);
  const RecorderDrawData* copied = up ? nullptr : d.data.get();  // run 49: the bytes the front end copied at submit
  if (indexed) {
    const uint32_t bytes = d.index_32bit ? 4 : 2;
    if (!d.index_count || uint64_t(d.start_index + d.index_count) * bytes > d.index_size) {
      DrawRejected(kSkipIndexData, "indices outside their buffer");
      return;
    }
    index_guest = memory_->TranslateVirtual<const uint8_t*>(d.index_address);
    if (!index_guest) {
      DrawRejected(kSkipIndexData, "indices outside their buffer");
      return;
    }
    index_guest += size_t(d.start_index) * bytes;
    index_source = copied && copied->index_bytes == uint64_t(d.index_count) * bytes
                       ? copied->base() + copied->index_offset
                       : index_guest;
    const uint64_t scan_start_ns = NowNs();
    const uint64_t index_bytes_total = uint64_t(d.index_count) * bytes;
    IndexConversion conversion;
    bool converted = false;
    // Run 53 (nfsmw-nx's geometry store keeps indices with their vmin/vmax): an index range seen before with the same
    // bytes is bound from the store, with no conversion and no scan; its stored lo/hi give the vertex range. The mode
    // is the format and the restart policy (bit 61: indices); a submit copy is keyed by its guest address and never
    // trusted for the rest of an epoch (bit 63), as for vertices.
    if (geometry_.active() && !ab_store_off_ && index_bytes_total <= UINT32_MAX) {
      const bool index_from_copy = index_source != index_guest;
      const uint64_t key = index_from_copy ? uint64_t(d.index_address) + uint64_t(d.start_index) * bytes
                                           : uint64_t(reinterpret_cast<uintptr_t>(index_source));
      const uint64_t mode = (uint64_t(1) << 61) | uint64_t(d.index_32bit) | (uint64_t(index_policy.restart) << 1) |
                            (uint64_t(index_policy.guest_restart) << 2) | (uint64_t(index_from_copy) << 63);
      const GeometryStore::Result stored = geometry_.Find(
          index_source, key, uint32_t(index_bytes_total), mode, uint32_t(index_bytes_total), [&](uint8_t* target) {
            conversion = ConvertIndices(index_source, d.index_count, d.index_32bit, index_policy, target);
            converted = true;
          });
      if (stored.gpu) {
        index_gpu = stored.gpu;
        index_entry = stored.entry;
        ++index_hits_;
        if (converted) {
          geometry_.SetRange(stored.entry, conversion.lo, conversion.hi);
        } else {
          conversion.lo = stored.lo;
          conversion.hi = stored.hi;
        }
      }
    }
    if (!index_gpu) {
      if (index_scratch_.size() < size_t(index_bytes_total)) {
        index_scratch_.resize(size_t(index_bytes_total));
      }
      conversion = ConvertIndices(index_source, d.index_count, d.index_32bit, index_policy, index_scratch_.data());
      converted = true;
      ++index_copies_;
    }
    if (converted) {
      gaps_.masked_indices += conversion.masked;
    }
    time_index_scan_ns_ += NowNs() - scan_start_ns;
    if (conversion.lo > conversion.hi) {
      return;  // restarts only
    }
    index_min = conversion.lo;
    first_vertex = uint32_t(int64_t(d.base_vertex) + conversion.lo);
    vertex_count = conversion.hi - conversion.lo + 1;
    gaps_.restart_draws += index_policy.restart;
    gaps_.restart_refused += d.primitive_restart && !index_policy.restart;
  } else {
    first_vertex = up ? 0 : d.start_vertex;
    vertex_count = d.vertex_count;
  }
  if (!vertex_count || vertex_count > kMaxVerticesPerDraw) {
    DrawRejected(kSkipTooManyVertices, "no vertices or too many in one draw");
    return;
  }

  // --- Run 49 (native_draw_snapshot.h): the bytes recorded are the bytes the title submitted. A copy is used when its
  // range is this draw's; a sampled draw's bytes are hashed again here, and a difference hands its buffers to the front
  // end, which copies them at submit from then on.
  const DrawVertexRange range{true, first_vertex, vertex_count};
  const bool use_copy = copied && copied->range.first_vertex == first_vertex && copied->range.vertex_count == vertex_count;
  gaps_.data_copy_range_differs += copied && !use_copy;
  gaps_.data_copied += use_copy;
  gaps_.data_copied_reason[std::min<uint32_t>(d.copy_reason, 4)] += use_copy;
  if (d.submit_hashed && !up) {
    std::array<DrawStreamSpan, kVertexStreamCount> spans{};
    for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
      if (!(d.data_streams & (1u << s))) {
        continue;
      }
      const RecorderVertexStream& st = d.streams[s];
      const uint8_t* base = IsPhysicalDataAddress(st.address, st.fetch_address)
                                ? memory_->TranslatePhysical<const uint8_t*>(st.address)
                                : memory_->TranslateVirtual<const uint8_t*>(st.address);
      if (base) {
        spans[s] = {base + st.offset, st.size > st.offset ? uint64_t(st.size - st.offset) : 0, st.stride};
      }
    }
    const uint64_t index_bytes = indexed ? uint64_t(d.index_count) * (d.index_32bit ? 4 : 2) : 0;
    ++gaps_.data_checked;
    if (DrawDataHash(index_guest, index_bytes, spans.data(), d.data_streams, range) != d.submit_hash) {
      ++gaps_.data_changed;
      // Run 59: the pages this draw read are rewritten under waiting draws: from now on the front end copies at submit
      // every draw that reads them (native_draw_snapshot.h VolatilePages), whatever buffer object it comes through.
      VolatilePages& pages = GlobalVolatilePages();
      for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
        if (d.data_streams & (1u << s)) {
          const RecorderVertexStream& st = d.streams[s];
          pages.Mark(IsPhysicalDataAddress(st.address, st.fetch_address),
                     st.address + st.offset + first_vertex * st.stride, uint64_t(vertex_count) * st.stride);
        }
      }
      if (indexed) {
        pages.Mark(false, d.index_address + d.start_index * (d.index_32bit ? 4 : 2), index_bytes);
      }
      // Which part changed, and whether the draw is a character's: skinning inputs, drawn into the G-buffer (three or
      // more colour targets with a depth target).
      const std::array<uint32_t, 4> parts = DrawDataPartHashes(index_guest, index_bytes, spans.data(), d.data_streams, range);
      uint32_t changed_parts = 0;
      for (uint32_t k = 0; k < 4; ++k) {
        if (parts[k] != d.submit_parts[k]) {
          changed_parts |= 1u << k;
          ++gaps_.data_changed_part[k];
        }
      }
      uint32_t colour_targets = 0;
      for (const SurfaceDescriptor& t : d.render_targets) {
        colour_targets += t.handle != 0;
      }
      const bool skinned = skin_weights && skin_indices;
      const bool gbuffer = colour_targets >= 3 && d.depth_stencil.handle;
      gaps_.data_changed_skinned += skinned;
      gaps_.data_changed_gbuffer += gbuffer;
      if ((skinned || gbuffer) && skinned_changed_logged_ < 48) {
        ++skinned_changed_logged_;
        REXLOG_INFO("deko3d: run 59 draw {} read bytes the title changed after submitting it: vertex shader {:016X}{}{}, "
                    "colour target 0 {:08X} ({} targets), depth {:08X}; vertices {}+{}, {} indices; changed: {}{}{}{} "
                    "(stream 0 buffer {:08X} at {:08X}+{:X} stride {}, stream 1 buffer {:08X} at {:08X}+{:X} stride {}); "
                    "its pages are copied at submit from now on",
                    d.sequence, d.vertex_shader, skinned ? " (skinned)" : "", gbuffer ? " (G-buffer)" : "",
                    d.render_targets[0].handle, colour_targets, d.depth_stencil.handle, first_vertex, vertex_count,
                    indexed ? d.index_count : 0, (changed_parts & 1) ? "indices " : "",
                    (changed_parts & 2) ? "stream 0 " : "", (changed_parts & 4) ? "stream 1 " : "",
                    (changed_parts & 8) ? "stream 2" : "", d.streams[0].buffer, d.streams[0].address, d.streams[0].offset,
                    d.streams[0].stride, d.streams[1].buffer, d.streams[1].address, d.streams[1].offset,
                    d.streams[1].stride);
      }
      if (options_.promote_dynamic_buffer) {
        for (uint32_t s = 0; s < kVertexStreamCount; ++s) {
          if (d.data_streams & (1u << s)) {
            options_.promote_dynamic_buffer(d.streams[s].buffer);
          }
        }
        if (indexed) {
          options_.promote_dynamic_buffer(d.index_buffer);
        }
      }
      if (data_changed_logged_ < 16) {
        ++data_changed_logged_;
        REXLOG_INFO("deko3d: draw {} changed between submit and record (vertex shader {:016X}, index buffer {:08X}, "
                    "stream 0 buffer {:08X} at {:08X} stride {}, vertices {}+{}): its buffers are copied at submit from now on",
                    d.sequence, d.vertex_shader, d.index_buffer, d.streams[0].buffer, d.streams[0].address,
                    d.streams[0].stride, first_vertex, vertex_count);
      }
    }
  }

  // --- Textures and samplers, before anything this draw records: an upload may move on to the next work slot.
  // Each stage's slots as its shader declares them (an empty texture of the type for the others).
  struct StageTextures {
    std::array<DkResHandle, 32> handles{};
    uint32_t count = 0;
  };
  bool lod_bias_seen = false;
  // Run 53: each slot's LOD bias for the shared block (GetTextureLodBias(SLOT), byte 752 + 4 * slot), as the Vulkan
  // path writes it (NativeSamplerLodBias: the fetch's signed 10 bits in 1/32 units, clamped to the device's limit,
  // 15 on Maxwell). Through run 52 it was only counted and the shaders read 0.
  std::array<float, 32> lod_bias{};
  auto prepare_textures = [&](const PackageShader* shader, StageTextures& out) {
    if (!shader) {
      return;
    }
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const uint8_t type = shader->texture_types[slot];
      if (!type) {
        continue;
      }
      uint32_t image = EmptyOf(type), sampler = 0;
      if (slot < kTextureStageCount && (d.texture_mask & (1u << slot)) && d.textures[slot].handle) {
        uint32_t tw = 0, th = 0;
        bool point = false;
        // The fetch constant's LOD bias (signed 10 bits at bit 12 of word 4): the shared block does not carry it.
        const int32_t fetch_bias = int32_t(d.textures[slot].fetch[4] << 10) >> 22;
        lod_bias_seen = lod_bias_seen || fetch_bias != 0;
        lod_bias[slot] = NativeSamplerLodBias(fetch_bias, 15.0f);
        image = PrepareTexture(d.textures[slot], type, tw, th, point);
        sampler = SamplerOf(d.textures[slot].fetch, point);
      }
      out.handles[slot] = dkMakeTextureHandle(image, sampler);
      out.count = slot + 1;
    }
  };
  StageTextures vs_textures, ps_textures;
  const uint64_t textures_start_ns = NowNs();
  prepare_textures(vs, vs_textures);
  prepare_textures(ps, ps_textures);
  gaps_.lod_bias_draws += lod_bias_seen;
  time_textures_ns_ += NowNs() - textures_start_ns;

  // --- Upload space for everything this draw needs, all in one slot
  uint32_t needed = 3 * DK_UNIFORM_BUF_ALIGNMENT + kSharedBlockBytes + kVertexBlockBytes + kPixelBlockBytes;
  const uint32_t rect_vertices = rect_list ? vertex_count / 3 * 4 : vertex_count;
  for (const StreamUse& s : used) {
    needed += Align(rect_vertices * s.stride, 16) + 16;
  }
  if (indexed) {
    needed += Align(d.index_count * (d.index_32bit ? 4 : 2), 16) + 16;
  }
  needed += zero_record ? 32 : 0;
  if (needed > (core_.config().upload_mb << 20)) {
    DrawRejected(kSkipUploadFull, "a draw larger than the upload ring");
    return;
  }
  EnsureWork();
  if (core_.UploadFree() < needed) {
    SwitchSlot();
  }

  // --- Render targets: the colour targets the pixel shader writes, and the depth target
  uint32_t color_mask = 0;
  if (ps) {
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      if (d.render_targets[i].handle && (ps->outputs & (1u << i))) {
        color_mask |= 1u << i;
      }
    }
  }
  // Run 55, from banjo-nx / sa2-nx (Renut's renderer, package 28, banjo_renut_tuning.cpp: "a draw that writes no colour
  // into a depth target, without the alpha test or alpha to coverage, whose pixel shader neither kills pixels, writes
  // depth nor exports to memory, is drawn without the shader (the rasterizer writes the same depth and stencil) ...
  // Shadow maps and depth passes: about half the draws. Same image"): GTA IV's shadow maps (depth only) and the stencil
  // passes of its deferred lights (colour writes off) ran their whole pixel shader for nothing. The targets stay as
  // they are (no extra pass); the flags come from the package (gta4_deko3d_runtime.py), never assumed.
  {
    DepthOnlyInputs in;
    in.enabled = options_.depth_only;
    in.has_pixel_shader = ps != nullptr;
    in.coverage = coverage;
    in.depth_target = d.depth_stencil.handle != 0;
    if (ps) {
      in.flags_valid = (ps->flags & PackageShader::kFlagsValid) != 0;
      in.kills = (ps->flags & PackageShader::kFlagKills) != 0;
      in.writes_depth = (ps->flags & PackageShader::kFlagWritesDepth) != 0;
    }
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      if (color_mask & (1u << i)) {
        in.colour_write_masks |= (NativeColorWriteMaskForTarget(d.fixed.color_write_mask, i) & 0xFu) << (4 * i);
      }
    }
    if (IsDepthOnlyDraw(in)) {
      ps_shader = shaders_[ShaderKey{0, 0, false}].get();
      ++depth_only_draws_;
    }
  }
  // Run 56, masseffect-nx's S6 (vertex outputs nobody reads; zero for depth only): every vertex shader of the package
  // writes 18 vec4 outputs, 9 to 18 of them the constant zero, and each written output is stored per vertex and set up
  // per primitive. Without a pixel shader (GTA IV's shadow maps bind none; the depth-only rule above) the position-only
  // variant; with a pixel shader that declares none of the vertex shader's constant-zero outputs, the live-outputs one.
  // Both from the package only (compiled on the PC, their program headers checked there); missing: the full variant.
  if (options_.vs_prune) {
    uint32_t bit = 0;
    if (ps_shader == shaders_[ShaderKey{0, 0, false}].get()) {
      bit = PackageShader::kVariantPositionOnly;
    } else if (ps && (vs->flags & PackageShader::kFlagIoMask) && (ps->flags & PackageShader::kFlagIoMask) &&
               !(ps->io_mask & vs->io_mask)) {
      bit = PackageShader::kVariantLiveOutputs;
    }
    if (bit) {
      if (const DkShader* pruned = PrunedShaderFor(*vs, vs_value | bit)) {
        vs_shader = pruned;
        ++(bit == PackageShader::kVariantPositionOnly ? pruned_position_draws_ : pruned_live_draws_);
      } else {
        ++pruned_missing_draws_;
      }
    } else if (ps) {
      ++pruned_refused_draws_;
    }
  }
  uint32_t width = 0, height = 0;
  // Run 57: a draw that writes no colour (no pixel shader: GTA IV's light-volume stencil passes, shadow casters) keeps
  // the title's colour targets bound when they have the depth target's size, with every colour write masked (the colour
  // write state below is built from color_mask only): the deferred lights' stencil pass and light pass then share one
  // binding instead of switching targets twice a light (run 56: ~36 light passes and ~20 depth-only passes a frame).
  uint32_t bind_mask = color_mask;
  if (!color_mask && options_.keep_targets && d.depth_stencil.handle) {
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      const SurfaceDescriptor& t = d.render_targets[i];
      if (t.handle && t.width == d.depth_stencil.width && t.height == d.depth_stencil.height) {
        bind_mask |= 1u << i;
      }
    }
    kept_target_draws_ += bind_mask != 0;
  }
  if (!BindTargets(d.render_targets, d.depth_stencil, bind_mask, d.depth_stencil.handle != 0, width, height)) {
    DrawRejected(kSkipNoTargets, "no render target");
    return;
  }
  Before3D();
  DkCmdBuf cmd = core_.commands();
  core_.InvalidateCodeIfNeeded();

  // --- Viewport and scissor (the guest's, D3D's convention: positive height, row 0 at the top)
  const float vx = std::bit_cast<float>(d.fixed.viewport_bits[0]);
  const float vy = std::bit_cast<float>(d.fixed.viewport_bits[1]);
  const float vw = std::bit_cast<float>(d.fixed.viewport_bits[2]);
  const float vh = std::bit_cast<float>(d.fixed.viewport_bits[3]);
  if (!(vw > 0.0f) || !(vh > 0.0f) || !std::isfinite(vx) || !std::isfinite(vy)) {
    DrawRejected(kSkipEmptyViewport, "an empty viewport");
    return;
  }
  const DkViewport viewport = {vx, vy, vw, vh, std::clamp(std::bit_cast<float>(d.fixed.viewport_bits[4]), 0.0f, 1.0f),
                               std::clamp(std::bit_cast<float>(d.fixed.viewport_bits[5]), 0.0f, 1.0f)};
  const int32_t sl = std::clamp(d.fixed.scissor[0], 0, int32_t(width));
  const int32_t st = std::clamp(d.fixed.scissor[1], 0, int32_t(height));
  const int32_t sr = std::clamp(d.fixed.scissor[2], sl, int32_t(width));
  const int32_t sb = std::clamp(d.fixed.scissor[3], st, int32_t(height));
  if (sr <= sl || sb <= st) {
    DrawRejected(kSkipEmptyViewport, "an empty scissor");
    return;
  }
  const DkScissor scissor = {uint32_t(sl), uint32_t(st), uint32_t(sr - sl), uint32_t(sb - st)};

  // --- Depth clamp (run 43) and the known differences from the Vulkan path that are still only counted (see Gaps in
  // dk_recorder.h). The Vulkan path clamps depth for the guest's own request and for the two-sided z-fail light volumes
  // of the deferred light setup (graphics_system.cpp, ShouldEnableNativeDepthClampForDraw): such a volume is closed
  // by caps that the near and far planes clip, and a z-fail count needs them. The title's lighting context
  // (IsLocalStencilSetup) is not in this front end; the volume's own state is the test here, and the draws that also carry
  // the light-setup phase marker are counted to say how far the two agree.
  bool depth_clamp = false;
  {
    const core::FixedFunctionState& x = d.fixed;
    gaps_.depth_clamp += x.depth_clamp_enable != 0;
    gaps_.depth_without_test += d.depth_stencil.handle && !x.depth_enable && !x.stencil_enable;
    bool writes = false;
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      writes = writes || ((color_mask & (1u << i)) && NativeColorWriteMaskForTarget(x.color_write_mask, i) != 0);
    }
    NativeStencilVolumeDepthState volume;
    volume.depth_enable = x.depth_enable;
    volume.depth_write_enable = x.depth_write_enable;
    volume.stencil_enable = x.stencil_enable;
    volume.two_sided_stencil = x.two_sided_stencil;
    volume.cull_mode = x.cull_mode;
    volume.active_color_write = writes;
    volume.stencil_depth_fail = x.stencil_depth_fail;
    volume.stencil_pass = x.stencil_pass;
    volume.stencil_write_mask = x.stencil_write_mask;
    volume.ccw_stencil_depth_fail = x.ccw_stencil_depth_fail;
    volume.ccw_stencil_pass = x.ccw_stencil_pass;
    volume.back_stencil_write_mask = x.back_stencil_write_mask;
    const bool zfail_volume = IsNativeTwoSidedZFailLightVolume(volume);
    gaps_.zfail_volumes += zfail_volume;
    gaps_.zfail_in_light_setup += zfail_volume && d.phase == RenderPhase::kLightSetup;
    if (d.depth_stencil.handle) {
      // Which depth surface the draws test against (run 44): per surface handle, draws, z-fail volumes, draws that write
      // depth. Says whether the lights use the surface the scene depth hand-off fills.
      DepthUse& use = depth_uses_[d.depth_stencil.handle];
      ++use.draws;
      use.zfail += zfail_volume;
      use.writes += x.depth_write_enable != 0;
      use.stencil += x.stencil_enable != 0;
      if (d.phase == RenderPhase::kLightsToScreen) {  // the one lighting marker the hooks emit (kLightSetup/kLightDraw are never sent)
        ++use.lights;
        use.lights_zfail += zfail_volume;
      }
      // The distinct depth/stencil states met on each surface (the first 24, once each): what the lights and the forward
      // draws do to the depth buffer (depth function, reference, masks, operations) and with which shaders.
      const uint64_t low = (uint64_t(x.depth_enable & 1) << 31) | (uint64_t(x.depth_function & 7) << 28) |
                           (uint64_t(x.depth_write_enable & 1) << 27) | (uint64_t(x.stencil_enable & 1) << 26) |
                           (uint64_t(x.stencil_function & 7) << 23) | (uint64_t(x.stencil_reference & 0xFF) << 15) |
                           (uint64_t(x.stencil_mask & 0xFF) << 7) | (uint64_t(zfail_volume) << 6) |
                           (uint64_t(x.two_sided_stencil & 1) << 5) | (uint64_t(x.stencil_pass & 7) << 2);
      const uint64_t state_key = (uint64_t(d.depth_stencil.handle) << 32) ^ low ^ (uint64_t(x.stencil_write_mask & 0xFF) << 48) ^
                                 (uint64_t(x.stencil_depth_fail & 7) << 56) ^ (d.pixel_shader * 0x9E3779B97F4A7C15ull);
      if (depth_states_logged_.size() < 24 && depth_states_logged_.insert(state_key).second) {
        REXLOG_INFO("deko3d: depth state on surface {:08X}: depth test {} func {} write {}, stencil {} func {} ref {:02X} "
                    "mask {:02X} write mask {:02X} ops fail/zfail/pass {}/{}/{} two-sided {}, z-fail volume {}, colour "
                    "write {:X}, phase {}, sample type {}, colour target 0 {:08X}, vertex shader {:016X} pixel shader {:016X}",
                    d.depth_stencil.handle, x.depth_enable, x.depth_function, x.depth_write_enable, x.stencil_enable,
                    x.stencil_function, x.stencil_reference, x.stencil_mask, x.stencil_write_mask, x.stencil_fail,
                    x.stencil_depth_fail, x.stencil_pass, x.two_sided_stencil, zfail_volume, x.color_write_mask,
                    uint32_t(d.phase), d.depth_stencil.sample_type, d.render_targets[0].handle, d.vertex_shader, d.pixel_shader);
      }
    }
    depth_clamp = ShouldEnableNativeDepthClampForDraw(x.depth_clamp_enable != 0, true, volume);
    gaps_.depth_clamp_set += depth_clamp;
  }

  // --- Run 60: the characters' vertex shaders. Each skinned G-buffer vertex shader's first state, once; in the A/B's
  // "probe shaders" phase a skinned draw whose shader has a ready probe draws with it (rasterizer on, the probe's
  // storage-buffer writes into a scratch block), so the package's DKSH is the one thing that differs from the baseline.
  bool skinned_gbuffer = false;  // run 61: the draws the "skinned depth always" phase changes
  if (skin_weights && skin_indices && !up && !rect_list) {
    uint32_t colour_targets = 0;
    for (const SurfaceDescriptor& t : d.render_targets) {
      colour_targets += t.handle != 0;
    }
    skinned_gbuffer = colour_targets >= 3 && d.depth_stencil.handle;
    if (colour_targets >= 3 && skinned_state_logged_.size() < 32 && skinned_state_logged_.insert(d.vertex_shader).second) {
      const core::FixedFunctionState& x = d.fixed;
      REXLOG_INFO("deko3d: run 60 skinned G-buffer draw {} vertex shader {:016X} pixel shader {:016X}: {} vertices, {} "
                  "indices, primitive {}; depth test {} func {} write {}, stencil {} func {} ref {:02X} mask {:02X} write "
                  "mask {:02X} pass op {}, cull {}, clip plane {} ({:08X} {:08X} {:08X} {:08X}), alpha test {} func {} "
                  "ref {:.3f}, alpha to mask {:X}, depth bias {}, viewport {}x{} at {},{} depth {:.3f}-{:.3f}, colour "
                  "write {:X}",
                  d.sequence, d.vertex_shader, d.pixel_shader, vertex_count, indexed ? d.index_count : 0,
                  d.primitive_type, x.depth_enable, x.depth_function, x.depth_write_enable, x.stencil_enable,
                  x.stencil_function, x.stencil_reference, x.stencil_mask, x.stencil_write_mask, x.stencil_pass,
                  x.cull_mode, x.user_clip_plane_enable_mask, x.clip_plane_bits[0], x.clip_plane_bits[1],
                  x.clip_plane_bits[2], x.clip_plane_bits[3], x.alpha_test_enable, x.alpha_function,
                  x.alpha_reference, x.alpha_to_mask, x.depth_bias_enable, viewport.width,
                  viewport.height, viewport.x, viewport.y, viewport.near, viewport.far, x.color_write_mask);
    }
    if (ab_probe_shaders_) {
      const auto found = probes_.find(d.vertex_shader ^ (uint64_t(vs_value) * 0x9E3779B97F4A7C15ull));
      const bool ready = found != probes_.end() && found->second.state == Probe::State::kReady &&
                         (64 * found->second.vars.size() + kProbePositions) * 16 <= kProbeScratchBytes;
      if (ready && !probe_scratch_) {
        probe_scratch_ = core_.CreateReadbackBlock(kProbeScratchBytes, "probe shaders scratch (run 60)");
      }
      if (ready && probe_scratch_) {
        vs_shader = &found->second.shader;
        dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, kProbeBinding, dkMemBlockGetGpuAddr(probe_scratch_),
                                  kProbeScratchBytes);
        ++probe_swapped_;
        if (probe_swap_logged_.insert(d.vertex_shader).second) {
          REXLOG_INFO("deko3d: run 60 probe shaders: vertex shader {:016X} variant {} drawn with its probe's shader "
                      "from draw {} on ({} vertices, {} colour targets)",
                      d.vertex_shader, vs_value, d.sequence, vertex_count, colour_targets);
        }
      } else {
        ++probe_unswapped_;
      }
    }
  }

  // --- Shaders
  if (vs_shader != e_.vs || ps_shader != e_.ps) {
    const DkShader* pair[2] = {vs_shader, ps_shader};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    e_.vs = vs_shader;
    e_.ps = ps_shader;
  }

  // --- Fixed state, each block only when it changes
  {
    DkRasterizerState rs = Zeroed<DkRasterizerState>();
    dkRasterizerStateDefaults(&rs);
    const NativeCullRasterState cull = DecodeNativeCullRasterState(d.fixed.cull_mode);
    rs.cullMode = ab_cull_off_                              ? DkFace_None  // run 58: the A/B's "cull off" phase
                  : cull.cull_face == NativeCullFace::kFront ? DkFace_Front
                  : cull.cull_face == NativeCullFace::kBack  ? DkFace_Back
                                                             : DkFace_None;
    rs.frontFace = cull.front_face_clockwise ? DkFrontFace_CW : DkFrontFace_CCW;
    rs.provokingVertex = DkProvokingVertex_First;
    rs.depthClampEnable = depth_clamp ? 1 : 0;
    // Run 55: the title's depth bias at every stage, as the Vulkan path applies it (graphics_system.cpp, vkCmdSetDepthBias:
    // the constant times kD3D10PolygonOffsetFactorFloat24, the slope times xenos::kPolygonOffsetScaleSubpixelUnit, 1/16).
    // Through run 54 it was off below nx_dk_stage 4 (the runs used 3) and its slope 16 times too large: the shadow maps
    // were drawn without the bias the title asks for (devlog L, item 7; C1).
    const bool bias = (options_.depth_bias || stage >= 4) && d.fixed.depth_bias_enable && d.fixed.depth_bias_representable;
    rs.depthBiasEnableMask = bias ? DkPolygonFlag_All : 0;
    biased_draws_ += bias;
    if (!e_.valid || !Same(rs, e_.raster)) {
      dkCmdBufBindRasterizerState(cmd, &rs);
      e_.raster = rs;
    }
    if (bias) {
      const DepthBiasFactors f = DepthBiasFor(d.fixed.depth_bias_bits, d.fixed.slope_scaled_depth_bias_bits);
      const float b[2] = {f.constant, f.slope};
      if (!e_.valid || std::memcmp(b, e_.depth_bias, sizeof(b)) != 0) {
        dkCmdBufSetDepthBias(cmd, b[0], 0.0f, b[1]);
        std::memcpy(e_.depth_bias, b, sizeof(b));
      }
    }
  }
  {
    DkColorState cs = Zeroed<DkColorState>();
    dkColorStateDefaults(&cs);
    DkColorWriteState cw = Zeroed<DkColorWriteState>();
    std::array<DkBlendState, 4> blends{};
    uint32_t index = 0;
    for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
      if (!(color_mask & (1u << i))) {
        continue;
      }
      const uint32_t mask = NativeColorWriteMaskForTarget(d.fixed.color_write_mask, i);
      const uint32_t packed = d.fixed.blend_controls[i];
      const bool blend = !IsNativeBlendEffectivelyDisabled(packed, mask) && IsNativeBlendControlEnabled(packed);
      dkColorStateSetBlendEnable(&cs, index, blend);
      dkColorWriteStateSetMask(&cw, index, mask);
      DkBlendState& b = blends[index];
      b = Zeroed<DkBlendState>();
      if (blend) {
        const NativeBlendControlState s = DecodeNativeBlendControl(packed);
        b.colorBlendOp = BlendOp(s.color_operation);
        b.srcColorBlendFactor = BlendFactor(s.source_color);
        b.dstColorBlendFactor = BlendFactor(s.destination_color);
        b.alphaBlendOp = BlendOp(s.alpha_operation);
        b.srcAlphaBlendFactor = BlendFactor(s.source_alpha);
        b.dstAlphaBlendFactor = BlendFactor(s.destination_alpha);
      }
      ++index;
    }
    if (!e_.valid || !Same(cs, e_.color)) {
      dkCmdBufBindColorState(cmd, &cs);
      e_.color = cs;
    }
    if (!e_.valid || !Same(cw, e_.masks)) {
      dkCmdBufBindColorWriteState(cmd, &cw);
      e_.masks = cw;
    }
    for (uint32_t i = 0; i < index; ++i) {
      if (((cs.blendEnableMask >> i) & 0x1) && (!e_.valid || !Same(blends[i], e_.blends[i]))) {
        dkCmdBufBindBlendState(cmd, i, &blends[i]);
        ++gaps_.blend_binds;
        // deko3d 0.5.0 wrote the colour destination factor as the alpha one (dk_blend_alpha.h): the right one again.
        if (blends[i].dstAlphaBlendFactor != blends[i].dstColorBlendFactor) {
          ++gaps_.blend_alpha_distinct;
          if (options_.blend_alpha_fix) {
            uint32_t words[2];
            DkBlendAlphaDstWords(i, blends[i].dstAlphaBlendFactor, words);
            dkCmdBufReplayCmds(cmd, words, 2);
          }
        }
        e_.blends[i] = blends[i];
      }
    }
    if (!e_.valid || std::memcmp(d.fixed.blend_constants.data(), e_.blend_constants, 16) != 0) {
      dkCmdBufSetBlendConst(cmd, d.fixed.blend_constants[0], d.fixed.blend_constants[1], d.fixed.blend_constants[2],
                            d.fixed.blend_constants[3]);
      std::memcpy(e_.blend_constants, d.fixed.blend_constants.data(), 16);
    }
  }
  {
    DkDepthStencilState ds = Zeroed<DkDepthStencilState>();
    ds.depthCompareOp = DkCompareOp_Always;
    ds.stencilFrontFailOp = ds.stencilFrontPassOp = ds.stencilFrontDepthFailOp = DkStencilOp_Keep;
    ds.stencilBackFailOp = ds.stencilBackPassOp = ds.stencilBackDepthFailOp = DkStencilOp_Keep;
    ds.stencilFrontCompareOp = ds.stencilBackCompareOp = DkCompareOp_Always;
    if (d.depth_stencil.handle) {
      const core::FixedFunctionState& x = d.fixed;
      ds.depthTestEnable = x.depth_enable != 0;
      ds.depthWriteEnable = x.depth_enable && x.depth_write_enable;
      ds.depthCompareOp = CompareOp(x.depth_function);
      if (ab_skinned_depth_always_ && skinned_gbuffer && ds.depthTestEnable) {
        ds.depthCompareOp = DkCompareOp_Always;  // run 61 A/B: does the depth test reject the characters' fragments?
        ++depth_always_draws_;
      }
      ds.stencilTestEnable = x.stencil_enable != 0;
      ds.stencilFrontFailOp = StencilOp(x.stencil_fail);
      ds.stencilFrontPassOp = StencilOp(x.stencil_pass);
      ds.stencilFrontDepthFailOp = StencilOp(x.stencil_depth_fail);
      ds.stencilFrontCompareOp = CompareOp(x.stencil_function);
      if (x.two_sided_stencil) {
        ds.stencilBackFailOp = StencilOp(x.ccw_stencil_fail);
        ds.stencilBackPassOp = StencilOp(x.ccw_stencil_pass);
        ds.stencilBackDepthFailOp = StencilOp(x.ccw_stencil_depth_fail);
        ds.stencilBackCompareOp = CompareOp(x.ccw_stencil_function);
      } else {
        ds.stencilBackFailOp = ds.stencilFrontFailOp;
        ds.stencilBackPassOp = ds.stencilFrontPassOp;
        ds.stencilBackDepthFailOp = ds.stencilFrontDepthFailOp;
        ds.stencilBackCompareOp = ds.stencilFrontCompareOp;
      }
      if (ds.stencilTestEnable) {
        const uint32_t front = x.stencil_reference | (x.stencil_mask << 8) | (x.stencil_write_mask << 16);
        const uint32_t back = x.two_sided_stencil
                                  ? x.back_stencil_reference | (x.back_stencil_mask << 8) |
                                        (x.back_stencil_write_mask << 16)
                                  : front;
        if (!e_.valid || front != e_.stencil[0] || back != e_.stencil[1]) {
          dkCmdBufSetStencil(cmd, DkFace_Front, uint8_t(front >> 16), uint8_t(front), uint8_t(front >> 8));
          dkCmdBufSetStencil(cmd, DkFace_Back, uint8_t(back >> 16), uint8_t(back), uint8_t(back >> 8));
          e_.stencil[0] = front;
          e_.stencil[1] = back;
        }
      }
    }
    if (!e_.valid || !Same(ds, e_.depth)) {
      dkCmdBufBindDepthStencilState(cmd, &ds);
      e_.depth = ds;
    }
  }
  if (!e_.valid || !Same(viewport, e_.viewport)) {
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    e_.viewport = viewport;
  }
  if (!e_.valid || !Same(scissor, e_.scissor)) {
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    e_.scissor = scissor;
  }
  const bool restart = indexed && index_policy.restart;  // run 49: the converted indices carry the host value (all ones)
  const uint32_t restart_index = restart ? (d.index_32bit ? UINT32_MAX : 0xFFFFu) : 0;
  if (!e_.valid || restart != e_.restart || restart_index != e_.restart_index) {
    dkCmdBufSetPrimitiveRestart(cmd, restart, restart_index);
    e_.restart = restart;
    e_.restart_index = restart_index;
  }
  std::array<DkVtxBufferState, 16> buffer_states{};
  for (size_t b = 0; b < used.size(); ++b) {
    buffer_states[b] = {used[b].stride, 0};
  }
  const size_t buffer_count = used.size() + (zero_record ? 1 : 0);
  if (zero_record) {
    buffer_states[zero_buffer] = {16, 1};  // one record per instance: every vertex reads the same zeros
  }
  const uint64_t attribs_hash = XXH3_64bits(attribs.data(), sizeof(DkVtxAttribState) * attrib_count) ^
                                XXH3_64bits(buffer_states.data(), sizeof(DkVtxBufferState) * buffer_count) ^
                                (uint64_t(attrib_count) << 56) ^ (uint64_t(buffer_count) << 48);
  if (!e_.valid || attribs_hash != e_.attribs_hash) {
    dkCmdBufBindVtxAttribState(cmd, attribs.data(), attrib_count);
    dkCmdBufBindVtxBufferState(cmd, buffer_states.data(), uint32_t(buffer_count));
    e_.attribs_hash = attribs_hash;
  }

  // --- Textures (prepared above) bound as handles
  auto bind_textures = [&](const StageTextures& t, DkStage dk_stage, std::array<DkResHandle, 32>& emitted,
                           uint32_t& emitted_count) {
    if (t.count &&
        (t.count > emitted_count || std::memcmp(t.handles.data(), emitted.data(), t.count * sizeof(DkResHandle)))) {
      dkCmdBufBindTextures(cmd, dk_stage, 0, t.handles.data(), t.count);
      std::copy(t.handles.begin(), t.handles.begin() + t.count, emitted.begin());
      emitted_count = std::max(emitted_count, t.count);
    }
  };
  bind_textures(vs_textures, DkStage_Vertex, e_.vs_textures, e_.vs_texture_count);
  bind_textures(ps_textures, DkStage_Fragment, e_.ps_textures, e_.ps_texture_count);

  // --- Vertices: the draw's range of each stream, converted to host order in the upload ring
  const uint64_t vertices_start_ns = NowNs();
  std::array<DkBufExtents, 16> extents{};
  std::array<const uint8_t*, 16> first_vertex_of{};  // run 49: each buffer's first vertex of the draw (guest order)
  for (size_t b = 0; b < used.size(); ++b) {
    const StreamUse& s = used[b];
    const uint8_t* source = nullptr;
    uint64_t available = 0;
    uint64_t start = 0;
    bool from_copy = false;
    if (up) {
      source = d.up_vertices ? d.up_vertices->data() : nullptr;
      available = d.up_vertices ? d.up_vertices->size() : 0;
    } else if (use_copy && copied->stream_bytes[s.stream] == uint64_t(vertex_count) * s.stride) {
      // Run 49: the range the front end copied at submit (it starts at first_vertex).
      source = copied->base() + copied->stream_offset[s.stream];
      available = copied->stream_bytes[s.stream];
      from_copy = true;
    } else {
      const RecorderVertexStream& stream = d.streams[s.stream];
      source = IsPhysicalDataAddress(stream.address, stream.fetch_address)
                   ? memory_->TranslatePhysical<const uint8_t*>(stream.address)
                   : memory_->TranslateVirtual<const uint8_t*>(stream.address);
      available = stream.size > stream.offset ? stream.size - stream.offset : 0;
      source = source ? source + stream.offset : nullptr;
      start = uint64_t(first_vertex) * s.stride;
    }
    const uint64_t bytes = uint64_t(vertex_count) * s.stride;
    if (!source || start + bytes > available) {
      DrawRejected(kSkipVertexData, "vertices outside their buffer");
      return;
    }
    const uint32_t out_bytes = uint32_t(rect_list ? uint64_t(vertex_count / 3) * 4 * s.stride : bytes);
    // Run 52: guest ranges (not DrawPrimitiveUP data, not rectangle lists, whose host form is built per draw) go through
    // the geometry store (dk_geometry_store.h, nfsmw-nx's): a range seen before with the same bytes is bound from there,
    // with no copy and no byte swap. The mode is the stride and the fix-ups, which change the host form.
    // Run 53: the front end's submit copies too (run 52: 3.5 M of ~6 M draws came from them and all went to the ring).
    // A copy lives at a new host address every draw, so it is keyed by the guest address of its range, and it is never
    // trusted for the rest of an epoch (mode bit 63): its buffer was copied because the title rewrites it.
    if (!up && !rect_list && geometry_.active() && !ab_store_off_ && bytes <= UINT32_MAX) {
      const uint64_t store_start_ns = NowNs();
      uint64_t mode = uint64_t(s.stride) | (uint64_t(s.fixes.size()) << 32);
      for (const auto& f : s.fixes) {
        mode = (mode ^ (uint64_t(f.first) << 40) ^ f.second) * 0x100000001B3ull;
      }
      const uint8_t* range = source + start;
      uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(range));
      if (from_copy) {
        const RecorderVertexStream& stream = d.streams[s.stream];
        key = uint64_t(stream.address) + stream.offset + uint64_t(first_vertex) * s.stride;
        mode = (mode & ~(uint64_t(3) << 62)) | (uint64_t(1) << 63) |
               (uint64_t(IsPhysicalDataAddress(stream.address, stream.fetch_address)) << 62);
      }
      const GeometryStore::Result stored =
          geometry_.Find(range, key, uint32_t(bytes), mode, uint32_t(bytes), [&](uint8_t* target) {
            if (s.fixes.empty()) {
              CopyGuestWords(target, range, bytes);
              return;
            }
            if (vertex_scratch_.size() < bytes) {
              vertex_scratch_.resize(size_t(bytes));
            }
            CopyGuestWords(vertex_scratch_.data(), range, bytes);
            ApplyVertexFixes(vertex_scratch_.data(), vertex_count, s.stride, {s.fixes.data(), s.fixes.size()});
            std::memcpy(target, vertex_scratch_.data(), size_t(bytes));
          });
      time_store_ns_ += NowNs() - store_start_ns;
      if (stored.gpu) {
        geometry_.Use(stored.entry, work_);
        first_vertex_of[b] = range;
        extents[b] = {stored.gpu, out_bytes};
        ++stream_hits_;
        stream_hit_bytes_ += bytes;
        continue;
      }
    }
    const uint64_t ring_start_ns = NowNs();
    ++stream_copies_;
    stream_copy_bytes_ += bytes;
    struct RingTime {
      uint64_t start;
      uint64_t& total;
      ~RingTime() { total += NowNs() - start; }
    } ring_time{ring_start_ns, time_ring_vertices_ns_};
    const Upload u = core_.Reserve(out_bytes, 16);
    if (!u) {
      DrawRejected(kSkipUploadFull, "no room in the upload ring");
      return;
    }
    first_vertex_of[b] = source + start;
    if (!rect_list && s.fixes.empty()) {
      // Run 49: nothing to fix up, so nothing is read back: swapped straight into the ring (write only), one pass
      // instead of two. Same bytes as the scratch route.
      CopyGuestWords(u.cpu, source + start, bytes);
      ++gaps_.direct_vertex_streams;
      extents[b] = {u.gpu, out_bytes};
      continue;
    }
    // Converted in cached memory and copied into the ring in one sequential pass: the fix-ups below read what they
    // change, and the upload ring is CPU-uncached (a read there is a bus round trip). Same bytes as before.
    if (vertex_scratch_.size() < bytes) {
      vertex_scratch_.resize(size_t(bytes));
    }
    uint8_t* target = vertex_scratch_.data();
    CopyGuestWords(target, source + start, bytes);
    ApplyVertexFixes(target, vertex_count, s.stride, {s.fixes.data(), s.fixes.size()});
    if (rect_list) {
      // A rectangle is three vertices. Run 52, as the Vulkan path (graphics_system.cpp, kRectangleList): the corner is
      // the vertex opposite the longest edge in the position's x/y (stream 0, location 0, float32 with 2+ components),
      // not always v0; the fourth vertex is c1 - c0 + c2 for the float32 elements of the declaration only (every other
      // byte is c2's), drawn as a quad c0 c1 c3 c2. Through run 51 v0 was taken as the corner and every 4-byte word
      // that read as a finite float was extrapolated, colours included: a rectangle whose corner is not v0 came out
      // as the wrong shape (text glyphs, sprites).
      const uint32_t stream = up ? 0 : s.stream;
      int32_t position_offset = -1;
      InlineVector<std::pair<uint32_t, uint32_t>, 16> floats;  // (offset, component count) of the float32 elements
      for (const VertexElement& e : d.declaration->elements) {
        const uint32_t components = core::GetFloat32VertexElementComponentCount(e.type);
        if ((up ? 0u : e.stream) != stream || !components || e.offset + components * 4 > s.stride) {
          continue;
        }
        floats.push_back({e.offset, components});
        if (position_offset < 0 && stream == 0 && components >= 2 &&
            core::ConvertVertexUsageToLocation(e.usage, e.usage_index) == 0) {
          position_offset = int32_t(e.offset);
        }
      }
      // Run 60: which elements of a rectangle list the fourth vertex extrapolates (float32 only) and which it copies from
      // c2 (the HUD text loses letters in every run 58-59 phase): once per vertex shader and stride.
      if (rect_logged_.size() < 24 && rect_logged_.insert(d.vertex_shader ^ (uint64_t(s.stride) << 56)).second) {
        std::string elements;
        for (const VertexElement& e : d.declaration->elements) {
          elements += fmt::format("{}u{}.{}@{}:{:06X}{}", elements.empty() ? "" : " ", e.usage, e.usage_index, e.offset,
                                  e.type, core::GetFloat32VertexElementComponentCount(e.type) ? "(f32)" : "(copied)");
        }
        REXLOG_INFO("deko3d: run 60 rectangle list draw {} vertex shader {:016X} pixel shader {:016X}: stream {} stride {}, "
                    "{} vertices, position {}; elements (usage index @offset:type) {}",
                    d.sequence, d.vertex_shader, d.pixel_shader, stream, s.stride, vertex_count,
                    position_offset >= 0 ? fmt::format("float32 at {}", position_offset) : std::string("none (v0 is the corner)"),
                    elements);
      }
      const uint32_t rects = vertex_count / 3;
      static thread_local std::vector<uint8_t> fourth;
      fourth.resize(s.stride);
      for (uint32_t r = 0; r < rects; ++r) {
        const uint8_t* v0 = target + size_t(r) * 3 * s.stride;
        ExpandRectangle(v0, v0 + s.stride, v0 + 2 * s.stride, s.stride, position_offset, {floats.data(), floats.size()},
                        fourth.data(), u.cpu + size_t(r) * 4 * s.stride);
      }
    } else {
      std::memcpy(u.cpu, target, size_t(bytes));
    }
    extents[b] = {u.gpu, out_bytes};
  }
  if (zero_record) {
    const Upload z = core_.Reserve(16, 16);
    if (!z) {
      DrawRejected(kSkipUploadFull, "no room in the upload ring");
      return;
    }
    std::memset(z.cpu, 0, 16);
    extents[zero_buffer] = {z.gpu, 16};
  }
  time_vertices_ns_ += NowNs() - vertices_start_ns;
  for (size_t b = 0; b < buffer_count; ++b) {
    // One buffer per call (deko3d 0.5.0 reserves 5 words per buffer and writes 6).
    dkCmdBufBindVtxBuffers(cmd, uint32_t(b), &extents[b], 1);
  }
  // Run 49: the skinning inputs of 1 skinned draw in 64, decoded from the first vertex. Weights and indices of four unorm
  // bytes (D3DCOLOR 182886, UBYTE4N 1A2086) are checked without caring for their order: the weights must add up to 1 and
  // every index must stay inside the palette (gBoneMtx at c64, 3 registers a bone, 64 bones). Run 48's pictures show
  // characters and fragment objects in pieces placed by the wrong bone; this says whether the inputs are the cause.
  if (skin_weights && skin_indices && !(d.sequence % 64)) {
    ++gaps_.skin_checked;
    auto at = [&](const VertexElement* e) -> const uint8_t* {
      const uint32_t stream = up ? 0 : e->stream;
      for (size_t b = 0; b < used.size(); ++b) {
        if (used[b].stream == stream && first_vertex_of[b]) {
          return first_vertex_of[b] + e->offset;
        }
      }
      return nullptr;
    };
    const auto unorm4 = [](uint32_t type) { return type == 0x182886 || type == 0x1A2086; };
    const uint8_t* w = at(skin_weights);
    const uint8_t* i = at(skin_indices);
    if (!w || !i || !unorm4(skin_weights->type) || !unorm4(skin_indices->type)) {
      ++gaps_.skin_other_type;
    } else {
      const uint32_t sum = uint32_t(w[0]) + w[1] + w[2] + w[3];
      const bool weight_bad = sum < 242 || sum > 268;  // 0.95-1.05 of 255
      const bool index_bad = i[0] >= 64 || i[1] >= 64 || i[2] >= 64 || i[3] >= 64;
      gaps_.skin_weight_bad += weight_bad;
      gaps_.skin_index_bad += index_bad;
      // Run 53 (dk_skin_check.h): the first vertex skinned on the CPU with this draw's palette, as the GPU is given it.
      const VertexElement* position = nullptr;
      for (const VertexElement& e : d.declaration->elements) {
        if (e.usage == 0 && e.usage_index == 0 && (e.type == 0x2A23B9 || e.type == 0x1A23A6)) {
          position = &e;
          break;
        }
      }
      const uint8_t* p = position ? at(position) : nullptr;
      if (p) {
        const SkinCheck k = CheckSkinnedVertex(constants_[d.device].vertex.data(), p, skin_weights->type, w,
                                               skin_indices->type, i);
        if (k.decoded) {
          ++gaps_.skin_cpu_checked;
          gaps_.skin_cpu_bones += uint64_t(k.bones_used);
          gaps_.skin_cpu_not_rigid += uint64_t(k.bones_not_rigid);
          gaps_.skin_cpu_zero += uint64_t(k.bones_zero);
          gaps_.skin_cpu_far += k.distance > 3.0f;
          const bool odd = k.bones_not_rigid || k.bones_zero || k.distance > 3.0f || k.bones_past_palette;
          if (skin_cpu_logged_ < 16 || (odd && skin_cpu_logged_ < 48)) {
            ++skin_cpu_logged_;
            const float* b0 = constants_[d.device].vertex.data() + (64 + std::min<uint32_t>(k.registers[0], 189)) * 4;
            REXLOG_INFO("deko3d: run 53 CPU skin check, draw {} vertex shader {:016X}: weights {:.3f} {:.3f} {:.3f} "
                        "{:.3f} on registers c{}+64 c{}+64 c{}+64 c{}+64; bones used {}, not rigid {}, all-zero {}, past "
                        "the palette {}; skinned ({:.2f}, {:.2f}, {:.2f}), weighted bone origin ({:.2f}, {:.2f}, {:.2f}), "
                        "distance {:.2f}; first bone rows ({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f}) "
                        "({:.3f} {:.3f} {:.3f} {:.3f}){}",
                        d.sequence, d.vertex_shader, k.weights[0], k.weights[1], k.weights[2], k.weights[3],
                        k.registers[0], k.registers[1], k.registers[2], k.registers[3], k.bones_used, k.bones_not_rigid,
                        k.bones_zero, k.bones_past_palette, k.skinned[0], k.skinned[1], k.skinned[2], k.origin[0],
                        k.origin[1], k.origin[2], k.distance, b0[0], b0[1], b0[2], b0[3], b0[4], b0[5], b0[6], b0[7],
                        b0[8], b0[9], b0[10], b0[11], odd ? " IMPLAUSIBLE" : "");
          }
        }
      }
      const uint64_t key = d.vertex_shader ^ (uint64_t(skin_weights->type) << 40) ^ (uint64_t(skin_indices->type) << 8) ^
                           (uint64_t(weight_bad) << 63) ^ (uint64_t(index_bad) << 62);
      if (skin_logged_.size() < 24 && skin_logged_.insert(key).second) {
        REXLOG_INFO("deko3d: skinned draw {} vertex shader {:016X}: weights type {:06X} at {} = {:02X} {:02X} {:02X} {:02X} "
                    "(sum {}/255{}), indices type {:06X} at {} = {:02X} {:02X} {:02X} {:02X}{}; stride {}",
                    d.sequence, d.vertex_shader, skin_weights->type, skin_weights->offset, w[0], w[1], w[2], w[3], sum,
                    weight_bad ? ", OUT of 0.95-1.05" : "", skin_indices->type, skin_indices->offset, i[0], i[1], i[2],
                    i[3], index_bad ? ", an index PAST the palette" : "",
                    up ? d.up_stride : d.streams[skin_weights->stream].stride);
      }
    }
  }
  if (indexed && index_gpu) {
    // Run 53: from the geometry store (converted once, when the range was admitted).
    geometry_.Use(index_entry, work_);
    dkCmdBufBindIdxBuffer(cmd, d.index_32bit ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, index_gpu);
  } else if (indexed) {
    const uint64_t indices_start_ns = NowNs();
    const uint32_t bytes = d.index_32bit ? 4 : 2;
    const Upload u = core_.Reserve(d.index_count * bytes, 16);
    if (!u) {
      DrawRejected(kSkipUploadFull, "no room in the upload ring");
      return;
    }
    // Converted by the scan above (cached memory), then one copy into the (uncached) ring.
    std::memcpy(u.cpu, index_scratch_.data(), size_t(d.index_count) * bytes);
    dkCmdBufBindIdxBuffer(cmd, d.index_32bit ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, u.gpu);
    time_indices_ns_ += NowNs() - indices_start_ns;
  }

  // --- Constants: the guest banks by differences, and the shared block when it changes
  const uint64_t constants_start_ns = NowNs();
  PushConstants(cmd, d.device, true, ps != nullptr);
  std::array<uint8_t, kSharedBlockBytes> shared{};
  auto put = [&shared](uint32_t at, const void* v, size_t n) { std::memcpy(shared.data() + at, v, n); };
  const uint32_t booleans = PackNativeShaderBooleans(d.vertex_booleans, d.pixel_booleans);
  put(kSharedBooleans, &booleans, 4);
  const float half[2] = {1.0f / float(width), -1.0f / float(height)};
  put(kSharedHalfPixel, half, 8);
  put(kSharedClipPlane, d.fixed.clip_plane_bits.data(), 16);
  const uint8_t clip_enabled = (d.fixed.user_clip_plane_enable_mask & 1u) != 0;
  put(kSharedClipPlaneEnabled, &clip_enabled, 1);
  put(kSharedAlphaThreshold, &d.fixed.alpha_reference, 4);
  put(kSharedAlphaToMask, &d.fixed.alpha_to_mask, 4);
  const uint32_t samples = 1;
  put(kSharedAlphaToMaskSamples, &samples, 4);
  const float fragment_scale[2] = {1.0f, 1.0f};
  put(kSharedFragmentScale, fragment_scale, 8);
  for (uint32_t slot = 0; slot < lod_bias.size() && kSharedLodBias + 4 * slot + 4 <= kSharedBlockBytes; ++slot) {
    put(kSharedLodBias + 4 * slot, &lod_bias[slot], 4);
  }
  if (shared_work_ != work_ || shared != shared_previous_) {
    const Upload u = core_.Reserve(kSharedBlockBytes, DK_UNIFORM_BUF_ALIGNMENT);
    if (!u) {
      DrawRejected(kSkipUploadFull, "no room in the upload ring");
      return;
    }
    std::memcpy(u.cpu, shared.data(), kSharedBlockBytes);
    shared_previous_ = shared;
    shared_gpu_ = u.gpu;
    shared_work_ = work_;
  }
  if (e_.ubo[2] != shared_gpu_) {
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, shared_gpu_, kSharedBlockBytes);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 2, shared_gpu_, kSharedBlockBytes);
    e_.ubo[2] = shared_gpu_;
  }
  e_.valid = true;
  time_constants_ns_ += NowNs() - constants_start_ns;

  // --- The draw
  // Run 62 (dk_stage_counters.h): indexed triangle lists into the G-buffer, the characters' and the scenery's, one in four
  // of each while the work's slot has report room: the five stage counters reset before the draw, reported after it.
  bool stage_sampled = false;
  StageClass stage_class = StageClass::kStaticGbuffer;
  if (indexed && primitive == DkPrimitive_Triangles && d.depth_stencil.handle && stage_layout_.samples_per_slot &&
      stage_slot_samples_.size() == kSlots) {
    uint32_t targets = 0;
    for (const SurfaceDescriptor& t : d.render_targets) {
      targets += t.handle != 0;
    }
    if (targets >= 3) {
      stage_class = skinned_gbuffer ? StageClass::kSkinnedGbuffer : StageClass::kStaticGbuffer;
      std::vector<StageSample>& samples = stage_slot_samples_[work_ % kSlots];
      stage_sampled = stage_sampler_.Take(stage_class, uint32_t(samples.size()), stage_layout_.samples_per_slot);
    }
  }
  static constexpr DkCounter kStageDk[kStageCounters] = {
      DkCounter_InputPrimitives, DkCounter_ClipperInputPrimitives, DkCounter_ClipperOutputPrimitives,
      DkCounter_FragmentShaderInvocations, DkCounter_SamplesPassed};
  if (stage_sampled) {
    for (DkCounter c : kStageDk) {
      dkCmdBufResetCounter(cmd, c);
    }
  }
  if (indexed) {
    dkCmdBufDrawIndexed(cmd, primitive, d.index_count, 1, 0, -int32_t(index_min), 0);
  } else if (rect_list) {
    dkCmdBufDraw(cmd, DkPrimitive_Quads, vertex_count / 3 * 4, 1, 0, 0);
  } else {
    dkCmdBufDraw(cmd, primitive, vertex_count, 1, 0, 0);
  }
  ++draws_drawn_;
  if (stage_sampled) {
    const uint32_t slot = uint32_t(work_ % kSlots);
    std::vector<StageSample>& samples = stage_slot_samples_[slot];
    const uint32_t index = uint32_t(samples.size());
    for (uint32_t c = 0; c < kStageCounters; ++c) {
      dkCmdBufReportCounter(cmd, kStageDk[c], core_.QueriesGpu() + stage_layout_.Offset(slot, index, c));
    }
    StageSample s;
    s.cls = stage_class;
    s.vertex_shader = d.vertex_shader;
    s.expected_primitives = d.index_count / 3;
    samples.push_back(s);
  }
  // --- Run 54: the GPU's own values of a skinned draw's vertex shader (dk_shader_probe.h). The same draw once more with
  // the instrumented shader and the rasterizer off: it writes every top-level variable of main and gl_Position for the
  // first 64 vertices into a read-back block; the inputs the GPU was given are saved with them for probe_compare.py.
  // Nothing reaches the screen, and the draw's own state is bound again right after.
  // Run 55: only meshes of kProbeMinVertices or more (run 54's one probe was a 4-vertex quad with one identity bone, not a
  // character), one per vertex shader, up to nx_dk_skin_diff of them, and with gl_Position of every vertex (up to
  // kProbePositions) and the draw's indices: the PC draws the mesh as the GPU placed it.
  if (options_.skin_diff && skin_weights && skin_indices && !rect_list && vs && !up && vertex_count >= kProbeMinVertices) {
    const uint64_t key = d.vertex_shader ^ (uint64_t(vs_value) * 0x9E3779B97F4A7C15ull);
    auto found = probes_.find(key);
    if (found == probes_.end() && probe_started_ < options_.skin_diff) {
      ++probe_started_;
      Probe& p = probes_[key];
      p.hash = d.vertex_shader;
      p.value = vs_value;
      p.glsl = InstrumentVertexGlsl(package_.VariantText(*vs, vs_value), 64, p.vars, kProbePositions);
      if (p.glsl.empty()) {
        p.state = Probe::State::kFailed;
        REXLOG_WARN("deko3d: run 54 skin probe: vertex shader {:016X} could not be instrumented", d.vertex_shader);
      } else {
        compiler_.Queue(true, p.glsl, [this, key](Compiler::Result result) {
          std::lock_guard<std::mutex> lock(probe_mutex_);
          probe_results_[key] = std::move(result);
        });
        REXLOG_INFO("deko3d: run 54 skin probe: vertex shader {:016X} variant {} instrumented ({} values a vertex), "
                    "compiling", d.vertex_shader, vs_value, p.vars.size());
      }
    } else if (found != probes_.end()) {
      Probe& p = found->second;
      if (p.state == Probe::State::kCompiling) {
        Compiler::Result result;
        bool have = false;
        {
          std::lock_guard<std::mutex> lock(probe_mutex_);
          if (auto r = probe_results_.find(key); r != probe_results_.end()) {
            result = std::move(r->second);
            probe_results_.erase(r);
            have = true;
          }
        }
        if (have) {
          const bool loaded =
              result.ok && core_.LoadShader(p.shader, {result.dksh.data(), result.dksh.size()}, "skin probe");
          p.state = loaded ? Probe::State::kReady : Probe::State::kFailed;
          REXLOG_INFO("deko3d: run 54 skin probe: vertex shader {:016X} compiled in {:.1f} s: {}{}", p.hash,
                      double(result.ns) / 1e9, loaded ? "ready" : "FAILED ", loaded ? "" : result.log);
        }
      }
      if (p.state == Probe::State::kReady && p.captures == 0) {
        ++p.captures;
        const uint32_t vertices = std::min<uint32_t>(vertex_count, 64);
        const uint32_t positions = std::min<uint32_t>(vertex_count, kProbePositions);
        const uint32_t slots = uint32_t(p.vars.size());
        const uint32_t bytes = (64 * slots + kProbePositions) * 16;
        PendingProbe pending;
        pending.block = core_.CreateReadbackBlock((bytes + 0xFFF) & ~0xFFFu, "skin probe read-back");
        if (pending.block) {
          // Unwritten slots read as all ones.
          std::memset(dkMemBlockGetCpuAddr(pending.block), 0xFF, bytes);
          dkMemBlockFlushCpuCache(pending.block, 0, bytes);
          dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, kProbeBinding, dkMemBlockGetGpuAddr(pending.block), bytes);
          DkRasterizerState off = e_.raster;
          off.rasterizerEnable = 0;
          dkCmdBufBindRasterizerState(cmd, &off);
          const DkShader* only[1] = {&p.shader};
          dkCmdBufBindShaders(cmd, DkStageFlag_Vertex, only, 1);
          if (indexed) {
            dkCmdBufDrawIndexed(cmd, primitive, d.index_count, 1, 0, -int32_t(index_min), 0);
          } else {
            dkCmdBufDraw(cmd, primitive, vertex_count, 1, 0, 0);
          }
          const DkShader* pair[2] = {e_.vs, e_.ps};
          dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
          dkCmdBufBindRasterizerState(cmd, &e_.raster);
          // The file: header, the variables, the instrumented GLSL, the attributes, the buffers in the host form the
          // GPU read (the first `vertices` vertices of each), the vertex constants and the shared block as bound.
          std::vector<uint8_t>& f = pending.file;
          auto put = [&f](const void* v, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(v);
            f.insert(f.end(), b, b + n);
          };
          auto u32 = [&put](uint32_t v) { put(&v, 4); };
          auto u64 = [&put](uint64_t v) { put(&v, 8); };
          auto str = [&](const std::string& s) {
            u32(uint32_t(s.size()));
            put(s.data(), s.size());
          };
          u32(0x52503454u);  // "T4PR"
          u32(2);            // run 55: the buffers hold `positions` vertices; positions and the draw follow the values
          u64(p.hash);
          u32(p.value);
          u64(d.sequence);
          u32(vertices);
          u32(slots);
          for (const ProbeVariable& v : p.vars) {
            str(v.type);
            str(v.name);
          }
          str(p.glsl);
          u32(attrib_count);
          for (uint32_t l = 0; l < attrib_count; ++l) {
            const DkVtxAttribState& a = attribs[l];
            u32(a.bufferId);
            u32(a.isFixed);
            u32(a.offset);
            u32(uint32_t(a.size));
            u32(uint32_t(a.type));
            u32(a.isBgra);
          }
          u32(uint32_t(buffer_count));
          for (size_t b = 0; b < buffer_count; ++b) {
            u32(buffer_states[b].stride);
            u32(buffer_states[b].divisor);
            if (zero_record && b == zero_buffer) {
              const std::vector<uint8_t> zeros(16, 0);
              u32(16);
              put(zeros.data(), zeros.size());
              continue;
            }
            const uint32_t stride = used[b].stride;
            std::vector<uint8_t> host(size_t(stride) * positions);
            if (first_vertex_of[b]) {
              CopyGuestWords(host.data(), first_vertex_of[b], host.size());
              ApplyVertexFixes(host.data(), positions, stride, {used[b].fixes.data(), used[b].fixes.size()});
            }
            u32(uint32_t(host.size()));
            put(host.data(), host.size());
          }
          put(constants_[d.device].vertex.data(), kVertexConstantRegisters * 16);
          put(shared_previous_.data(), shared_previous_.size());
          pending.work = work_;
          pending.bytes = vertices * slots * 16;
          // Run 55 (version 2): after the values, the positions of `positions` vertices, then the draw: Xenos primitive
          // type, the indices as gl_VertexID (index - index_min; restarts all ones), the viewport, the target's size,
          // depth test/function/write, cull mode, the pixel shader.
          pending.position_offset = 64 * slots * 16;
          pending.positions = positions;
          std::vector<uint8_t>& t = pending.tail;
          auto tput = [&t](const void* v, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(v);
            t.insert(t.end(), b, b + n);
          };
          auto t32 = [&tput](uint32_t v) { tput(&v, 4); };
          t32(d.primitive_type);
          if (indexed) {
            const uint32_t width_bytes = d.index_32bit ? 4 : 2;
            std::vector<uint8_t> converted(size_t(d.index_count) * width_bytes);
            ConvertIndices(index_source, d.index_count, d.index_32bit, index_policy, converted.data());
            t32(d.index_count);
            for (uint32_t k = 0; k < d.index_count; ++k) {
              uint32_t v = 0;
              std::memcpy(&v, converted.data() + size_t(k) * width_bytes, width_bytes);
              const bool restart = d.index_32bit ? v == 0xFFFFFFFFu : v == 0xFFFFu;
              t32(restart ? 0xFFFFFFFFu : v - index_min);
            }
          } else {
            t32(vertex_count);
            for (uint32_t k = 0; k < vertex_count; ++k) {
              t32(k);
            }
          }
          tput(&viewport, sizeof(float) * 6);
          t32(width);
          t32(height);
          t32(d.fixed.depth_enable);
          t32(d.fixed.depth_function);
          t32(d.fixed.depth_write_enable);
          t32(d.fixed.cull_mode);
          tput(&d.pixel_shader, 8);
          char name[96];
          std::snprintf(name, sizeof(name), "vs_%016" PRIX64 "_%u_%" PRIu64 ".probe", p.hash, p.value, d.sequence);
          pending.name = name;
          pending_probes_.push_back(std::move(pending));
        }
      }
    }
  }
  if (slot_stamps_.size() == kSlots && !slot_stamps_[work_ % kSlots].empty()) {
    Stamp& pass = slot_stamps_[work_ % kSlots].back();  // the pass this draw belongs to
    if (pass.draws++ == 0) {
      pass.vertex_shader = d.vertex_shader;  // run 45: named in the `slow work` line when the pass has one draw
      pass.pixel_shader = d.pixel_shader;
    }
  }
  record_ns_ += NowNs() - start_ns;
}

// ---- Present and the output

void DkRecorder::Present(const RecorderPresent& p) {
  const PresentCommand& c = p.command;
  EnsureWork();
  const uint32_t base = ((c.frontbuffer_fetch[1] >> 12) << 12) & 0x1FFFFFFF;
  DkImageEntry* source = ResolvedTexture(c.frontbuffer_texture, base);
  if (!source) {
    ++presents_without_source_;
  }
  const int image = swapchain_.Acquire();
  Before3D();
  DkCmdBuf cmd = core_.commands();
  DkImageView target;
  dkImageViewDefaults(&target, &swapchain_.Image(image));
  dkCmdBufBindRenderTarget(cmd, &target, nullptr);
  const DkViewport viewport = {0.0f, 0.0f, float(swapchain_.width()), float(swapchain_.height()), 0.0f, 1.0f};
  const DkScissor all = {0, 0, swapchain_.width(), swapchain_.height()};
  dkCmdBufSetViewports(cmd, 0, &viewport, 1);
  dkCmdBufSetScissors(cmd, 0, &all, 1);
  dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
  if (source && output_valid_) {
    const uint32_t width = std::min(c.width ? c.width : source->width, source->width);
    const uint32_t height = std::min(c.height ? c.height : source->height, source->height);
    const float gamma = options_.gamma ? options_.gamma() : 1.0f;
    const float data[4] = {float(width) / float(source->width), float(height) / float(source->height),
                           gamma > 0.0f ? 1.0f / gamma : 1.0f, 0.0f};
    const Upload ubo = core_.Reserve(sizeof(data), DK_UNIFORM_BUF_ALIGNMENT);
    if (ubo) {
      std::memcpy(ubo.cpu, data, sizeof(data));
      // The source's writes (draws, copies) end before it is sampled.
      dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
      DkRasterizerState rs;
      dkRasterizerStateDefaults(&rs);
      rs.cullMode = DkFace_None;
      DkColorState cs;
      dkColorStateDefaults(&cs);
      DkColorWriteState cw;
      dkColorWriteStateDefaults(&cw);
      DkDepthStencilState ds;
      dkDepthStencilStateDefaults(&ds);
      ds.depthTestEnable = false;
      ds.depthWriteEnable = false;
      dkCmdBufBindRasterizerState(cmd, &rs);
      dkCmdBufBindColorState(cmd, &cs);
      dkCmdBufBindColorWriteState(cmd, &cw);
      dkCmdBufBindDepthStencilState(cmd, &ds);
      dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
      dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
      const DkShader* pair[2] = {&output_vs_, &output_ps_};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, ubo.gpu, sizeof(data));
      dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, sizeof(data));
      const uint32_t view = ViewOf(*source, 0x688, kSwizzleRGBA);  // RGBA as stored
      dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(view, 0));
      dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
      ++presents_shown_;
      if (anatomy_frame_) {
        // Run 56: the anatomy frame ends with the image it shows (the title's front buffer, before the output pass).
        AnatomyCapture(*source, 0, 0, "shown");
        anatomy_frame_ = false;
        ++anatomy_taken_;
        REXLOG_INFO("deko3d: run 56 anatomy frame {} done: {} images, the frame stalled {:.0f} ms for the read-backs "
                    "(not a benchmark frame)",
                    frames_.load(), anatomy_images_, double(anatomy_ns_) / 1e6);
        anatomy_ns_ = 0;
      }
      // A capture of the frame every nx_dk_capture_s seconds.
      if (capture_next_ns_ && NowNs() >= capture_next_ns_ && !capture_.pending &&
          source->format == DkImageFormat_RGBA8_Unorm) {
        capture_next_ns_ = NowNs() + uint64_t(options_.capture_seconds) * 1'000'000'000ull;
        Capture(*source, width, height);
        // Run 56: every kAnatomyEvery-th capture is followed by an anatomy frame (nx_dk_anatomy).
        if (options_.anatomy && anatomy_taken_ < kAnatomyFrames && captures_requested_++ % kAnatomyEvery == 0) {
          anatomy_frame_ = true;
          anatomy_images_ = 0;
        }
      }
    }
  }
  ForgetState();
  CloseWork();
  swapchain_.Present(image);
  frames_.fetch_add(1);
  geometry_.NewFrame(frames_.load() + 1);
  WriteCaptures();
  HarvestProbes();
  CollectCompiles();
  ReleaseTexturesIfNeeded();
}

void DkRecorder::Capture(const DkImageEntry& source, uint32_t width, uint32_t height) {
  const uint32_t bytes = width * height * 4;
  if (capture_bytes_ < bytes) {
    capture_bytes_ = (bytes + 0xFFFF) & ~0xFFFFu;
    capture_block_ = core_.CreateReadbackBlock(capture_bytes_, "capture read-back");
    if (!capture_block_) {
      capture_bytes_ = 0;
      return;
    }
  }
  dkMemBlockFlushCpuCache(capture_block_, 0, bytes);  // before the GPU writes it
  BeforeTransfer();
  DkImageView view;
  dkImageViewDefaults(&view, &source.image);
  const DkImageRect rect = {0, 0, 0, width, height, 1};
  const DkCopyBuf to = {dkMemBlockGetGpuAddr(capture_block_), width * 4, width * 4 * height};
  dkCmdBufCopyImageToBuffer(core_.commands(), &view, &rect, &to, 0);
  capture_ = {work_, width, height, true};
}

void DkRecorder::AnatomyCapture(const DkImageEntry& image, uint32_t source, uint32_t destination, const char* kind) {
  AnatomyFormat f = AnatomyFormat::kUnknown;
  switch (image.format) {
    case DkImageFormat_RGBA8_Unorm: f = AnatomyFormat::kRGBA8; break;
    case DkImageFormat_RGB10A2_Unorm: f = AnatomyFormat::kRGB10A2; break;
    case DkImageFormat_RG16_Float: f = AnatomyFormat::kRG16F; break;
    case DkImageFormat_RGBA16_Float: f = AnatomyFormat::kRGBA16F; break;
    case DkImageFormat_R32_Float: f = AnatomyFormat::kR32F; break;
    case DkImageFormat_ZF32_X24S8: f = AnatomyFormat::kZ32FS8; break;
    default: break;
  }
  const uint32_t index = anatomy_images_++;
  const uint64_t frame = frames_.load();
  const uint32_t bpp = AnatomyBytesPerTexel(f);
  const uint64_t bytes = uint64_t(image.width) * image.height * bpp;
  if (!bpp || !image.live || image.layers != 1 || index >= kAnatomyImages || bytes > kAnatomyMaxBytes) {
    REXLOG_INFO("deko3d: run 56 anatomy f{} #{} {} {:08X} -> {:08X}: {}x{} host format {}: not saved (format, layers "
                "or size)",
                frame, index, kind, source, destination, image.width, image.height, int(image.format));
    return;
  }
  const uint64_t t0 = NowNs();
  if (anatomy_bytes_ < bytes) {
    if (anatomy_block_) {
      core_.WaitIdle();
      core_.DestroyReadbackBlock(anatomy_block_);  // run 57: the core forgets it too (run 56's fault at exit)
    }
    anatomy_bytes_ = uint32_t((bytes + 0xFFFF) & ~uint64_t(0xFFFF));
    anatomy_block_ = core_.CreateReadbackBlock(anatomy_bytes_, "anatomy read-back");
    if (!anatomy_block_) {
      anatomy_bytes_ = 0;
      return;
    }
  }
  // As a texture lock: after the work's commands so far, the work submitted and waited for (this frame only).
  dkMemBlockFlushCpuCache(anatomy_block_, 0, uint32_t(bytes));
  EnsureWork();
  BeforeTransfer();
  DkImageView view;
  dkImageViewDefaults(&view, &image.image);
  const DkImageRect rect = {0, 0, 0, image.width, image.height, 1};
  const DkCopyBuf to = {dkMemBlockGetGpuAddr(anatomy_block_), image.width * bpp, image.width * bpp * image.height};
  dkCmdBufCopyImageToBuffer(core_.commands(), &view, &rect, &to, 0);
  CloseWork();
  core_.WaitIdle();
  dkMemBlockFlushCpuCache(anatomy_block_, 0, uint32_t(bytes));
  auto rgb = std::make_shared<std::vector<uint8_t>>();
  const AnatomyStats st = AnatomyToRgb(f, static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(anatomy_block_)),
                                       image.width, image.height, *rgb);
  char path[256];
  std::snprintf(path, sizeof(path), "%s/dk-f%06" PRIu64 "-a%02u-%s-%08X-%08X.png", options_.capture_dir.c_str(), frame,
                index, kind, source, destination);
  REXLOG_INFO("deko3d: run 56 anatomy f{} #{} {} {:08X} -> {:08X}: {}x{} host format {}: {} ({})", frame, index, kind,
              source, destination, image.width, image.height, int(image.format), st.Describe(), path);
  if (anatomy_writer_.joinable()) {
    anatomy_writer_.join();  // one PNG at a time: the SD card is the limit, not this thread
  }
  const std::string file = path;
  const uint32_t w = image.width, h = image.height;
  anatomy_writer_ = std::thread([rgb, file, w, h] {
    if (!WritePng(file.c_str(), rgb->data(), w, h)) {
      REXLOG_WARN("deko3d: anatomy image {} could not be written", file);
    }
  });
  EnsureWork();
  ForgetState();
  anatomy_ns_ += NowNs() - t0;
}

void DkRecorder::HarvestProbes() {
  for (size_t i = 0; i < pending_probes_.size();) {
    PendingProbe& p = pending_probes_[i];
    if (!core_.WorkDone(p.work)) {
      ++i;
      continue;
    }
    const uint32_t position_bytes = p.positions * 16;
    dkMemBlockFlushCpuCache(p.block, 0, std::max(p.bytes, p.position_offset + position_bytes));
    const uint8_t* values = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(p.block));
    p.file.insert(p.file.end(), values, values + p.bytes);
    // Run 55 (version 2): the positions, then the draw.
    const uint32_t count = p.positions;
    p.file.insert(p.file.end(), reinterpret_cast<const uint8_t*>(&count), reinterpret_cast<const uint8_t*>(&count) + 4);
    p.file.insert(p.file.end(), values + p.position_offset, values + p.position_offset + position_bytes);
    p.file.insert(p.file.end(), p.tail.begin(), p.tail.end());
    // Beside the captures folder (sdmc:/switch/theft4/probe), without a ".." in the path.
    const size_t slash = options_.capture_dir.find_last_of('/');
    const std::string dir = (slash == std::string::npos ? std::string("sdmc:/switch/theft4")
                                                        : options_.capture_dir.substr(0, slash)) +
                            "/probe";
    mkdir(dir.c_str(), 0777);
    const std::string path = dir + "/" + p.name;
    bool ok = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      ok = std::fwrite(p.file.data(), 1, p.file.size(), f) == p.file.size();
      std::fclose(f);
    }
    ++probe_files_;
    REXLOG_INFO("deko3d: run 54 skin probe {}: {} ({} bytes): the GPU's values of the draw's vertex shader with its "
                "inputs; on the PC: python tools/deko3d/probe_compare.py <file>",
                ok ? "written" : "NOT written", path, p.file.size());
    pending_probes_.erase(pending_probes_.begin() + std::ptrdiff_t(i));
  }
}

void DkRecorder::WriteCaptures() {
  if (!capture_.pending || !core_.WorkDone(capture_.work)) {
    return;
  }
  capture_.pending = false;
  const uint32_t width = capture_.width, height = capture_.height;
  dkMemBlockFlushCpuCache(capture_block_, 0, width * height * 4);  // what the GPU wrote, not the cache
  const uint8_t* texels = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(capture_block_));
  auto rgb = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 3);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    (*rgb)[i * 3] = texels[i * 4];
    (*rgb)[i * 3 + 1] = texels[i * 4 + 1];
    (*rgb)[i * 3 + 2] = texels[i * 4 + 2];
  }
  char path[256];
  std::snprintf(path, sizeof(path), "%s/dk-f%06" PRIu64 ".png", options_.capture_dir.c_str(), frames_.load());
  // Written on a thread joined before the next one (libnx has no detach): 2.7 MB to the SD card would be a stutter.
  if (capture_writer_.joinable()) {
    capture_writer_.join();
  }
  const std::string file = path;
  capture_writer_ = std::thread([rgb, file, width, height] {
    if (WritePng(file.c_str(), rgb->data(), width, height)) {
      REXLOG_INFO("deko3d: capture {} ({}x{})", file, width, height);
    } else {
      REXLOG_WARN("deko3d: capture {} could not be written", file);
    }
  });
  ++captures_written_;
}

// ---- The report

// Run 58 (dk_ab.h): one report window into the A/B schedule; at a phase's end its totals against the baseline's, then
// the next phase's switches.
void DkRecorder::StepAb(double seconds, uint64_t frames, double gpu_busy_ms, double gpu_gap_ms) {
  const uint64_t materializations = placement_stats_.copies + placement_stats_.conversions;
  AbWindow w;
  w.seconds = seconds;
  w.frames = frames;
  w.draws = draws_ - ab_mark_draws_;
  w.gpu_busy_ms = gpu_busy_ms;
  w.gpu_gap_ms = gpu_gap_ms;
  w.materializations = materializations - ab_mark_materializations_;
  w.target_binds = target_binds_ - ab_mark_binds_;
  w.rechecked = gaps_.data_checked - ab_mark_rechecked_;
  w.changed = gaps_.data_changed - ab_mark_changed_;
  ab_mark_draws_ = draws_;
  ab_mark_materializations_ = materializations;
  ab_mark_binds_ = target_binds_;
  ab_mark_rechecked_ = gaps_.data_checked;
  ab_mark_changed_ = gaps_.data_changed;
  if (!ab_.Step(w)) {
    if (ab_.capture_now() && capture_next_ns_) {
      capture_next_ns_ = NowNs();  // the phase's last window: its picture at the next present
      REXLOG_INFO("deko3d: run 58 A/B: capture of phase \"{}\" (cycle {}) at frame {}", AbPhaseName(ab_.phase()),
                  ab_.cycle(), frames_.load());
    }
    return;
  }
  if (ab_.ended()) {
    const AbTotals& t = ab_.totals(ab_.ended_phase());
    const AbTotals& b = ab_.totals(AbPhase::kBaseline);
    REXLOG_INFO("deko3d: run 58 A/B phase \"{}\" (cycle {}, {} windows counted, all cycles): {:.2f} fps ({:+.1f} % against "
                "the baseline's {:.2f}), {:.0f} draws/s, {:.0f} draws a frame, GPU busy {:.1f} ms a frame, GPU idle {:.1f} ms "
                "a frame, {:.1f} EDRAM materializations a frame, {:.1f} target binds a frame; run 59: sampled draws "
                "drawn from bytes changed since submit {} of {} ({:.1f} %)",
                AbPhaseName(ab_.ended_phase()), ab_.cycle(), t.windows, t.fps(),
                b.fps() > 0 ? (t.fps() / b.fps() - 1.0) * 100.0 : 0.0, b.fps(),
                t.seconds > 0 ? double(t.draws) / t.seconds : 0.0, t.per_frame(double(t.draws)),
                t.per_frame(t.gpu_busy_ms), t.per_frame(t.gpu_gap_ms), t.per_frame(double(t.materializations)),
                t.per_frame(double(t.target_binds)), t.changed, t.rechecked, t.changed_percent());
    const std::string what = fmt::format("phase \"{}\" (all cycles)", AbPhaseName(ab_.ended_phase()));
    LogStageCounters(what.c_str(), stage_phase_[size_t(ab_.ended_phase())]);
  }
  const AbPhase p = ab_.phase();
  const bool placement_was = options_.placement;
  options_.keep_targets = ab_keep_targets_ && p != AbPhase::kKeepTargetsOff;
  options_.placement = ab_placement_ && p != AbPhase::kPlacementOff;
  ab_store_off_ = p == AbPhase::kStoreOff;
  ab_cull_off_ = p == AbPhase::kCullOff;
  ab_probe_shaders_ = p == AbPhase::kProbeShaders;
  ab_skinned_depth_always_ = p == AbPhase::kSkinnedDepthAlways;
  if (options_.submit_copies) {
    options_.submit_copies(p != AbPhase::kCopiesOff);
  }
  if (options_.volatile_pages) {
    options_.volatile_pages(p != AbPhase::kPagesOff);
  }
  if (options_.handle_promotions) {
    options_.handle_promotions(p != AbPhase::kPagesOnly);
  }
  if (options_.placement && !placement_was) {
    placement_.clear();  // nothing was claimed while it was off: the owners known from before are stale
  }
  e_.valid = false;  // the rasterizer state (culling) is emitted again
  REXLOG_INFO("deko3d: run 58 A/B phase \"{}\" begins (cycle {}): keep targets {}, placement {}, geometry store {}, "
              "submit copies {} (run 59: rewritten pages {}, promoted buffers {}), culling {}, skinned vertex shaders {} "
              "(run 60; {} probes ready, {} draws swapped so far), skinned G-buffer depth compare {} (run 61; {} draws "
              "with it always so far)",
              AbPhaseName(p), ab_.cycle(), options_.keep_targets ? "on" : "off", options_.placement ? "on" : "off",
              ab_store_off_ ? "bypassed" : "on", p == AbPhase::kCopiesOff ? "off" : "on",
              p == AbPhase::kPagesOff ? "off" : "on", p == AbPhase::kPagesOnly ? "off" : "on",
              ab_cull_off_ ? "off" : "the title's", ab_probe_shaders_ ? "the probes' (console uam)" : "the package's",
              std::count_if(probes_.begin(), probes_.end(),
                            [](const auto& e) { return e.second.state == Probe::State::kReady; }),
              probe_swapped_, ab_skinned_depth_always_ ? "always" : "the title's", depth_always_draws_);
}

void DkRecorder::Report() {
  const uint64_t now = NowNs();
  const double seconds = std::max(1e-3, double(now - report_ns_) / 1e9);
  const uint64_t frames = frames_.load();
  const double fps = double(frames - report_frames_) / seconds;
  const double per_frame = frames > report_frames_ ? double(draws_ - report_draws_) / double(frames - report_frames_)
                                                    : 0.0;
  std::string skipped;
  for (const auto& [reason, count] : skipped_) {
    skipped += fmt::format(" {}={}", reason, count);
  }
  const CoreStats s = core_.TakeStats();
  const DebugCounters& dbg = GetDebugCounters();
  REXLOG_INFO("deko3d: {:.1f} frames/s, {:.0f} draws per frame; totals: frames {}, draws {} drawn {} (skipped by "
              "reason:{}), clears {}, resolves {} ({} copied), presents shown {} without a source {}; record {:.2f} "
              "us per drawn draw",
              fps, per_frame, frames, draws_, draws_drawn_, skipped.empty() ? " none" : skipped, clears_, resolves_,
              resolves_done_, presents_shown_, presents_without_source_,
              draws_drawn_ ? double(record_ns_) / double(draws_drawn_) / 1e3 : 0.0);
  REXLOG_INFO("deko3d: textures {} live ({} MB), {} created, {} uploads ({} MB); resolved textures {}; shaders: {} "
              "from the PC's DKSH, {} compiled by the console's uam, {} failed, {} compiling; command memory max {} "
              "KB per work, {} works, upload {} MB in the interval ({} full), slot switches {}, barriers {}, "
              "constant pushes {}; images {} MB in {} chunks, shader code {} KB, scratch per warp {} of {} B ({} "
              "refused); deko3d blocks {} ({} MB); libdeko3dd errors {} warnings {}",
              textures_.size(), texture_bytes_ >> 20, textures_created_, textures_uploaded_, bytes_uploaded_ >> 20,
              resolved_.size(), shaders_from_pc_, shaders_compiled_, shaders_failed_, compiling_.size(),
              s.command_bytes_max >> 10, s.works, s.upload_bytes >> 20, s.upload_full, slot_switches_, barriers_,
              constant_pushes_, s.image_bytes >> 20, s.image_chunks, s.code_bytes >> 10, s.scratch_max,
              kQueueScratchBytes, s.scratch_refused, device_.live_blocks(), device_.block_bytes() >> 20,
              dbg.errors.load(), dbg.warnings.load());
  REXLOG_INFO("deko3d: depth clamp (totals): requested by the title {}, z-fail light volumes {} (under the light-setup "
              "phase marker {}), draws drawn with depthClampEnable {}; resolved colour fetches {} (0x60A viewed as stored {})",
              gaps_.depth_clamp, gaps_.zfail_volumes, gaps_.zfail_in_light_setup, gaps_.depth_clamp_set,
              gaps_.resolved_colour_fetches, gaps_.resolved_colour_stored);
  REXLOG_INFO("deko3d: known gaps seen (counted, nothing done; totals): depth clamp {}, z-fail light volumes {}, "
              "depth bound without a test {}, target binds of unequal size {}, vertex inputs without an element {}, "
              "draws with a LOD bias {}, textures found by base address {} (of which the wrong type {}), resolves "
              "with a colour exponent {}, resolve clears of the other aspect {}; waiting for slot fences {} ms",
              gaps_.depth_clamp, gaps_.zfail_volumes, gaps_.depth_without_test, gaps_.extent_mismatch,
              gaps_.missing_inputs, gaps_.lod_bias_draws, gaps_.base_lookups, gaps_.base_type_mismatch,
              gaps_.resolve_exponent, gaps_.resolve_cross_clear, core_.NsWaitingForFences() / 1'000'000);
  REXLOG_INFO("deko3d: run 48 (totals): blend states bound {}, of which with an alpha destination factor other than the "
              "colour one {} ({}); colour fetches at a resolved depth's address read from guest memory {} ({}); 3D "
              "texture fetches (empty texture) {}; image descriptors {} handed out of {}, {} free, {} textures "
              "released for descriptors",
              gaps_.blend_binds, gaps_.blend_alpha_distinct,
              options_.blend_alpha_fix ? "alpha factor written again" : "left as deko3d wrote it",
              gaps_.mixup_from_memory, options_.mixup_from_memory ? "on" : "off: served the depth", gaps_.volume_fetches,
              next_descriptor_, core_.config().image_descriptors, free_descriptors_.size(), gaps_.descriptor_evictions);
  REXLOG_INFO("deko3d: run 49 (totals): draws from bytes copied at submit {} ({} copies not used: other range); sampled "
              "draws checked again {}, changed since submit {}; inputs read from the zero record {}; 32-bit indices cut to "
              "24 bits {}; restart on {} draws, refused {} (a list or a wide 16-bit value); vertex streams copied straight "
              "into the ring {}; skinned draws checked {}: weight sum off {}, bone index past the palette {}, types not "
              "decoded {}",
              gaps_.data_copied, gaps_.data_copy_range_differs, gaps_.data_checked, gaps_.data_changed, gaps_.zero_inputs,
              gaps_.masked_indices, gaps_.restart_draws, gaps_.restart_refused, gaps_.direct_vertex_streams,
              gaps_.skin_checked, gaps_.skin_weight_bad, gaps_.skin_index_bad, gaps_.skin_other_type);
  REXLOG_INFO("deko3d: run 59 submit race (totals): changed since submit {} of {} sampled draws re-checked; by part: "
              "indices {}, stream 0 {}, stream 1 {}, stream 2 {}; with skinning inputs {}, into the G-buffer {}; pages "
              "rewritten under a waiting draw {} (64 KiB each); draws recorded from copies at submit: locked {}, rewritten "
              "after the first fill {}, promoted buffer {}, rewritten pages {}",
              gaps_.data_changed, gaps_.data_checked, gaps_.data_changed_part[0], gaps_.data_changed_part[1],
              gaps_.data_changed_part[2], gaps_.data_changed_part[3], gaps_.data_changed_skinned,
              gaps_.data_changed_gbuffer, GlobalVolatilePages().marked(), gaps_.data_copied_reason[1],
              gaps_.data_copied_reason[2], gaps_.data_copied_reason[3], gaps_.data_copied_reason[4]);
  REXLOG_INFO("deko3d: run 60 probe shaders (totals): skinned draws drawn with their probe's shader {}, skinned draws "
              "in that phase without a ready probe {}; probes started {}, ready {}",
              probe_swapped_, probe_unswapped_, probe_started_,
              std::count_if(probes_.begin(), probes_.end(),
                            [](const auto& e) { return e.second.state == Probe::State::kReady; }));
  // Run 62: the stage counters over the whole session every window; the characters' vertex shaders one by one every
  // sixth window (once a minute).
  LogStageCounters("totals", stage_totals_);
  if (++stage_report_windows_ % 6 == 0) {
    for (const auto& [vs, t] : stage_totals_.shaders()) {
      REXLOG_INFO("deko3d: run 62 stage counters, totals, skinned vertex shader {:016X}: {} draws ({} short at "
                  "assembly), triangles expected {}; input {:.3f}, clipper input {:.3f}, clipper output {:.3f}, {:.1f} "
                  "fragments a triangle, {:.3f} samples passed a fragment",
                  vs, t.draws, t.assembly_short, t.expected, t.assembled(), t.reached_clipper(), t.left_clipper(),
                  t.fragments_per_triangle(), t.samples_per_fragment());
    }
  }
  REXLOG_INFO("deko3d: run 50 (totals): constant banks checked against the front end's {}, different: vertex c0-c63 "
              "{}, bone palette c64-c255 {}, pixel {}; constant blocks {} (vertex versions {}, pixel versions {}; inline "
              "pushes {})",
              gaps_.constants_checked, gaps_.constants_differ[0], gaps_.constants_differ[1], gaps_.constants_differ[2],
              options_.constant_versions ? "versioned" : "pushed inline", gaps_.vertex_versions, gaps_.pixel_versions,
              constant_pushes_);
  REXLOG_INFO("deko3d: run 55 draws in the interval: {} without their pixel shader (depth only: no colour written, no "
              "discard, no depth export; nx_dk_depth_only {}), {} with the title's depth bias (nx_dk_depth_bias {}); "
              "totals {} and {}",
              depth_only_draws_ - report_depth_only_, options_.depth_only ? "on" : "off", biased_draws_ - report_biased_,
              options_.depth_bias ? "on" : "off", depth_only_draws_, biased_draws_);
  report_depth_only_ = depth_only_draws_;
  report_biased_ = biased_draws_;
  {
    const uint64_t now[4] = {pruned_position_draws_, pruned_live_draws_, pruned_missing_draws_, pruned_refused_draws_};
    REXLOG_INFO("deko3d: run 56 vertex output pruning in the interval (nx_dk_vs_prune {}): {} draws with the "
                "position-only vertex shader, {} with the live-outputs one, {} without a pruned variant in the package, {} "
                "kept whole (the pixel shader declares a constant-zero output); totals {} / {} / {} / {}, {} variants loaded",
                options_.vs_prune ? "on" : "off", now[0] - report_pruned_[0], now[1] - report_pruned_[1],
                now[2] - report_pruned_[2], now[3] - report_pruned_[3], now[0], now[1], now[2], now[3], pruned_loaded_);
    std::copy(std::begin(now), std::end(now), report_pruned_);
  }
  REXLOG_INFO("deko3d: run 57 EDRAM placements in the interval (nx_dk_placement {}): {} materialized by an image copy, "
              "{} by the conversion pass, {} refused, {} owners gone; {} placements known, conversion pass {}",
              options_.placement ? "on" : "off", placement_stats_.copies - placement_reported_.copies,
              placement_stats_.conversions - placement_reported_.conversions,
              placement_stats_.refused - placement_reported_.refused, placement_stats_.stale - placement_reported_.stale,
              placement_.size(), placement_valid_ ? "compiled" : "NOT compiled");
  placement_reported_ = placement_stats_;
  REXLOG_INFO("deko3d: run 57 target binds in the interval: {} (render target changes), {} draws without colour output "
              "kept the title's colour targets bound (nx_dk_keep_targets {})",
              target_binds_ - report_target_binds_, kept_target_draws_ - report_kept_targets_,
              options_.keep_targets ? "on" : "off");
  report_target_binds_ = target_binds_;
  report_kept_targets_ = kept_target_draws_;
  REXLOG_INFO("deko3d: run 54 texture locks in the recorder (totals): {} locks, {} not GPU-produced (guest copy kept), "
              "{} read back ({} KiB), {} refused (level/array/depth), {} layout mismatches; {:.1f} ms in all",
              lock_stats_.locks, lock_stats_.not_gpu, lock_stats_.copied, lock_stats_.bytes >> 10, lock_stats_.unsupported,
              lock_stats_.layout_mismatch, double(lock_stats_.ns) / 1e6);
  REXLOG_INFO("deko3d: run 53 CPU skin check (totals): {} skinned draws checked, {} bones used, {} not rigid, {} with "
              "all-zero rows, {} vertices more than 3 m from their bones",
              gaps_.skin_cpu_checked, gaps_.skin_cpu_bones, gaps_.skin_cpu_not_rigid, gaps_.skin_cpu_zero,
              gaps_.skin_cpu_far);
  {
    const GeometryStore::Stats& g = geometry_.stats();
    REXLOG_INFO("deko3d: run 52 geometry store (totals): lookups {}, hits {} ({} MiB not copied), admitted {} ({} MiB), "
                "changed {}, volatile {}, evictions {}, no room {}, collisions {}; hashed {} MiB; {} of {} MiB used, "
                "{} ranges; hits without a hash (trusted in their epoch, run 53) {}",
                g.lookups, g.hits, g.hit_bytes >> 20, g.admitted, g.admitted_bytes >> 20, g.changes, g.volatile_lookups,
                g.evictions, g.no_room, g.collisions, g.hashed_bytes >> 20, geometry_.used_bytes() >> 20,
                geometry_.total_bytes() >> 20, geometry_.entries(), g.trusted_hits);
  }
  REXLOG_INFO("deko3d: packed depth alias (A8R8G8B8 fetches of a resolved depth; totals): pass {}, {} conversions, {} "
              "fetches served by one already converted, {} failed, {} other depth/colour mix-ups, {} aliases live; "
              "title registrations {} (fetches served through one {}, before the depth was resolved {}, not A8R8G8B8 {})",
              alias_valid_ ? "compiled" : "NOT available", alias_stats_.converted, alias_stats_.reused,
              alias_stats_.failed, alias_stats_.other_mismatch, aliases_.size(), alias_stats_.registered,
              alias_stats_.by_registration, alias_stats_.source_missing, alias_stats_.other_format);
  REXLOG_INFO("deko3d: depth/stencil clears of colour resolves (flags bit 9; totals): {} done, {} without a depth surface, "
              "{} refused (clear-format override); {} resolves in total",
              resolve_clears_.depth_done, resolve_clears_.depth_missing, resolve_clears_.override_skipped, resolves_);
  REXLOG_INFO("deko3d: scene depth hand-off (totals): pass {}, {} commands, {} done, rejected by verdict (policy {}, pass "
              "{}, surface {}, same image {}, source unwritten {}, extent {}, encoding {})",
              handoff_valid_ ? "compiled" : "NOT available", handoff_.commands, handoff_.done, handoff_.rejected[1],
              handoff_.rejected[2], handoff_.rejected[3], handoff_.rejected[4], handoff_.rejected[5],
              handoff_.rejected[6], handoff_.rejected[7]);
  {
    std::string uses;
    for (const auto& [handle, u] : depth_uses_) {
      uses += fmt::format("{}{:08X}{}: {} draws, {} z-fail volumes, {} write depth, {} test stencil, {} under the lights marker ({} z-fail)", uses.empty() ? "" : "; ",
                          handle, handle == handoff_destination_ ? " (the hand-off's)" : "", u.draws, u.zfail, u.writes,
                          u.stencil, u.lights, u.lights_zfail);
    }
    REXLOG_INFO("deko3d: draws by depth surface (totals): {}", uses.empty() ? "none" : uses);
  }
  {
    // GPU time from the timestamps of the works that finished in the interval (StampPass / HarvestStamps).
    std::vector<size_t> order(labels_.size());
    for (size_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return labels_[a].ns > labels_[b].ns; });
    std::string top;
    for (size_t k = 0; k < order.size() && k < 6 && labels_[order[k]].ns; ++k) {
      const PassLabel& l = labels_[order[k]];
      top += fmt::format("{}{}: {:.0f} ms ({} passes, {} draws)", k ? "; " : "", l.text, double(l.ns) / 1e6, l.passes,
                         l.draws);
    }
    REXLOG_INFO("deko3d: GPU time from timestamps in the last {:.0f} s: {} works finished, busy {:.0f} ms, idle or "
                "uploads between works {:.0f} ms, longest work {:.0f} ms; most of it in: {}",
                seconds, gpu_.works, double(gpu_.busy_ns) / 1e6, double(gpu_.gap_ns) / 1e6,
                double(gpu_.max_work_ns) / 1e6, top.empty() ? "nothing yet" : top);
    // Run 55: per target of the list above, its costliest single pass and the shaders of that pass's first draw (the
    // deferred lights' passes: which pixel shader, how many draws).
    std::string costliest;
    for (size_t k = 0; k < order.size() && k < 6 && labels_[order[k]].ns; ++k) {
      const PassLabel& l = labels_[order[k]];
      costliest += fmt::format("{}{}: {:.2f} ms, {} draws, vertex shader {:016X} pixel shader {:016X}", k ? "; " : "",
                               l.text, double(l.max_ns) / 1e6, l.max_draws, l.max_vs, l.max_ps);
    }
    if (!costliest.empty()) {
      REXLOG_INFO("deko3d: run 55 costliest single pass per target in the last {:.0f} s: {}", seconds, costliest);
    }
    for (PassLabel& l : labels_) {
      l.ns = l.passes = l.draws = 0;
      l.max_ns = 0;
      l.max_draws = 0;
      l.max_vs = l.max_ps = 0;
    }
    if (options_.ab_phases) {
      StepAb(seconds, frames - report_frames_, double(gpu_.busy_ns) / 1e6, double(gpu_.gap_ns) / 1e6);
    }
    gpu_.busy_ns = gpu_.gap_ns = gpu_.works = gpu_.max_work_ns = 0;
    gpu_.slow_logged = 0;
  }
  // Where the recorder's time went in the interval, per drawn draw.
  if (draws_drawn_ > report_drawn_) {
    const double n = double(draws_drawn_ - report_drawn_);
    const double record = double(record_ns_ - report_record_ns_) / n / 1e3;
    const double textures = double(time_textures_ns_ - report_textures_ns_) / n / 1e3;
    const double vertices = double(time_vertices_ns_ - report_vertices_ns_) / n / 1e3;
    const double constants = double(time_constants_ns_ - report_constants_ns_) / n / 1e3;
    const double indices = double(time_indices_ns_ - report_indices_ns_) / n / 1e3;
    const double scan = double(time_index_scan_ns_ - report_index_scan_ns_) / n / 1e3;
    REXLOG_INFO("deko3d: recorder time per drawn draw in the interval ({} draws): {:.1f} us = textures {:.1f} + "
                "vertices {:.1f} + indices {:.1f} + index range scan {:.1f} + constants {:.1f} + the rest {:.1f} "
                "(the rest includes the waits for a free slot)",
                uint64_t(n), record, textures, vertices, indices, scan, constants,
                record - textures - vertices - indices - scan - constants);
    const uint64_t fence_ns = core_.NsWaitingForFences();
    const uint64_t hits = stream_hits_ - report_stream_hits_, copies = stream_copies_ - report_stream_copies_;
    REXLOG_INFO("deko3d: run 53 vertices per drawn draw: geometry store {:.1f} us ({} streams bound from it, {} MiB "
                "not copied), ring copies {:.1f} us ({} streams, {} MiB, {:.0f} MB/s); waits for a free work slot "
                "{:.1f} us; index ranges from the store {}, converted into the ring {}",
                double(time_store_ns_ - report_store_ns_) / n / 1e3, hits,
                (stream_hit_bytes_ - report_stream_hit_bytes_) >> 20,
                double(time_ring_vertices_ns_ - report_ring_vertices_ns_) / n / 1e3, copies,
                (stream_copy_bytes_ - report_stream_copy_bytes_) >> 20,
                time_ring_vertices_ns_ > report_ring_vertices_ns_
                    ? double(stream_copy_bytes_ - report_stream_copy_bytes_) * 1e3 /
                          double(time_ring_vertices_ns_ - report_ring_vertices_ns_)
                    : 0.0,
                double(fence_ns - report_fence_ns_) / n / 1e3, index_hits_ - report_index_hits_,
                index_copies_ - report_index_copies_);
    report_fence_ns_ = fence_ns;
    report_index_hits_ = index_hits_;
    report_index_copies_ = index_copies_;
  }
  report_store_ns_ = time_store_ns_;
  report_ring_vertices_ns_ = time_ring_vertices_ns_;
  report_stream_hits_ = stream_hits_;
  report_stream_hit_bytes_ = stream_hit_bytes_;
  report_stream_copies_ = stream_copies_;
  report_stream_copy_bytes_ = stream_copy_bytes_;
  report_record_ns_ = record_ns_;
  report_textures_ns_ = time_textures_ns_;
  report_vertices_ns_ = time_vertices_ns_;
  report_constants_ns_ = time_constants_ns_;
  report_indices_ns_ = time_indices_ns_;
  report_index_scan_ns_ = time_index_scan_ns_;
  report_drawn_ = draws_drawn_;
  report_ns_ = now;
  report_frames_ = frames;
  report_draws_ = draws_;
}

}  // namespace rex::graphics::gta4_native::dk
