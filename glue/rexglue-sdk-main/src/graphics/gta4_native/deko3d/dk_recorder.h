// gta4_native deko3d back end: the recorder (native_recorder.h's INativeRecorder on deko3d), docs/switch-port/
// 16-deko3d-devlog.md, M3 B. Switch only.
//
// Built the way nfsmw-nx's deko3d renderer is (StevensND and contributors, GPL-3.0; app/src/nfsmw_dk_*.cpp at
// 266f943: the frame and slot handling, the texture cache with fingerprints and untiling, the samplers, the
// constants by differences and the state emitted only when it changes are ported from there), on what this game's
// front end hands over instead of nfsmw's PM4 registers. Its rules (16-deko3d-devlog.md, "Rules"):
//   - no pipeline objects: each block of state (rasterizer, colour, blend, depth and stencil, vertex input,
//     viewport, scissor, textures, uniform buffers) is emitted only when it differs from what this command list
//     last got;
//   - one vertex buffer per dkCmdBufBindVtxBuffers call; 256 bytes of slack at the end of each command memory piece
//     (dk_core.h);
//   - textures are bound as handles (dkMakeTextureHandle(image descriptor, sampler descriptor)); descriptors are
//     written with dkCmdBufPushData only, into the slot's pre-list;
//   - barriers only where there is a hazard: a Full barrier before a copy engine transfer when 3D work was recorded
//     since the last one, and after it before the next 3D work;
//   - a flip is the viewport swizzle, never a negative height.
//
// Stages (nx_dk_stage, read at every draw so a toml change needs no rebuild):
//   1 the presenter and the output (the guest front buffer shown, the gamma, PNG captures), clears, render targets
//     as images, resolves as copies; every draw is skipped (counted);
//   2 + textures (untiled with NEON on the CPU, uploaded in the pre-list), samplers and the 2D draws (no depth
//     test): logos, loading screens, Bink videos, the HUD;
//   3 + the 3D draws with the PC's DKSH; a variant the PC could not compile goes to the console's uam in the
//     background and its draws are skipped with a log line until it is ready;
//   4 + depth: depth bias, ZCULL, deferred depth clears, shadows and reflections (later work);
//   5 + post-processing, FSR1 and the final pass (later work).

#pragma once

#include <deko3d.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../native_recorder.h"
#include "dk_compiler.h"
#include "dk_core.h"
#include "dk_shader_package.h"

namespace rex::memory {
class Memory;
}

namespace rex::graphics::gta4_native::dk {

struct RecorderOptions {
  std::function<int()> stage;           // nx_dk_stage, read at every draw
  std::function<float()> gamma;         // nx_dk_gamma: output = pow(front buffer, 1 / gamma); 1 = as it is
  int capture_seconds = 0;              // nx_dk_capture_s: a PNG of the shown frame every N seconds (0: none)
  std::string capture_dir = "sdmc:/switch/theft4/captures";
  uint32_t output_width = 1280, output_height = 720;
};

// An image of the recorder: a render target, a resolved texture or a game texture.
struct DkImageEntry {
  DkImage image{};
  ImageMemory memory;
  DkImageType type = DkImageType_2D;
  DkImageFormat format = DkImageFormat_None;
  uint32_t width = 0, height = 0, layers = 1, levels = 1;
  bool depth = false;
  bool live = false;
  uint64_t serial = 0;  // unique per creation: its descriptors' key
};

class DkRecorder final : public INativeRecorder {
 public:
  DkRecorder(Device& device, memory::Memory* memory, ShaderPackage& package, Compiler& compiler,
             RecorderOptions options);
  ~DkRecorder() override;
  bool Initialize();

  void Draw(const RecorderDraw& draw) override;
  void Clear(const RecorderClear& clear) override;
  void Resolve(const RecorderResolve& resolve) override;
  void Present(const RecorderPresent& present) override;
  void ResourceEvent(const RecorderResourceEvent& event) override;
  void Report() override;

  uint64_t frames() const { return frames_.load(); }

 private:
  // ---- Works and barriers
  void EnsureWork();
  void CloseWork();
  void SwitchSlot();
  void BeginWorkState();
  void ForgetState();
  void BeforeTransfer();
  void Before3D();
  Upload Reserve(uint32_t bytes, uint32_t alignment);

  // ---- Images, render targets, resolved textures
  bool CreateImage(DkImageEntry& image, DkImageType type, DkImageFormat format, uint32_t width, uint32_t height,
                   uint32_t layers, uint32_t levels, uint32_t flags, const char* what);
  void DestroyImage(DkImageEntry& image);
  DkImageEntry* Surface(const SurfaceDescriptor& d, bool depth);
  bool BindTargets(const std::array<SurfaceDescriptor, kRenderTargetCount>& colors, const SurfaceDescriptor& depth,
                   uint32_t color_mask, bool use_depth, uint32_t& width, uint32_t& height);
  DkImageEntry* ResolvedTexture(uint32_t handle, uint32_t base);

  // ---- Textures, samplers and descriptors (nfsmw_dk_texturas.cpp's)
  struct TextureFormat {
    DkImageFormat format = DkImageFormat_None;
    uint8_t block = 1;   // texels per block side
    uint8_t bytes = 1;   // bytes per block
    uint8_t unit = 0;    // byte swap unit: 0 none, 2 or 4
    uint16_t host_swizzle = 0;
  };
  static bool FormatOf(uint32_t format, TextureFormat& out);
  uint32_t PrepareTexture(const RecorderTexture& t, uint8_t shader_type, uint32_t& width, uint32_t& height,
                          bool& point);
  uint32_t ViewOf(const DkImageEntry& image, uint32_t guest_swizzle, uint16_t host_swizzle);
  uint32_t SamplerOf(const std::array<uint32_t, 6>& f, bool point);
  uint32_t EmptyOf(uint8_t shader_type) const;
  uint32_t ReserveImageDescriptor();
  void ReleaseDescriptorsOf(uint64_t serial);
  void WriteImageDescriptor(uint32_t slot, const DkImageView& view);
  bool CreateEmpties();
  bool ReadLevel(uint64_t address, bool tiled, uint32_t pitch_blocks, uint64_t row_bytes, const TextureFormat& tf,
                 uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host, uint8_t* out);
  void ReleaseTexturesIfNeeded();

  // ---- Shaders
  const DkShader* ShaderFor(const PackageShader& shader, uint32_t value, bool& pending);
  void CollectCompiles();

  // ---- Draws
  bool DrawRejected(uint32_t reason, const char* text);
  // Every draw's constant differences reach the device's banks, drawn or skipped; a drawn draw then pushes all the
  // registers that changed since its device's blocks were last brought up to date.
  void ApplyConstants(const RecorderDraw& d);
  void PushConstants(DkCmdBuf cmd, uint32_t device, bool vertex_used, bool pixel_used);

  // ---- Output
  bool CreateOutputShaders();
  void Capture(const DkImageEntry& source, uint32_t width, uint32_t height);
  void WriteCaptures();

  Device& device_;
  memory::Memory* memory_;
  ShaderPackage& package_;
  Compiler& compiler_;
  RecorderOptions options_;
  Core core_;
  Swapchain swapchain_;
  uint64_t work_ = 0;
  uint64_t serial_ = 0;

  bool transfer_since_barrier_ = false;
  bool work3d_since_barrier_ = true;

  // Render targets by guest surface object (and format and size); resolved textures by destination object, and by
  // guest base address for the fetch constants that name them.
  struct SurfaceKey {
    uint32_t handle, format, width, height;
    bool depth;
    bool operator==(const SurfaceKey&) const = default;
  };
  struct SurfaceKeyHash {
    size_t operator()(const SurfaceKey& k) const {
      return (size_t(k.handle) * 0x9E3779B97F4A7C15ull) ^ (size_t(k.format) << 1) ^ (size_t(k.width) << 20) ^
             (size_t(k.height) << 40) ^ size_t(k.depth);
    }
  };
  std::unordered_map<SurfaceKey, DkImageEntry, SurfaceKeyHash> surfaces_;
  struct Resolved {
    DkImageEntry image;
    uint32_t base = 0;
    uint32_t guest_format = 0;
    bool written = false;
  };
  std::unordered_map<uint32_t, Resolved> resolved_;          // destination texture object
  std::unordered_map<uint32_t, uint32_t> resolved_by_base_;  // guest base -> destination object
  uint64_t texture_generation_ = 1;                          // goes up when a resolved texture appears or goes

  // Game textures (nfsmw's TexturaDk)
  struct Texture {
    DkImageEntry image;
    uint64_t raw_hash = 0;
    uint64_t frame = UINT64_MAX;  // last frame checked
    uint64_t next = 0;            // frame of the next check
    uint32_t interval = 1;        // frames between checks: 1 to 32
    bool has_content = false;
    uint64_t bytes = 0;
  };
  std::unordered_map<uint64_t, Texture> textures_;
  std::unordered_set<uint32_t> dirty_handles_;  // ResourceUnlock with a guest write: checked at the next use
  uint64_t texture_bytes_ = 0;
  std::unordered_map<uint64_t, uint32_t> views_;  // (serial, swizzles) -> descriptor
  std::unordered_map<uint64_t, std::vector<uint64_t>> views_by_serial_;
  std::vector<uint32_t> free_descriptors_;
  std::vector<std::vector<uint32_t>> descriptors_to_free_;  // per slot
  uint32_t next_descriptor_ = 8;
  std::unordered_map<uint32_t, uint32_t> samplers_;  // key -> descriptor
  uint32_t next_sampler_ = 1;
  std::array<DkImageEntry, 4> empties_;  // 2D, 2D array, 3D, cube (descriptors 0-3)
  std::vector<uint8_t> scratch_;

  // Shaders: (hash, late, value) -> loaded DKSH (nullptr: none); the variants the console's uam compiles.
  struct ShaderKey {
    uint64_t hash;
    uint32_t value;
    bool late;
    bool operator==(const ShaderKey&) const = default;
  };
  struct ShaderKeyHash {
    size_t operator()(const ShaderKey& k) const {
      return size_t(k.hash) ^ (size_t(k.value) * 0x9E3779B97F4A7C15ull) ^ size_t(k.late);
    }
  };
  std::unordered_map<ShaderKey, std::unique_ptr<DkShader>, ShaderKeyHash> shaders_;
  struct Mailbox {
    std::mutex mutex;
    std::vector<std::pair<ShaderKey, std::vector<uint8_t>>> done;
    std::vector<std::pair<ShaderKey, std::string>> failed;
  };
  std::shared_ptr<Mailbox> mailbox_ = std::make_shared<Mailbox>();
  std::unordered_set<ShaderKey, ShaderKeyHash> compiling_;
  uint64_t shaders_from_pc_ = 0, shaders_compiled_ = 0, shaders_failed_ = 0, uam_ns_ = 0;

  // What this command list last got (nfsmw's "Grabado")
  struct Emitted {
    bool valid = false;
    std::array<uint32_t, kRenderTargetCount + 1> targets{};  // the bound surfaces' handles (and the depth's)
    bool targets_valid = false;
    uint32_t width = 0, height = 0;
    const DkShader* vs = nullptr;
    const DkShader* ps = nullptr;
    DkRasterizerState raster{};
    DkColorState color{};
    DkColorWriteState masks{};
    DkDepthStencilState depth{};
    std::array<DkBlendState, 4> blends{};
    DkViewport viewport{};
    DkScissor scissor{};
    float blend_constants[4] = {};
    float depth_bias[2] = {};
    uint32_t stencil[2] = {};
    bool restart = false;
    uint32_t restart_index = 0;
    uint64_t attribs_hash = 0;
    std::array<DkResHandle, 32> vs_textures{}, ps_textures{};
    uint32_t vs_texture_count = 0, ps_texture_count = 0;
    DkGpuAddr ubo[3] = {};
  } e_;

  // Constants by differences: per device, the banks as the recorder knows them; per work, one block per stage
  // written whole at the work's first draw, then kept in step with dkCmdBufPushConstants.
  struct DeviceConstants {
    std::array<float, kVertexConstantRegisters * 4> vertex{};
    std::array<float, kPixelConstantRegisters * 4> pixel{};
    std::array<bool, kVertexConstantRegisters> vertex_dirty{};
    std::array<bool, kPixelConstantRegisters> pixel_dirty{};
    bool any_dirty = false;
    uint64_t work = UINT64_MAX;
    DkGpuAddr vertex_block = 0, pixel_block = 0;
  };
  std::unordered_map<uint32_t, DeviceConstants> constants_;
  std::array<uint8_t, 1024> shared_previous_{};
  DkGpuAddr shared_gpu_ = 0;
  uint64_t shared_work_ = UINT64_MAX;

  // Output
  DkShader output_vs_{}, output_ps_{};
  bool output_valid_ = false;
  DkMemBlock capture_block_ = nullptr;
  uint32_t capture_bytes_ = 0;
  uint64_t capture_next_ns_ = 0;
  struct PendingCapture {
    uint64_t work = 0;
    uint32_t width = 0, height = 0;
    bool pending = false;
  } capture_;
  std::thread capture_writer_;
  uint32_t captures_written_ = 0;

  // Statistics
  std::atomic<uint64_t> frames_{0};
  uint64_t draws_ = 0, draws_drawn_ = 0, clears_ = 0, resolves_ = 0, resolves_done_ = 0;
  uint64_t presents_shown_ = 0, presents_without_source_ = 0;
  std::unordered_map<uint32_t, uint64_t> skipped_;  // reason -> draws
  std::unordered_map<uint32_t, bool> skipped_logged_;
  uint64_t textures_created_ = 0, textures_uploaded_ = 0, bytes_uploaded_ = 0;
  uint64_t barriers_ = 0, slot_switches_ = 0, constant_pushes_ = 0;
  uint64_t record_ns_ = 0;
  uint64_t report_frames_ = 0, report_draws_ = 0, report_ns_ = 0;
};

}  // namespace rex::graphics::gta4_native::dk
