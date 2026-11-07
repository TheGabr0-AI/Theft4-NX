// gta4_native deko3d back end: the core. See dk_core.h (ported from nfsmw-nx's nfsmw_dk_nucleo.cpp, GPL-3.0).

#include "dk_core.h"

#include <switch.h>

#include <rex/logging.h>
#include <rex/logging/api.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rex::graphics::gta4_native::dk {
namespace {

constexpr uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// Command memory goes to the command buffer in pieces of this size, each with kCommandSlack bytes at its end that
// deko3d is not told about (dkCmdBufBindVtxBuffers in 0.5.0 reserves 5 words per buffer and writes 6).
constexpr uint32_t kCommandPiece = 64u << 10;
constexpr uint32_t kCommandSlack = 256;

DebugCounters g_debug_counters;

// deko3d's messages: only the debug library (libdeko3dd) calls this, for every check it makes.
void DebugMessage(void*, const char* context, DkResult result, const char* message) {
  static std::atomic<uint32_t> logged{0};
  if (result != DkResult_Success) {
    g_debug_counters.errors.fetch_add(1);
  } else {
    g_debug_counters.warnings.fetch_add(1);
  }
  const uint32_t n = logged.fetch_add(1);
  if (n < 200) {
    REXLOG_ERROR("deko3d: libdeko3dd {} in {}: result {}: {}", result != DkResult_Success ? "ERROR" : "warning",
                 context ? context : "?", int(result), message ? message : "");
    rex::FlushLogging();
  } else if (n == 200) {
    REXLOG_ERROR("deko3d: libdeko3dd: more messages, not logged");
  }
}

void AddCommandMemory(void* core, DkCmdBuf which, size_t minimum) {
  static_cast<Core*>(core)->MoreCommandMemory(which, minimum);
}

}  // namespace

DebugCounters& GetDebugCounters() { return g_debug_counters; }

// ---- Device and queue

Device::~Device() { Destroy(); }

bool Device::Create(bool zcull) {
  REXLOG_INFO("deko3d: creating the device (origin upper left, depth 0..1)");
  rex::FlushLogging();
  DkDeviceMaker dm;
  dkDeviceMakerDefaults(&dm);
  dm.cbDebug = DebugMessage;
  dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
  device_ = dkDeviceCreate(&dm);
  if (!device_) {
    REXLOG_ERROR("deko3d: dkDeviceCreate failed");
    return false;
  }
  DkQueueMaker qm;
  dkQueueMakerDefaults(&qm, device_);
  qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio |
             (zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
  qm.commandMemorySize = 1u << 20;
  qm.flushThreshold = qm.commandMemorySize / 8;
  qm.perWarpScratchMemorySize = kQueueScratchBytes;  // the default 2048 is below what 27 vertex programs declare
  REXLOG_INFO("deko3d: creating the queue (graphics, ZCULL {}, {} KiB of scratch per warp)", zcull ? "on" : "off",
              kQueueScratchBytes >> 10);
  rex::FlushLogging();
  queue_ = dkQueueCreate(&qm);
  if (!queue_) {
    REXLOG_ERROR("deko3d: dkQueueCreate failed");
    return false;
  }
  return true;
}

void Device::Destroy() {
  if (!device_) {
    return;
  }
  if (queue_) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!dkQueueIsInErrorState(queue_)) {
      dkQueueWaitIdle(queue_);
    }
    dkQueueDestroy(queue_);
    queue_ = nullptr;
  }
  if (const uint32_t alive = live_blocks_.load()) {
    // The debug library ends the process at dkDeviceDestroy when a block is left (run 35).
    REXLOG_ERROR("deko3d: {} memory blocks still alive at dkDeviceDestroy", alive);
    rex::FlushLogging();
  }
  dkDeviceDestroy(device_);
  device_ = nullptr;
}

DkMemBlock Device::CreateBlock(uint32_t bytes, uint32_t flags, const char* name) {
  bytes = Align(bytes, DK_MEMBLOCK_ALIGNMENT);
  std::lock_guard<std::mutex> lock(create_mutex_);
  REXLOG_INFO("deko3d: memory block {}, {} KiB (flags 0x{:X}; {} MB in blocks before it)", name, bytes >> 10, flags,
              block_bytes_.load() >> 20);
  rex::FlushLogging();  // a failed creation ends the process inside deko3d
  DkMemBlockMaker m;
  dkMemBlockMakerDefaults(&m, device_, bytes);
  m.flags = flags;
  DkMemBlock block = dkMemBlockCreate(&m);
  if (block) {
    live_blocks_.fetch_add(1);
    block_bytes_.fetch_add(bytes);
  }
  return block;
}

void Device::DestroyBlock(DkMemBlock block) {
  if (block) {
    std::lock_guard<std::mutex> lock(create_mutex_);
    block_bytes_.fetch_sub(dkMemBlockGetSize(block));
    dkMemBlockDestroy(block);
    live_blocks_.fetch_sub(1);
  }
}

DkSwapchain Device::CreateSwapchain(NWindow* window, const DkImage* const images[], uint32_t count) {
  std::lock_guard<std::mutex> lock(create_mutex_);
  rex::FlushLogging();
  DkSwapchainMaker sm;
  dkSwapchainMakerDefaults(&sm, device_, window ? window : nwindowGetDefault(), images, count);
  return dkSwapchainCreate(&sm);
}

void Device::DestroySwapchain(DkSwapchain swapchain) {
  if (swapchain) {
    std::lock_guard<std::mutex> lock(create_mutex_);
    dkSwapchainDestroy(swapchain);
  }
}

bool Device::QueueHealthy(const char* before) {
  if (dkQueueIsInErrorState(queue_)) {
    if (!lost_.exchange(true)) {
      REXLOG_ERROR("deko3d: the queue is in an error state (a GPU fault) before {}: nothing more is submitted",
                   before);
      rex::FlushLogging();
    }
    return false;
  }
  return true;
}

void Device::Submit(DkCmdList list, const char* what) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!QueueHealthy(what)) {
    return;
  }
  dkQueueSubmitCommands(queue_, list);
  dkQueueFlush(queue_);
}

void Device::Submit(DkCmdList first, DkCmdList second, const char* what) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!QueueHealthy(what)) {
    return;
  }
  dkQueueSubmitCommands(queue_, first);
  dkQueueSubmitCommands(queue_, second);
  dkQueueFlush(queue_);
}

void Device::WaitIdle() {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (QueueHealthy("waiting for the GPU")) {
    dkQueueWaitIdle(queue_);
  }
}

int Device::Acquire(DkSwapchain swapchain) {
  // The acquire may wait for the window to give an image back: without the queue lock; only the GPU wait for that
  // image goes into the queue.
  int image = 0;
  DkFence fence;
  dkSwapchainAcquireImage(swapchain, &image, &fence);
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (QueueHealthy("waiting for a swapchain image")) {
    dkQueueWaitFence(queue_, &fence);
  }
  return image;
}

void Device::Present(DkSwapchain swapchain, int image) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (QueueHealthy("presenting")) {
    dkQueuePresentImage(queue_, swapchain, image);
  }
}

// ---- One recording thread's command buffer and memory

Core::~Core() { Destroy(); }

bool Core::Create(Device& device, const CoreConfig& config) {
  device_ = &device;
  config_ = config;
  config_.slots = std::max<uint32_t>(config_.slots, 1);
  config_.command_kb = std::max<uint32_t>(Align(config_.command_kb, kCommandPiece >> 10), kCommandPiece >> 10);

  DkCmdBufMaker cm;
  dkCmdBufMakerDefaults(&cm, device_->device());
  cm.userData = this;
  cm.cbAddMem = AddCommandMemory;
  commands_ = dkCmdBufCreate(&cm);
  if (config_.pre_kb) {
    config_.pre_kb = std::max<uint32_t>(Align(config_.pre_kb, kCommandPiece >> 10), kCommandPiece >> 10);
    pre_ = dkCmdBufCreate(&cm);
  }
  constexpr uint32_t kUncachedCpu = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
  const uint32_t n = config_.slots;
  command_memory_ = device_->CreateBlock(n * (config_.command_kb << 10), kUncachedCpu, "command memory (per slot)");
  if (pre_) {
    pre_memory_ = device_->CreateBlock(n * (config_.pre_kb << 10), kUncachedCpu, "pre-list memory (per slot)");
  }
  upload_ = device_->CreateBlock(n * (config_.upload_mb << 20), kUncachedCpu, "upload ring (per slot)");
  code_ = device_->CreateBlock(config_.code_mb << 20, kUncachedCpu | DkMemBlockFlags_Code, "shader code");
  descriptors_ = device_->CreateBlock((config_.image_descriptors + config_.sampler_descriptors) * 32, kUncachedCpu,
                                      "image and sampler descriptors");
  queries_ = device_->CreateBlock(config_.query_kb << 10, kUncachedCpu, "GPU reports");
  if (!commands_ || !command_memory_ || (config_.pre_kb && (!pre_ || !pre_memory_)) || !upload_ || !code_ ||
      !descriptors_ || !queries_) {
    REXLOG_ERROR("deko3d: the core's memory was not created");
    return false;
  }
  upload_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(upload_));
  upload_gpu_ = dkMemBlockGetGpuAddr(upload_);
  queries_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(queries_));
  queries_gpu_ = dkMemBlockGetGpuAddr(queries_);
  std::memset(queries_cpu_, 0, config_.query_kb << 10);
  std::memset(dkMemBlockGetCpuAddr(descriptors_), 0,
              size_t(config_.image_descriptors + config_.sampler_descriptors) * 32);
  slots_.assign(n, Slot{});
  AddChunk(config_.image_chunk_mb << 20);
  REXLOG_INFO("deko3d: core ready: {} slots, {} MiB of uploads, {} KiB of commands and {} KiB of pre-list per slot, "
              "{} MiB of shader code",
              n, config_.upload_mb, config_.command_kb, pre_ ? config_.pre_kb : 0, config_.code_mb);
  return true;
}

void Core::Destroy() {
  if (!device_) {
    return;
  }
  device_->WaitIdle();
  if (commands_) {
    dkCmdBufDestroy(commands_);
  }
  if (pre_) {
    dkCmdBufDestroy(pre_);
  }
  for (Slot& s : slots_) {
    for (SlotMemory* m : {&s.commands, &s.pre}) {
      for (auto& [block, bytes] : m->overflow) {
        device_->DestroyBlock(block);
      }
    }
  }
  slots_.clear();
  for (Chunk& c : chunks_) {
    device_->DestroyBlock(c.block);
  }
  chunks_.clear();
  for (DkMemBlock b : readback_blocks_) {
    device_->DestroyBlock(b);
  }
  readback_blocks_.clear();
  for (DkMemBlock b : {command_memory_, pre_memory_, upload_, code_, descriptors_, queries_}) {
    device_->DestroyBlock(b);
  }
  device_ = nullptr;
  commands_ = pre_ = nullptr;
  command_memory_ = pre_memory_ = upload_ = code_ = descriptors_ = queries_ = nullptr;
}

void Core::BeginWork(uint64_t work) {
  slot_ = uint32_t(work % config_.slots);
  Slot& s = slots_[slot_];
  if (s.fenced) {
    const uint64_t before = NowNs();
    for (int wait = 1;; ++wait) {
      if (dkFenceWait(&s.fence, 2'000'000'000ll) == DkResult_Success) {
        break;
      }
      if (!device_->QueueHealthy("waiting for a slot's fence")) {
        break;
      }
      REXLOG_WARN("deko3d: work {}: the GPU has not finished slot {} after {} s", work, slot_, wait * 2);
      rex::FlushLogging();
    }
    ns_waiting_ += NowNs() - before;
  }
  for (const ImageMemory& m : s.to_release) {
    ReturnToChunk(chunks_[size_t(m.chunk)], m.offset, m.bytes);
  }
  s.to_release.clear();
  s.upload_used = 0;
  // dkCmdBufClear rewinds to the start of the last piece given (deko3d 0.5.0), which belongs to another slot:
  // the slot's first piece is given again explicitly, as deko_examples' CCmdMemRing does.
  dkCmdBufClear(commands_);
  dkCmdBufAddMemory(commands_, command_memory_, slot_ * (config_.command_kb << 10), kCommandPiece - kCommandSlack);
  s.commands.used = kCommandPiece;
  s.commands.overflow_used = 0;
  if (pre_) {
    dkCmdBufClear(pre_);
    dkCmdBufAddMemory(pre_, pre_memory_, slot_ * (config_.pre_kb << 10), kCommandPiece - kCommandSlack);
    s.pre.used = kCommandPiece;
    s.pre.overflow_used = 0;
  }
  pre_used_ = false;
  commands_this_work_ = kCommandPiece;
  open_ = true;
}

void Core::MoreCommandMemory(DkCmdBuf which, size_t minimum) {
  Slot& s = slots_[slot_];
  const bool pre = pre_ && which == pre_;
  SlotMemory& m = pre ? s.pre : s.commands;
  DkMemBlock memory = pre ? pre_memory_ : command_memory_;
  const uint32_t want =
      Align(uint32_t(std::max<size_t>(minimum + kCommandSlack, kCommandPiece)), DK_MEMBLOCK_ALIGNMENT);
  const uint32_t slice = (pre ? config_.pre_kb : config_.command_kb) << 10;
  if (m.used + want <= slice) {
    dkCmdBufAddMemory(which, memory, slot_ * slice + m.used, want - kCommandSlack);
    m.used += want;
    commands_this_work_ += want;
    return;
  }
  if (m.overflow_used == 0) {
    ++stats_.command_overflows;
  }
  while (m.overflow_used < m.overflow.size()) {
    const auto [block, bytes] = m.overflow[m.overflow_used++];
    if (bytes >= want) {
      dkCmdBufAddMemory(which, block, 0, bytes - kCommandSlack);
      commands_this_work_ += bytes;
      return;
    }
  }
  const uint32_t bytes = std::max<uint32_t>(want, 1u << 20);
  DkMemBlock block = device_->CreateBlock(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                          pre ? "pre-list overflow" : "command memory overflow");
  m.overflow.push_back({block, bytes});
  m.overflow_used = m.overflow.size();
  dkCmdBufAddMemory(which, block, 0, bytes - kCommandSlack);
  commands_this_work_ += bytes;
}

DkCmdList Core::ClosePre() {
  if (!pre_ || !pre_used_) {
    return 0;
  }
  // Uploads, descriptor writes and code are done and visible before commands() runs.
  dkCmdBufBarrier(pre_, DkBarrier_Full,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
  pre_used_ = false;
  return dkCmdBufFinishList(pre_);
}

void Core::EndWork() {
  Slot& s = slots_[slot_];
  const DkCmdList pre = ClosePre();
  dkCmdBufSignalFence(commands_, &s.fence, true);
  s.fenced = true;
  if (pre) {
    device_->Submit(pre, dkCmdBufFinishList(commands_), "submitting a work");
  } else {
    device_->Submit(dkCmdBufFinishList(commands_), "submitting a work");
  }
  open_ = false;
  ++stats_.works;
  stats_.upload_bytes += s.upload_used;
  stats_.command_bytes_max = std::max(stats_.command_bytes_max, commands_this_work_);
}

bool Core::WorkDone(uint64_t work) {
  Slot& s = slots_[work % config_.slots];
  return !s.fenced || dkFenceWait(&s.fence, 0) == DkResult_Success;
}

Upload Core::Reserve(uint32_t bytes, uint32_t alignment) {
  Slot& s = slots_[slot_];
  const uint32_t slice = config_.upload_mb << 20;
  const uint32_t offset = Align(s.upload_used, std::max<uint32_t>(alignment, 4));
  if (!open_ || uint64_t(offset) + bytes > slice) {
    if (stats_.upload_full++ < 20) {
      REXLOG_WARN("deko3d: upload: {} bytes do not fit in slot {} ({} of {} MiB used){}", bytes, slot_,
                  s.upload_used >> 20, config_.upload_mb, open_ ? "" : " (no open work)");
    }
    return {};
  }
  s.upload_used = offset + bytes;
  const uint32_t at = slot_ * slice + offset;
  return {upload_cpu_ + at, upload_gpu_ + at};
}

uint32_t Core::UploadFree() const {
  if (!open_) {
    return 0;
  }
  const uint32_t slice = config_.upload_mb << 20;
  const uint32_t used = slots_[slot_].upload_used;
  return used < slice ? slice - used : 0;
}

DkMemBlock Core::CreateReadbackBlock(uint32_t bytes, const char* name) {
  DkMemBlock b = device_->CreateBlock(bytes, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached, name);
  if (b) {
    readback_blocks_.push_back(b);
  }
  return b;
}

void Core::DestroyReadbackBlock(DkMemBlock block) {
  const auto it = std::find(readback_blocks_.begin(), readback_blocks_.end(), block);
  if (it == readback_blocks_.end()) {
    return;  // not ours (or already gone): never destroyed twice
  }
  readback_blocks_.erase(it);
  device_->DestroyBlock(block);
}

// ---- Image heap: first fit in chunks, frees delayed until the slot's fence

void Core::AddChunk(uint32_t bytes) {
  char name[48];
  std::snprintf(name, sizeof(name), "image chunk %zu", chunks_.size());
  bytes = Align(bytes, DK_MEMBLOCK_ALIGNMENT);
  Chunk c;
  c.block = device_->CreateBlock(bytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, name);
  if (!c.block) {
    return;
  }
  c.bytes = bytes;
  c.free.push_back({0, bytes});
  chunks_.push_back(std::move(c));
  stats_.image_chunks = chunks_.size();
}

void Core::ReturnToChunk(Chunk& c, uint32_t offset, uint32_t bytes) {
  auto it = std::lower_bound(c.free.begin(), c.free.end(), std::make_pair(offset, 0u));
  it = c.free.insert(it, {offset, bytes});
  if (it + 1 != c.free.end() && it->first + it->second == (it + 1)->first) {
    it->second += (it + 1)->second;
    c.free.erase(it + 1);
  }
  if (it != c.free.begin() && (it - 1)->first + (it - 1)->second == it->first) {
    (it - 1)->second += it->second;
    c.free.erase(it);
  }
}

ImageMemory Core::ReserveImage(uint32_t bytes, uint32_t alignment) {
  alignment = std::max<uint32_t>(alignment, 256);
  bytes = Align(bytes, 256);
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < chunks_.size(); ++i) {
      Chunk& c = chunks_[i];
      for (size_t k = 0; k < c.free.size(); ++k) {
        const auto [start, length] = c.free[k];
        const uint32_t at = Align(start, alignment);
        if (uint64_t(at - start) + bytes > length) {
          continue;
        }
        c.free.erase(c.free.begin() + std::ptrdiff_t(k));
        if (at > start) {
          ReturnToChunk(c, start, at - start);
        }
        if (at + bytes < start + length) {
          ReturnToChunk(c, at + bytes, start + length - at - bytes);
        }
        stats_.image_bytes += bytes;
        return {c.block, at, bytes, int32_t(i)};
      }
    }
    AddChunk(std::max(config_.image_chunk_mb << 20, Align(bytes + alignment, DK_MEMBLOCK_ALIGNMENT)));
  }
  REXLOG_ERROR("deko3d: the image heap cannot place {} bytes", bytes);
  return {};
}

void Core::ReleaseImageLater(const ImageMemory& memory) {
  if (memory.chunk < 0) {
    return;
  }
  stats_.image_bytes -= memory.bytes;
  slots_[slot_].to_release.push_back(memory);
}

// ---- Shader code

bool Core::LoadShader(DkShader& shader, std::span<const uint8_t> dksh, const char* name) {
  // The DKSH header (uam's dksh.h): the control section (starting with this header) stays in CPU memory, the code
  // that follows it goes to code memory.
  DkshProgram h;
  if (!ParseDksh(dksh, h) || h.programs != 1) {
    REXLOG_ERROR("deko3d: shader {}: {} bytes, not a one-program DKSH", name, dksh.size());
    return false;
  }
  // The queue gives every warp kQueueScratchBytes of local memory; a program that declares more would run past it
  // (run 37: seconds per work from the first such vertex shader, and nothing from libdeko3dd).
  if (h.per_warp_scratch > kQueueScratchBytes) {
    ++stats_.scratch_refused;
    REXLOG_ERROR("deko3d: shader {} needs {} bytes of scratch per warp, the queue gives {}: not loaded", name,
                 h.per_warp_scratch, kQueueScratchBytes);
    return false;
  }
  stats_.scratch_max = std::max(stats_.scratch_max, h.per_warp_scratch);
  const uint32_t at = Align(code_used_, DK_SHADER_CODE_ALIGNMENT);
  if (uint64_t(at) + h.code_bytes > (config_.code_mb << 20) - DK_SHADER_CODE_UNUSABLE_SIZE) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("deko3d: shader code memory full ({} MiB): {} ({} bytes) is not loaded", config_.code_mb, name,
                   h.code_bytes);
    }
    return false;
  }
  std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(code_)) + at, dksh.data() + h.control_bytes,
              h.code_bytes);
  DkShaderMaker m;
  dkShaderMakerDefaults(&m, code_, at);
  m.control = dksh.data();
  m.programId = 0;
  dkShaderInitialize(&shader, &m);
  if (!dkShaderIsValid(&shader)) {
    REXLOG_ERROR("deko3d: shader {}: deko3d does not take it", name);
    return false;
  }
  code_used_ = at + h.code_bytes;
  stats_.code_bytes = code_used_;
  code_new_ = true;
  return true;
}

void Core::InvalidateCodeIfNeeded() {
  if (code_new_) {
    dkCmdBufBarrier(commands_, DkBarrier_None, DkInvalidateFlags_Shader);
    code_new_ = false;
  }
}

DkGpuAddr Core::ImageDescriptors() const { return dkMemBlockGetGpuAddr(descriptors_); }
DkGpuAddr Core::SamplerDescriptors() const {
  return dkMemBlockGetGpuAddr(descriptors_) + DkGpuAddr(config_.image_descriptors) * 32;
}
void Core::BindDescriptors() {
  dkCmdBufBindImageDescriptorSet(commands_, ImageDescriptors(), config_.image_descriptors);
  dkCmdBufBindSamplerDescriptorSet(commands_, SamplerDescriptors(), config_.sampler_descriptors);
}

CoreStats Core::TakeStats() {
  const CoreStats s = stats_;
  stats_.works = stats_.upload_bytes = stats_.upload_full = 0;
  stats_.command_bytes_max = stats_.command_overflows = 0;
  return s;
}

// ---- Images

void FixTileSize(DkImageLayoutMaker& m, uint32_t rows) {
  if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) {
    return;
  }
  if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D ||
      m.type == DkImageType_Buffer) {
    return;
  }
  if ((m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) &&
      (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x)) {
    rows *= 2;
  }
  // deko3d's choice (1.5 times the height in 8-row GOBs) and the shrink its layout applies to level 0, which the
  // descriptor and the engines do not.
  const uint32_t gobs = (rows + rows / 2 + 7) / 8;
  const uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
  uint32_t shrunk = tile;
  while (shrunk && (8u << (shrunk - 1)) >= rows) {
    --shrunk;
  }
  if (shrunk == tile) {
    return;
  }
  m.flags |= DkImageFlags_CustomTileSize;
  m.tileSize = DkTileSize(shrunk);
}

// ---- Swapchain

Swapchain::~Swapchain() { Destroy(); }

bool Swapchain::Create(Device& device, uint32_t width, uint32_t height, uint32_t interval, NWindow* window) {
  Destroy();
  device_ = &device;
  width_ = width;
  height_ = height;
  DkImageLayoutMaker lm;
  dkImageLayoutMakerDefaults(&lm, device.device());
  lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
  lm.format = DkImageFormat_RGBA8_Unorm;
  lm.dimensions[0] = width;
  lm.dimensions[1] = height;
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &lm);
  const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
  const uint32_t bytes = Align(uint32_t(dkImageLayoutGetSize(&layout)), alignment);
  REXLOG_INFO("deko3d: swapchain of {} RGBA8 images of {}x{}, swap interval {}", kImages, width, height, interval);
  memory_ = device.CreateBlock(kImages * bytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "swapchain images");
  if (!memory_) {
    return false;
  }
  const DkImage* images[kImages];
  for (uint32_t i = 0; i < kImages; ++i) {
    dkImageInitialize(&images_[i], &layout, memory_, i * bytes);
    images[i] = &images_[i];
  }
  swapchain_ = device.CreateSwapchain(window, images, kImages);
  if (!swapchain_) {
    return false;
  }
  dkSwapchainSetSwapInterval(swapchain_, interval);
  return true;
}

void Swapchain::Destroy() {
  if (!device_) {
    return;
  }
  device_->WaitIdle();
  device_->DestroySwapchain(swapchain_);
  device_->DestroyBlock(memory_);
  swapchain_ = nullptr;
  memory_ = nullptr;
  device_ = nullptr;
}

int Swapchain::Acquire() { return device_->Acquire(swapchain_); }

void Swapchain::Present(int image) { device_->Present(swapchain_, image); }

}  // namespace rex::graphics::gta4_native::dk
