// gta4_native deko3d back end: the recorder. See dk_recorder.h.

#include "dk_recorder.h"

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
#include "../native_fixed_function_policy.h"
#include "../native_shader_booleans.h"
#include "dk_png.h"
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
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t v = (guest >> (i * 3)) & 0x7;
    if (v <= 3) {
      out[i] = DkImageSwizzle(DkImageSwizzle_Red + ((host >> (v * 3)) & 0x7));
    } else if (v == 4) {
      out[i] = DkImageSwizzle_Zero;
    } else {
      out[i] = DkImageSwizzle_One;
    }
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

uint32_t ElementBytes(uint32_t type) {
  switch (type) {
    case 0x2C23A5: return 8;
    case 0x2A23B9: return 12;
    case 0x1A23A6: return 16;
    case 0x1A235A:
    case 0x1A215A:
    case 0x1A205A:
    case 0x1A2360: return 8;
    default: return 4;
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
    : device_(device), memory_(memory), package_(package), compiler_(compiler), options_(std::move(options)) {}

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
  swapchain_.Destroy();
  shaders_.clear();
  core_.Destroy();
}

bool DkRecorder::Initialize() {
  CoreConfig config;
  config.slots = kSlots;
  config.upload_mb = 32;
  config.command_kb = 2048;
  config.pre_kb = 512;
  config.code_mb = 48;
  config.image_chunk_mb = 32;
  config.image_descriptors = 8192;
  config.sampler_descriptors = 1024;
  if (!core_.Create(device_, config)) {
    return false;
  }
  descriptors_to_free_.resize(kSlots);
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
  return true;
}

// ---- Works and barriers

void DkRecorder::EnsureWork() {
  if (core_.WorkOpen()) {
    return;
  }
  core_.BeginWork(++work_);
  // The descriptors freed the last time this slot was recorded: its fence has just been waited for.
  auto& freed = descriptors_to_free_[work_ % kSlots];
  free_descriptors_.insert(free_descriptors_.end(), freed.begin(), freed.end());
  freed.clear();
  BeginWorkState();
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
  Before3D();
  DkImageView views[kRenderTargetCount];
  const DkImageView* bound[kRenderTargetCount] = {};
  uint32_t count = 0;
  width = UINT32_MAX;
  height = UINT32_MAX;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i) {
    if (!images[i]) {
      continue;
    }
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
  dkCmdBufBindRenderTargets(core_.commands(), bound, count, depth_image ? &depth_view : nullptr);
  e_.valid = false;  // the scissor and viewport of the new targets are emitted again
  e_.targets = key;
  e_.targets_valid = true;
  e_.width = width;
  e_.height = height;
  return true;
}

DkImageEntry* DkRecorder::ResolvedTexture(uint32_t handle, uint32_t base) {
  if (auto it = resolved_.find(handle); handle && it != resolved_.end() && it->second.written) {
    return &it->second.image;
  }
  if (auto b = resolved_by_base_.find(base & 0x1FFFF000u); base && b != resolved_by_base_.end()) {
    if (auto it = resolved_.find(b->second); it != resolved_.end() && it->second.written) {
      return &it->second.image;
    }
  }
  return nullptr;
}

// ---- Clears

void DkRecorder::Clear(const RecorderClear& c) {
  ++clears_;
  EnsureWork();
  const uint32_t colors = c.flags & 0xF;
  const bool depth = (c.flags & 0x30) != 0;
  uint32_t width = 0, height = 0;
  if (!BindTargets(c.render_targets, c.depth_stencil, colors, depth, width, height)) {
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
  ++resolves_;
  const ResolveCommand& c = r.command;
  EnsureWork();
  const bool depth = (c.flags & 7) == 4;
  DkImageEntry* source = Surface(c.source, depth);
  if (!source) {
    return;
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
    ++resolves_done_;
  }

  // The clears that come with the resolve (bits 8 and 9), on the source surface.
  const bool clear_color = (c.flags & (1u << 8)) != 0;
  const bool clear_depth = (c.flags & (1u << 9)) != 0;
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
    ++texture_generation_;
  }
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

uint32_t DkRecorder::ViewOf(const DkImageEntry& image, uint32_t guest_swizzle, uint16_t host_swizzle) {
  const uint64_t key = (image.serial << 24) ^ (uint64_t(guest_swizzle & 0xFFF) << 12) ^ host_swizzle;
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
    v.dsSource = DkDsSource_Depth;
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
    if (DkImageEntry* resolved = ResolvedTexture(t.handle, base & 0x1FFFFFFF)) {
      width_out = resolved->width;
      height_out = resolved->height;
      if (resolved->depth) {
        point = true;
        return ViewOf(*resolved, swizzle, kSwizzleRRRR);
      }
      return ViewOf(*resolved, swizzle, kSwizzleRGBA);
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
    // 3D textures come later (B3/B4); the empty one keeps the draw.
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
  if (texture_bytes_ <= kTextureLimitBytes) {
    return;
  }
  const uint64_t target = kTextureLimitBytes / 4 * 3;
  const uint64_t frame = frames_.load();
  EnsureWork();
  for (auto it = textures_.begin(); it != textures_.end() && texture_bytes_ > target;) {
    Texture& t = it->second;
    if (t.frame != UINT64_MAX && t.frame + kFramesUnusedToRelease < frame) {
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

bool DkRecorder::DrawRejected(uint32_t reason, const char* text) {
  ++skipped_[reason];
  if (skipped_logged_.emplace(reason, true).second) {
    REXLOG_WARN("deko3d: draws skipped: {} (reason {}; counted in the report)", text, reason);
  }
  return false;
}

void DkRecorder::ApplyConstants(const RecorderDraw& d) {
  DeviceConstants& c = constants_[d.device];
  auto apply = [&c](const ConstantDelta& delta, float* bank, bool* dirty, uint32_t registers) {
    for (const ConstantRun& run : delta.runs) {
      if (run.first + run.count > registers) {
        continue;
      }
      std::memcpy(bank + run.first * 4, delta.values.data() + run.data, size_t(run.count) * 16);
      std::fill(dirty + run.first, dirty + run.first + run.count, true);
      c.any_dirty = true;
    }
  };
  apply(d.vertex_constants, c.vertex.data(), c.vertex_dirty.data(), kVertexConstantRegisters);
  apply(d.pixel_constants, c.pixel.data(), c.pixel_dirty.data(), kPixelConstantRegisters);
}

void DkRecorder::PushConstants(DkCmdBuf cmd, uint32_t device, bool vertex_used, bool pixel_used) {
  DeviceConstants& c = constants_[device];
  if (c.work != work_) {
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
    std::vector<std::pair<uint32_t, uint32_t>> fixes;  // (offset in the vertex, fix kind and element type << 8)
  };
  std::vector<StreamUse> used;
  const bool up = d.kind == RecorderDraw::Kind::kUp;
  for (const PackageShader::Input& input : vs->inputs) {
    if (input.uam_location >= 16) {
      continue;
    }
    attrib_count = std::max<uint32_t>(attrib_count, input.uam_location + 1u);
    if (!d.declaration) {
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
      continue;
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
      used[buffer].fixes.emplace_back(element->offset, fix | (element->type << 8));
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
  uint32_t index_min = 0;
  if (indexed) {
    const uint32_t bytes = d.index_32bit ? 4 : 2;
    if (!d.index_count || uint64_t(d.start_index + d.index_count) * bytes > d.index_size) {
      DrawRejected(kSkipIndexData, "indices outside their buffer");
      return;
    }
    index_source = memory_->TranslateVirtual<const uint8_t*>(d.index_address) + size_t(d.start_index) * bytes;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < d.index_count; ++i) {
      uint32_t v;
      if (d.index_32bit) {
        uint32_t raw;
        std::memcpy(&raw, index_source + size_t(i) * 4, 4);
        v = __builtin_bswap32(raw);
      } else {
        uint16_t raw;
        std::memcpy(&raw, index_source + size_t(i) * 2, 2);
        v = __builtin_bswap16(raw);
      }
      if (d.primitive_restart && v == d.primitive_restart_index) {
        continue;
      }
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (lo > hi) {
      return;  // restarts only
    }
    index_min = lo;
    first_vertex = uint32_t(int64_t(d.base_vertex) + lo);
    vertex_count = hi - lo + 1;
  } else {
    first_vertex = up ? 0 : d.start_vertex;
    vertex_count = d.vertex_count;
  }
  if (!vertex_count || vertex_count > kMaxVerticesPerDraw) {
    DrawRejected(kSkipTooManyVertices, "no vertices or too many in one draw");
    return;
  }

  // --- Textures and samplers, before anything this draw records: an upload may move on to the next work slot.
  // Each stage's slots as its shader declares them (an empty texture of the type for the others).
  struct StageTextures {
    std::array<DkResHandle, 32> handles{};
    uint32_t count = 0;
  };
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
        image = PrepareTexture(d.textures[slot], type, tw, th, point);
        sampler = SamplerOf(d.textures[slot].fetch, point);
      }
      out.handles[slot] = dkMakeTextureHandle(image, sampler);
      out.count = slot + 1;
    }
  };
  StageTextures vs_textures, ps_textures;
  prepare_textures(vs, vs_textures);
  prepare_textures(ps, ps_textures);

  // --- Upload space for everything this draw needs, all in one slot
  uint32_t needed = 3 * DK_UNIFORM_BUF_ALIGNMENT + kSharedBlockBytes + kVertexBlockBytes + kPixelBlockBytes;
  const uint32_t rect_vertices = rect_list ? vertex_count / 3 * 4 : vertex_count;
  for (const StreamUse& s : used) {
    needed += Align(rect_vertices * s.stride, 16) + 16;
  }
  if (indexed) {
    needed += Align(d.index_count * (d.index_32bit ? 4 : 2), 16) + 16;
  }
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
  uint32_t width = 0, height = 0;
  if (!BindTargets(d.render_targets, d.depth_stencil, color_mask, d.depth_stencil.handle != 0, width, height)) {
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
    rs.cullMode = cull.cull_face == NativeCullFace::kFront  ? DkFace_Front
                  : cull.cull_face == NativeCullFace::kBack ? DkFace_Back
                                                             : DkFace_None;
    rs.frontFace = cull.front_face_clockwise ? DkFrontFace_CW : DkFrontFace_CCW;
    rs.provokingVertex = DkProvokingVertex_First;
    const bool bias = stage >= 4 && d.fixed.depth_bias_enable && d.fixed.depth_bias_representable;
    rs.depthBiasEnableMask = bias ? DkPolygonFlag_All : 0;
    if (!e_.valid || !Same(rs, e_.raster)) {
      dkCmdBufBindRasterizerState(cmd, &rs);
      e_.raster = rs;
    }
    if (bias) {
      // D24FS8's units as Vulkan's D32_SFLOAT path takes them (da verificare, B4).
      const float b[2] = {std::bit_cast<float>(d.fixed.depth_bias_bits) * float(1 << 24),
                          std::bit_cast<float>(d.fixed.slope_scaled_depth_bias_bits)};
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
  const bool restart = indexed && d.primitive_restart;
  const uint32_t restart_index = restart ? (d.index_32bit ? d.primitive_restart_index : d.primitive_restart_index & 0xFFFF)
                                         : 0;
  if (!e_.valid || restart != e_.restart || restart_index != e_.restart_index) {
    dkCmdBufSetPrimitiveRestart(cmd, restart, restart_index);
    e_.restart = restart;
    e_.restart_index = restart_index;
  }
  std::array<DkVtxBufferState, 16> buffer_states{};
  for (size_t b = 0; b < used.size(); ++b) {
    buffer_states[b] = {used[b].stride, 0};
  }
  const uint64_t attribs_hash = XXH3_64bits(attribs.data(), sizeof(DkVtxAttribState) * attrib_count) ^
                                XXH3_64bits(buffer_states.data(), sizeof(DkVtxBufferState) * used.size()) ^
                                (uint64_t(attrib_count) << 56) ^ (uint64_t(used.size()) << 48);
  if (!e_.valid || attribs_hash != e_.attribs_hash) {
    dkCmdBufBindVtxAttribState(cmd, attribs.data(), attrib_count);
    dkCmdBufBindVtxBufferState(cmd, buffer_states.data(), uint32_t(used.size()));
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
  std::array<DkBufExtents, 16> extents{};
  for (size_t b = 0; b < used.size(); ++b) {
    const StreamUse& s = used[b];
    const uint8_t* source = nullptr;
    uint64_t available = 0;
    uint64_t start = 0;
    if (up) {
      source = d.up_vertices ? d.up_vertices->data() : nullptr;
      available = d.up_vertices ? d.up_vertices->size() : 0;
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
    const Upload u = core_.Reserve(out_bytes, 16);
    if (!u) {
      DrawRejected(kSkipUploadFull, "no room in the upload ring");
      return;
    }
    std::vector<uint8_t> converted;
    uint8_t* target = rect_list ? (converted.resize(bytes), converted.data()) : u.cpu;
    CopyGuestWords(target, source + start, bytes);
    for (const auto& [offset, fix] : s.fixes) {
      const uint32_t kind = fix & 0xFF;
      const uint32_t type = fix >> 8;
      for (uint64_t v = 0; v < vertex_count; ++v) {
        uint8_t* p = target + v * s.stride + offset;
        if (offset + ElementBytes(type) > s.stride) {
          break;
        }
        if (kind == 1) {
          // 16-bit components: after the 32-bit swap each word holds them in the wrong order.
          for (uint32_t w = 0; w < ElementBytes(type) / 4; ++w) {
            uint32_t x;
            std::memcpy(&x, p + w * 4, 4);
            x = std::rotl(x, 16);
            std::memcpy(p + w * 4, &x, 4);
          }
        } else if (kind == 2) {
          uint32_t x;
          std::memcpy(&x, p, 4);
          x = (x & 0x3FFFFFFF) | 0x40000000;
          std::memcpy(p, &x, 4);
        } else if (kind == 3) {
          std::swap(p[0], p[2]);
        }
      }
    }
    if (rect_list) {
      // A rectangle is three vertices; the fourth is v1 + v2 - v0 for the float words of the vertex (the others are
      // v2's), drawn as a quad v0 v1 v3 v2.
      const uint32_t rects = vertex_count / 3;
      for (uint32_t r = 0; r < rects; ++r) {
        const uint8_t* v0 = converted.data() + size_t(r) * 3 * s.stride;
        const uint8_t* v1 = v0 + s.stride;
        const uint8_t* v2 = v1 + s.stride;
        uint8_t* q = u.cpu + size_t(r) * 4 * s.stride;
        std::memcpy(q, v0, s.stride);
        std::memcpy(q + s.stride, v1, s.stride);
        std::memcpy(q + 3 * s.stride, v2, s.stride);
        uint8_t* v3 = q + 2 * s.stride;
        std::memcpy(v3, v2, s.stride);
        for (uint32_t w = 0; w + 4 <= s.stride; w += 4) {
          float a, b2, c;
          std::memcpy(&a, v0 + w, 4);
          std::memcpy(&b2, v1 + w, 4);
          std::memcpy(&c, v2 + w, 4);
          const float x = b2 + c - a;
          if (std::isfinite(a) && std::isfinite(b2) && std::isfinite(c) && std::isfinite(x)) {
            std::memcpy(v3 + w, &x, 4);
          }
        }
      }
    }
    extents[b] = {u.gpu, out_bytes};
  }
  for (size_t b = 0; b < used.size(); ++b) {
    // One buffer per call (deko3d 0.5.0 reserves 5 words per buffer and writes 6).
    dkCmdBufBindVtxBuffers(cmd, uint32_t(b), &extents[b], 1);
  }
  if (indexed) {
    const uint32_t bytes = d.index_32bit ? 4 : 2;
    const Upload u = core_.Reserve(d.index_count * bytes, 16);
    if (!u) {
      DrawRejected(kSkipUploadFull, "no room in the upload ring");
      return;
    }
    if (d.index_32bit) {
      for (uint32_t i = 0; i < d.index_count; ++i) {
        uint32_t raw;
        std::memcpy(&raw, index_source + size_t(i) * 4, 4);
        raw = __builtin_bswap32(raw);
        std::memcpy(u.cpu + size_t(i) * 4, &raw, 4);
      }
    } else {
      for (uint32_t i = 0; i < d.index_count; ++i) {
        uint16_t raw;
        std::memcpy(&raw, index_source + size_t(i) * 2, 2);
        raw = __builtin_bswap16(raw);
        std::memcpy(u.cpu + size_t(i) * 2, &raw, 2);
      }
    }
    dkCmdBufBindIdxBuffer(cmd, d.index_32bit ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, u.gpu);
  }

  // --- Constants: the guest banks by differences, and the shared block when it changes
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

  // --- The draw
  if (indexed) {
    dkCmdBufDrawIndexed(cmd, primitive, d.index_count, 1, 0, -int32_t(index_min), 0);
  } else if (rect_list) {
    dkCmdBufDraw(cmd, DkPrimitive_Quads, vertex_count / 3 * 4, 1, 0, 0);
  } else {
    dkCmdBufDraw(cmd, primitive, vertex_count, 1, 0, 0);
  }
  ++draws_drawn_;
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
      // A capture of the frame every nx_dk_capture_s seconds.
      if (capture_next_ns_ && NowNs() >= capture_next_ns_ && !capture_.pending &&
          source->format == DkImageFormat_RGBA8_Unorm) {
        capture_next_ns_ = NowNs() + uint64_t(options_.capture_seconds) * 1'000'000'000ull;
        Capture(*source, width, height);
      }
    }
  }
  ForgetState();
  CloseWork();
  swapchain_.Present(image);
  frames_.fetch_add(1);
  WriteCaptures();
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
              "constant pushes {}; images {} MB in {} chunks, shader code {} KB; deko3d blocks {} ({} MB); "
              "libdeko3dd errors {} warnings {}",
              textures_.size(), texture_bytes_ >> 20, textures_created_, textures_uploaded_, bytes_uploaded_ >> 20,
              resolved_.size(), shaders_from_pc_, shaders_compiled_, shaders_failed_, compiling_.size(),
              s.command_bytes_max >> 10, s.works, s.upload_bytes >> 20, s.upload_full, slot_switches_, barriers_,
              constant_pushes_, s.image_bytes >> 20, s.image_chunks, s.code_bytes >> 10, device_.live_blocks(),
              device_.block_bytes() >> 20, dbg.errors.load(), dbg.warnings.load());
  report_ns_ = now;
  report_frames_ = frames;
  report_draws_ = draws_;
}

}  // namespace rex::graphics::gta4_native::dk
