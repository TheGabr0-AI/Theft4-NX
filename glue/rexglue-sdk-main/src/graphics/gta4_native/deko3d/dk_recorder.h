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
#include "dk_depth_alias.h"
#include "dk_depth_handoff.h"
#include "dk_geometry_store.h"
#include "dk_ab.h"
#include "dk_placement.h"
#include "dk_shader_probe.h"
#include "dk_shader_package.h"
#include "dk_overwrite.h"
#include "dk_stage_counters.h"

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
  bool blend_alpha_fix = true;    // nx_dk_blend_alpha_fix (run 48): write deko3d 0.5.0's wrong alpha factor again
  bool mixup_from_memory = true;  // nx_dk_mixup_from_memory (run 48): a colour fetch whose address holds a resolved
                                  // depth reads guest memory, as LRA does, instead of the depth
  // Run 49 (native_draw_snapshot.h): a buffer whose bytes changed between submit and record; the front end copies its
  // draws at submit from then on. Called on the recorder's thread.
  std::function<void(uint32_t handle)> promote_dynamic_buffer;
  // Run 58: the A/B's "copies off" phase switches the front end's submit copies (NativeFrontEnd::SetSubmitCopies).
  std::function<void(bool on)> submit_copies;
  // Run 59: the A/B's "pages off" / "pages only" phases (NativeFrontEnd::SetVolatilePages / SetHandlePromotions).
  std::function<void(bool on)> volatile_pages;
  std::function<void(bool on)> handle_promotions;
  // nx_dk_ab_phases (run 59, dk_ab.h AbPhaseBit): the phases the A/B runs; 0 = run 58's six.
  uint32_t ab_phase_mask = 0;
  // nx_dk_constant_versions (run 50): a draw whose constants changed gets its own copy of the bank (a version), written
  // by the CPU and never changed afterwards, as the Vulkan path's constant arena; off: one block per work updated inline
  // with dkCmdBufPushConstants (up to run 49).
  bool constant_versions = true;
  // nx_dk_geometry_mb (run 52): MiB of the geometry store (dk_geometry_store.h); 0 = off, every draw's vertices to the
  // upload ring as up to run 51.
  uint32_t geometry_mb = 96;
  // nx_dk_skin_diff (run 54): skinned vertex shaders whose GPU values are read back (dk_shader_probe.h), one draw each;
  // 0 = none.
  uint32_t skin_diff = 1;
  // nx_dk_geometry_trust (run 53): a range checked once is trusted until the next work, frame or non-draw command
  // (nfsmw-nx's epoch); false = every lookup hashes the whole range, as run 52.
  bool geometry_trust = true;
  // nx_dk_depth_bias (run 55): the title's depth bias at every stage, as the Vulkan path (through run 54: only at stage 4)
  bool depth_bias = true;
  // nx_dk_depth_only (run 55, banjo-nx's depth-only draws): a draw that writes no colour, without alpha test or alpha to
  // coverage, whose pixel shader neither discards nor writes depth (package flags), is drawn with no pixel shader
  bool depth_only = true;
  // nx_dk_vs_prune (run 56, masseffect-nx S6): a draw drawn without a pixel shader takes its vertex shader's
  // position-only variant; a pixel shader that declares none of the vertex shader's constant-zero outputs, the
  // live-outputs variant (both from the package, compiled and checked on the PC); false = the full variant always.
  bool vs_prune = true;
  // nx_dk_anatomy (run 56): with the frame captures, every resolve of the captured frame is saved too (its source and
  // destination named in the file), so a picture fault is placed at the first target that shows it; false = only the
  // shown frame.
  bool anatomy = false;
  // nx_dk_placement (run 57, dk_placement.h, the Vulkan path's placement owners): a colour surface used while another
  // surface of the same EDRAM placement holds the content gets that content first (copy or sample-layout conversion);
  // false = every surface keeps only what was drawn into it, as through run 56.
  bool placement = true;
  // nx_dk_keep_targets (run 57): a draw that writes no colour keeps the title's colour targets of the depth's size bound
  // (all colour writes masked), so the lights' stencil and light passes share one binding; false = as through run 56.
  bool keep_targets = true;
  // nx_dk_ab (run 58, dk_ab.h): once gameplay is on screen, phases of 30 s switch one thing at a time (keep targets,
  // placement, the geometry store, culling) and the log compares each with the baseline of the same session.
  bool ab_phases = false;
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
  uint64_t placement_serial = 0;  // run 57: the EDRAM placement content it holds (DkRecorder::placement_), 0: its own
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
  void DepthHandoff(const RecorderDepthHandoff& handoff) override;
  void Present(const RecorderPresent& present) override;
  void ResourceEvent(const RecorderResourceEvent& event) override;
  void TextureLock(const RecorderTextureLock& lock) override;  // run 54
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

  // ---- GPU time per pass (run 44, read-only measures): a GPU timestamp at the start of each work, at each change of
  // render targets, at each resolve and at the end of the work, written into the core's report block (a slice per slot);
  // when the slot comes round again (its fence has been waited for) the differences are added to the label of the pass
  // that began at the earlier stamp. Says which passes the GPU's time goes to, and how much of the wall time it is busy.
  void StampPass(uint32_t kind, uint32_t first, uint32_t second, uint32_t width, uint32_t height);
  void HarvestStamps(uint32_t slot);

  // ---- Images, render targets, resolved textures
  bool CreateImage(DkImageEntry& image, DkImageType type, DkImageFormat format, uint32_t width, uint32_t height,
                   uint32_t layers, uint32_t levels, uint32_t flags, const char* what);
  void DestroyImage(DkImageEntry& image);
  DkImageEntry* Surface(const SurfaceDescriptor& d, bool depth);
  bool BindTargets(const std::array<SurfaceDescriptor, kRenderTargetCount>& colors, const SurfaceDescriptor& depth,
                   uint32_t color_mask, bool use_depth, uint32_t& width, uint32_t& height);
  // `object`: the destination object that was found (the handle itself, or the one at that base address).
  DkImageEntry* ResolvedTexture(uint32_t handle, uint32_t base, bool* by_base = nullptr, uint32_t* object = nullptr);
  // The RGBA8 image whose bytes are the resolved depth `object` packed as the title's A8R8G8B8 alias reads it
  // (dk_depth_alias.h): converted by a full-screen pass when the depth was resolved again since; nullptr if it cannot
  // be made (the caller keeps the depth image).
  DkImageEntry* PackedDepthAlias(uint32_t object);

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
  // `stencil`: a depth image's stencil plane instead of its depth.
  uint32_t ViewOf(const DkImageEntry& image, uint32_t guest_swizzle, uint16_t host_swizzle, bool stencil = false);
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
  const DkShader* PrunedShaderFor(const PackageShader& shader, uint32_t value);  // run 56
  void CollectCompiles();

  // ---- Draws
  bool DrawRejected(uint32_t reason, const char* text);
  void ShaderMissing(uint64_t hash, bool pixel);
  // Every draw's constant differences reach the device's banks, drawn or skipped; a drawn draw then pushes all the
  // registers that changed since its device's blocks were last brought up to date.
  void ApplyConstants(const RecorderDraw& d);
  void PushConstants(DkCmdBuf cmd, uint32_t device, bool vertex_used, bool pixel_used);

  // ---- Output
  bool CreateOutputShaders();
  void Capture(const DkImageEntry& source, uint32_t width, uint32_t height);
  // Run 56 (nx_dk_anatomy, dk_anatomy.h): one image of the anatomy frame read back at once (the GPU is waited for).
  void AnatomyCapture(const DkImageEntry& image, uint32_t source, uint32_t destination, const char* kind);
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
    uint64_t writes = 0;  // resolves into it so far (a packed alias is converted again after each)
    std::array<uint32_t, 6> fetch{};  // run 54: the destination's fetch constant (its guest layout, for a lock)
  };
  struct DepthAlias {
    DkImageEntry image;  // RGBA8: the bytes of (depth24 << 8) | stencil
    uint64_t source_serial = 0;
    uint64_t source_writes = UINT64_MAX;
  };
  std::unordered_map<uint32_t, DepthAlias> aliases_;  // by the resolved depth's destination object
  DkShader alias_vs_{}, alias_ps_{};
  bool alias_valid_ = false;
  // The scene depth hand-off (dk_depth_handoff.h): its pixel shader (the vertex shader is the alias pass's), and what
  // became of the commands: done, and the rejections by verdict (HandoffVerdict).
  DkShader handoff_ps_{};
  // Run 57: EDRAM placements (dk_placement.h). The owner of each placement: the surface whose content is current there,
  // by its key (never a pointer: a released surface leaves the map), the image's serial when it claimed it, its view.
  struct PlacementOwner {
    SurfaceKey surface{};
    uint64_t image_serial = 0;
    GuestSurfaceView view{};
    uint64_t serial = 0;
  };
  std::unordered_map<GuestPlacementKey, PlacementOwner, GuestPlacementKeyHash> placement_;
  uint64_t placement_serial_ = 0;
  bool full_clear_bind_ = false;  // BindTargets for a clear of every pixel: no placement content needed first
  uint64_t kept_target_draws_ = 0, report_kept_targets_ = 0, report_target_binds_ = 0;  // run 57: nx_dk_keep_targets
  uint64_t target_binds_ = 0;  // run 57: BindTargets calls that bound new targets
  DkShader placement_ps_{};
  bool placement_valid_ = false;
  struct {
    uint64_t copies = 0, conversions = 0, refused = 0, stale = 0;
  } placement_stats_, placement_reported_;
  std::unordered_set<uint64_t> placement_logged_;
  void ClaimPlacement(const SurfaceDescriptor& d, DkImageEntry& image, bool depth);
  void PrepareSurfaceContent(const SurfaceDescriptor& d, DkImageEntry& image, bool depth);
  // Run 58: the A/B phases (dk_ab.h, nx_dk_ab). The configured switches are kept to restore them between phases.
  AbSchedule ab_;
  bool ab_store_off_ = false, ab_cull_off_ = false;
  bool ab_keep_targets_ = true, ab_placement_ = true;
  uint64_t ab_mark_materializations_ = 0, ab_mark_binds_ = 0, ab_mark_draws_ = 0;
  uint64_t ab_mark_rechecked_ = 0, ab_mark_changed_ = 0;  // run 59
  void StepAb(double seconds, uint64_t frames, double gpu_busy_ms, double gpu_gap_ms);
  bool handoff_valid_ = false;
  uint32_t handoff_destination_ = 0;  // the depth surface the last hand-off filled (its guest handle)
  // Draws per depth surface (guest handle), since the start: all, two-sided z-fail volumes, depth-writing, stencil-testing.
  struct DepthUse {
    uint64_t draws = 0, zfail = 0, writes = 0, stencil = 0, lights = 0, lights_zfail = 0;  // lights: under the kLightsToScreen marker
  };
  std::unordered_map<uint32_t, DepthUse> depth_uses_;
  std::unordered_set<uint64_t> depth_states_logged_, clears_logged_;
  struct HandoffStats {
    uint64_t done = 0, commands = 0, rejected[8] = {};
    uint32_t logged = 0;
  } handoff_;
  std::unordered_set<uint32_t> exponents_logged_;
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
    bool vertex_changed = false, pixel_changed = false;  // since the last version (constant_versions)
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
  // Run 56: the anatomy frame (the frame after every kAnatomyEvery-th capture, kAnatomyFrames in a run).
  // Run 57: every 12th capture, 4 frames, images up to 16 MB (run 56: 8 frames of 35 images, ~5 s each, most of it the
  // two 2560x2560 shadow maps of 26 and 52 MB).
  static constexpr uint32_t kAnatomyEvery = 12, kAnatomyFrames = 4, kAnatomyImages = 40;
  static constexpr uint64_t kAnatomyMaxBytes = 16ull << 20;
  bool anatomy_frame_ = false;
  uint32_t anatomy_taken_ = 0, anatomy_images_ = 0, captures_requested_ = 0;
  uint64_t anatomy_ns_ = 0;
  DkMemBlock anatomy_block_ = nullptr;
  uint32_t anatomy_bytes_ = 0;
  std::thread anatomy_writer_;

  // GPU time per pass (StampPass): the labels (interval accumulators, reset by the report), the stamps of each slot's last
  // work (stamp i is the 16-byte report at query index slot * kStampsPerSlot + i), the GPU's busy and idle time.
  struct PassLabel {
    std::string text;
    uint64_t ns = 0, passes = 0, draws = 0;
    uint64_t max_ns = 0, max_draws = 0, max_vs = 0, max_ps = 0;  // run 55: its costliest pass in the interval
  };
  struct Stamp {
    uint32_t label = 0;
    uint32_t draws = 0;
    uint64_t vertex_shader = 0, pixel_shader = 0;  // the pass's first draw (run 45: the stall's one-draw passes)
  };
  static constexpr uint32_t kStampsPerSlot = 1000;
  std::vector<std::vector<Stamp>> slot_stamps_;
  std::unordered_map<uint64_t, uint32_t> label_ids_;
  std::vector<PassLabel> labels_;
  struct GpuTime {
    uint64_t busy_ns = 0, gap_ns = 0, works = 0, max_work_ns = 0, last_end_ns = 0, unstamped_works = 0;
    bool have_last = false;
    uint32_t raw_logged = 0, slow_logged = 0;
  } gpu_;

  // Statistics
  std::atomic<uint64_t> frames_{0};
  uint64_t draws_ = 0, draws_drawn_ = 0, clears_ = 0, resolves_ = 0, resolves_done_ = 0;
  uint64_t presents_shown_ = 0, presents_without_source_ = 0;
  std::unordered_map<uint32_t, uint64_t> skipped_;  // reason -> draws
  std::unordered_map<uint32_t, bool> skipped_logged_;
  // Run 61: the hash of every shader a draw asked for and the package lacks (case A: the draw is skipped), once each.
  std::unordered_set<uint64_t> missing_shaders_logged_;
  uint64_t textures_created_ = 0, textures_uploaded_ = 0, bytes_uploaded_ = 0;
  uint64_t barriers_ = 0, slot_switches_ = 0, constant_pushes_ = 0;
  // Draws and binds that met a known difference from the Vulkan path (run 37 audit): counted and reported, nothing is
  // done about them. A count is not a visible defect; it says which candidate to test next.
  struct Gaps {
    uint64_t depth_clamp = 0;          // fixed.depth_clamp_enable: the title's own request (run 43: honoured)
    uint64_t zfail_volumes = 0;        // two-sided z-fail light volumes (the Vulkan path clamps depth for them too)
    uint64_t zfail_in_light_setup = 0;  // ... of those, drawn under the title's light-setup phase marker
    uint64_t depth_clamp_set = 0;      // draws drawn with depthClampEnable (run 43: the title's request or a z-fail volume)
    uint64_t resolved_colour_fetches = 0;  // fetches of a resolved colour image (run 42 view rule; counts, not logged each)
    uint64_t resolved_colour_stored = 0;   // ... of those, the 0x60A fetches viewed as stored
    uint64_t depth_without_test = 0;   // a depth surface bound by a draw with neither depth nor stencil test
    uint64_t extent_mismatch = 0;      // target binds whose images differ in size (the draw takes the smallest)
    uint64_t missing_inputs = 0;       // vertex shader inputs without an element (bound as fixed attributes)
    uint64_t lod_bias_draws = 0;       // draws sampling a fetch constant with a non-zero LOD bias (not delivered)
    uint64_t base_lookups = 0;         // textures found by base address because the handle had no resolve
    uint64_t base_type_mismatch = 0;   // ... of those, a depth image for a colour fetch or the reverse
    uint64_t resolve_exponent = 0;     // resolves with a non-zero colour exponent (flags >> 26; not applied)
    uint64_t resolve_cross_clear = 0;  // resolve clears (bits 8, 9) of the aspect the source does not have
    // Run 48.
    uint64_t blend_binds = 0;           // blend states bound (dkCmdBufBindBlendState)
    uint64_t blend_alpha_distinct = 0;  // ... of those, alpha destination factor != colour one (deko3d 0.5.0 got them wrong)
    uint64_t mixup_from_memory = 0;     // colour fetches of a resolved depth's address read from guest memory instead
    uint64_t volume_fetches = 0;        // 3D texture fetches (served the empty texture)
    uint64_t descriptor_evictions = 0;  // textures released because the image descriptor table was nearly full
    // Run 49.
    uint64_t zero_inputs = 0;           // inputs without an element read from a zero record (the Vulkan path's default binding)
    uint64_t masked_indices = 0;        // 32-bit indices with a top byte (cut to Xenos's 24 bits)
    uint64_t restart_draws = 0;         // indexed draws with the host restart on (strips and fans only)
    uint64_t restart_refused = 0;       // ... draws whose device state asked for it on another primitive or a wide 16-bit value
    uint64_t data_copied = 0;           // draws recorded from the bytes the front end copied at submit
    uint64_t data_copy_range_differs = 0;  // ... copies not used: the range differed (read from guest memory instead)
    uint64_t data_checked = 0;          // sampled draws whose bytes were hashed again here
    uint64_t data_changed = 0;          // ... of those, bytes changed since submit (the buffers handed back to the front end)
    // Run 59: the changed draws by part (indices, streams 0, 1, 2: DrawDataPartHashes), those with skinning inputs, those
    // drawn into the G-buffer's depth (the characters' holes), and the draws recorded from copies by reason.
    uint64_t data_changed_part[4] = {};
    uint64_t data_changed_skinned = 0, data_changed_gbuffer = 0;
    uint64_t data_copied_reason[5] = {};
    uint64_t direct_vertex_streams = 0;  // streams copied straight into the ring (no fix-up, no scratch pass)
    uint64_t skin_checked = 0;          // sampled skinned draws whose first vertex was decoded (BLENDWEIGHT/BLENDINDICES)
    uint64_t skin_weight_bad = 0;       // ... with a weight sum outside 0.95-1.05 (4 unorm bytes)
    uint64_t skin_index_bad = 0;        // ... with a bone index past the palette (byte >= 64: c64 + 3 * 64 = c256)
    // Run 53 (dk_skin_check.h): the sampled first vertex skinned on the CPU: draws, bones used, bones whose 3x3 is
    // not rigid, bones with all-zero rows, vertices more than 3 m from their weighted bone origin.
    uint64_t skin_cpu_checked = 0, skin_cpu_bones = 0, skin_cpu_not_rigid = 0, skin_cpu_zero = 0, skin_cpu_far = 0;
    uint64_t skin_other_type = 0;       // ... with a weight or index type not decoded here
    // Run 50: sampled draws whose banks, when recorded, differ from the front end's at submit (vertex c0-c63, the
    // bone palette c64-c255, pixel), and the constant blocks written whole (versions) instead of pushed inline.
    uint64_t constants_checked = 0;
    uint64_t constants_differ[3] = {};
    uint64_t vertex_versions = 0, pixel_versions = 0;
  } gaps_;
  uint32_t constants_logged_ = 0;
  std::unordered_set<uint64_t> skin_logged_;      // (vertex shader, weight type, index type) logged once each
  uint32_t data_changed_logged_ = 0;
  uint32_t skinned_changed_logged_ = 0;  // run 59
  std::unordered_set<uint64_t> missing_inputs_logged_;  // (vertex shader hash ^ location) of inputs without an element
  // The packed depth alias (dk_depth_alias.h): conversions made, fetches served by one already made, conversions that
  // failed (image, descriptor or upload), colour/depth mix-ups that are not an A8R8G8B8 fetch of a depth of its size.
  struct Aliases {
    uint64_t converted = 0, reused = 0, failed = 0, other_mismatch = 0;
    // The title's own registrations (run 39: the fetch of the alias carries the size of a 32x32 placeholder, so the
    // size test of the by-address route never held): registered, fetches served through one, fetches of a
    // registered alias whose depth had not been resolved yet, fetches of one in another format than A8R8G8B8.
    uint64_t registered = 0, by_registration = 0, source_missing = 0, other_format = 0;
  } alias_stats_;
  // The depth/stencil clears that ride on colour resolves (native_resolve_clear.h): done, not done because the depth
  // surface could not be made, refused because the command carries a clear-format override.
  struct ResolveClears {
    uint64_t depth_done = 0, depth_missing = 0, override_skipped = 0;
  } resolve_clears_;
  std::unordered_map<uint32_t, uint32_t> alias_sources_;  // alias texture object -> resolved depth destination object
  std::unordered_set<uint32_t> mismatch_logged_;           // fetch handles whose depth/colour mix-up was logged
  std::unordered_set<uint32_t> swizzles_logged_;           // (fetch swizzle, host format) of resolved colour fetches logged
  uint64_t record_ns_ = 0;
  // Where the recorder's time of a drawn draw goes (run 38: ~275 us per draw at 2700 draws per frame, nothing says
  // where): the textures (incl. the packed depth alias passes), the vertices and indices copied into the upload ring,
  // the constants. Totals, and their values at the last report.
  uint64_t time_textures_ns_ = 0, time_vertices_ns_ = 0, time_constants_ns_ = 0;
  uint64_t time_indices_ns_ = 0, time_index_scan_ns_ = 0;  // the index copy; the index range scan (before the textures)
  // Run 53: the vertices split: geometry store lookups (hash included) and ring copies, with their streams and bytes;
  // the waits for a free work slot (Core::NsWaitingForFences), all as totals with the last report's values.
  uint64_t time_store_ns_ = 0, time_ring_vertices_ns_ = 0;
  uint64_t index_hits_ = 0, index_copies_ = 0, report_index_hits_ = 0, report_index_copies_ = 0;
  uint64_t stream_hits_ = 0, stream_hit_bytes_ = 0, stream_copies_ = 0, stream_copy_bytes_ = 0;
  uint64_t report_store_ns_ = 0, report_ring_vertices_ns_ = 0, report_fence_ns_ = 0;
  uint64_t report_stream_hits_ = 0, report_stream_hit_bytes_ = 0, report_stream_copies_ = 0, report_stream_copy_bytes_ = 0;
  uint64_t report_record_ns_ = 0, report_textures_ns_ = 0, report_vertices_ns_ = 0, report_constants_ns_ = 0,
           report_drawn_ = 0, report_indices_ns_ = 0, report_index_scan_ns_ = 0;
  // Vertex and index data are converted here (cached memory) before they are copied into the upload ring.
  std::vector<uint8_t> vertex_scratch_, index_scratch_;
  GeometryStore geometry_;  // run 52
  uint32_t skin_cpu_logged_ = 0;  // run 53
  uint64_t depth_only_draws_ = 0, biased_draws_ = 0;  // run 55: drawn without their pixel shader; with the depth bias
  uint64_t report_depth_only_ = 0, report_biased_ = 0;
  // Run 56: draws with a pruned vertex shader (position only, live outputs), without one in the package, refused (the
  // pixel shader declares a constant-zero output); variants loaded.
  uint64_t pruned_position_draws_ = 0, pruned_live_draws_ = 0, pruned_missing_draws_ = 0, pruned_refused_draws_ = 0;
  uint64_t pruned_loaded_ = 0;
  uint64_t report_pruned_[4] = {};
  // Run 54: the title's texture locks of GPU-produced textures, copied back into guest memory.
  struct LockStats {
    uint64_t locks = 0, not_gpu = 0, unsupported = 0, layout_mismatch = 0, copied = 0, bytes = 0, ns = 0;
  } lock_stats_;
  DkMemBlock lock_block_ = nullptr;
  // Run 54: the GPU's own values of skinned draws' vertex shaders (dk_shader_probe.h), saved for probe_compare.py.
  struct Probe {
    enum class State { kCompiling, kReady, kFailed } state = State::kCompiling;
    uint64_t hash = 0;
    uint32_t value = 0;
    std::string glsl;
    std::vector<ProbeVariable> vars;
    DkShader shader{};
    uint32_t captures = 0;
  };
  struct PendingProbe {
    uint64_t work = 0;
    DkMemBlock block = nullptr;
    uint32_t bytes = 0;
    std::vector<uint8_t> file;  // everything but the GPU's values, which are appended once the work is done
    std::string name;
    uint32_t position_offset = 0, positions = 0;  // run 55: gl_Position of every vertex, after the values in the block
    std::vector<uint8_t> tail;                    // run 55: the draw (indices, viewport, depth state), after the positions
  };
  std::unordered_map<uint64_t, Probe> probes_;
  std::mutex probe_mutex_;
  std::unordered_map<uint64_t, Compiler::Result> probe_results_;  // filled on the compiler's thread
  std::vector<PendingProbe> pending_probes_;
  uint32_t probe_files_ = 0, probe_started_ = 0;
  void HarvestProbes();
  // Run 60 (dk_ab.h kProbeShaders): skinned draws drawn with their probe's shader; its storage-buffer writes go to a
  // scratch block nobody reads. Counted: draws swapped, skinned draws whose shader had no ready probe; the first state of
  // each skinned G-buffer vertex shader is logged once (depth, stencil, cull, clip plane, alpha test, viewport).
  bool ab_probe_shaders_ = false;
  bool ab_skinned_depth_always_ = false;  // run 61: the A/B's "skinned depth always" phase
  uint64_t depth_always_draws_ = 0;       // skinned G-buffer draws drawn with the depth compare "always" (run 61)
  DkMemBlock probe_scratch_ = nullptr;
  static constexpr uint32_t kProbeScratchBytes = 1u << 20;
  uint64_t probe_swapped_ = 0, probe_unswapped_ = 0;
  std::unordered_set<uint64_t> probe_swap_logged_, skinned_state_logged_, rect_logged_;
  uint32_t lock_bytes_ = 0;
  std::unordered_set<uint32_t> lock_logged_;
  DkMemBlock geometry_block_ = nullptr;
  uint64_t report_frames_ = 0, report_draws_ = 0, report_ns_ = 0;
  // Run 62 (dk_stage_counters.h): the GPU's own counters of each pipeline stage around sampled G-buffer draws, the
  // characters' against the scenery's. The samples of each slot's last work, read when its fence has been waited for.
  StageCounterLayout stage_layout_;
  StageSampler stage_sampler_;
  std::vector<std::vector<StageSample>> stage_slot_samples_;
  // By A/B phase (all cycles; the last entry: before the A/B began or without it) and over the whole session.
  std::array<StageReport, size_t(AbPhase::kCount) + 1> stage_phase_{};
  StageReport stage_totals_;
  uint64_t stage_report_windows_ = 0;
  // Run 63 (dk_overwrite.h): what writes the characters' surfaces after them in the same frame.
  OverwriteTracker overwrite_;
  void HarvestStageCounters(uint32_t slot);
  void LogStageCounters(const char* what, const StageReport& r);
};

}  // namespace rex::graphics::gta4_native::dk
