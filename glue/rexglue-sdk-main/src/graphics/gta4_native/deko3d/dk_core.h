// gta4_native deko3d back end: the core. Device, queue, memory, work slots, shader code and swapchain
// (docs/switch-port/16-deko3d-devlog.md, M3). Switch only.
//
// Ported from nfsmw-nx (StevensND and contributors, GPL-3.0), app/src/nfsmw_dk_nucleo.{h,cpp} at 266f943
// (Dispositivo, Nucleo, Presentacion, ArreglarTamanoDeTesela), with its rules:
//   - device conventions DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne: row 0 is the top and
//     clip-space z goes from 0 to 1, as in Direct3D; clip-space y points up (Direct3D's), so the shaders are
//     compiled without -fvk-invert-y and no viewport ever gets a negative height (a flip is a viewport swizzle);
//   - command memory is given to deko3d in 64 KB pieces, each with 256 bytes at its end deko3d is not told about
//     (dkCmdBufBindVtxBuffers in 0.5.0 writes one word past what it reserves per buffer);
//   - texture uploads and descriptor writes go into the slot's pre-list, which runs before the slot's commands
//     and ends with a barrier that invalidates the image, descriptor and shader caches;
//   - descriptors are written with dkCmdBufPushData only, ordered with the commands;
//   - every creation flushes the log first: deko3d's release library ends the process on a failed creation.
// What changed: English names, REXLOG for the log, the debug library's messages logged (an error is logged and
// the process is not ended here: the caller sees the failure), counters for the "deko3d:" report lines.

#pragma once

#include <deko3d.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

struct NWindow;

namespace rex::graphics::gta4_native::dk {

// The CPU counter (19.2 MHz) in nanoseconds: cheap enough for per-draw timers.
inline uint64_t NowNs() {
  uint64_t ticks;
  asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
  return ticks * 625 / 12;
}

// GPU timestamps count 1.627 ns per unit on the Switch (nfsmw-nx docs/measuring.md).
inline constexpr double kNsPerGpuUnit = 1.627;

// deko3d's debug messages and errors since start (the debug library only).
struct DebugCounters {
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> warnings{0};
};
DebugCounters& GetDebugCounters();

// The device and its one queue. Every queue operation takes the lock.
class Device {
 public:
  Device() = default;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  ~Device();

  bool Create(bool zcull);
  // Waits for the GPU and destroys the queue and the device. Safe to call twice.
  void Destroy();

  DkDevice device() const { return device_; }
  DkQueue queue() const { return queue_; }

  DkMemBlock CreateBlock(uint32_t bytes, uint32_t flags, const char* name);
  void DestroyBlock(DkMemBlock block);
  DkSwapchain CreateSwapchain(NWindow* window, const DkImage* const images[], uint32_t count);
  void DestroySwapchain(DkSwapchain swapchain);
  // Blocks made and not destroyed yet (the teardown logs them before dkDeviceDestroy).
  uint32_t live_blocks() const { return live_blocks_.load(); }
  uint64_t block_bytes() const { return block_bytes_.load(); }

  void Submit(DkCmdList list, const char* what);
  void Submit(DkCmdList first, DkCmdList second, const char* what);
  void WaitIdle();
  int Acquire(DkSwapchain swapchain);
  void Present(DkSwapchain swapchain, int image);
  // False (logged once) when a GPU fault left the queue in an error state.
  bool QueueHealthy(const char* before);
  bool lost() const { return lost_.load(); }

 private:
  DkDevice device_ = nullptr;
  DkQueue queue_ = nullptr;
  std::mutex queue_mutex_;
  std::mutex create_mutex_;
  std::atomic<uint32_t> live_blocks_{0};
  std::atomic<uint64_t> block_bytes_{0};
  std::atomic<bool> lost_{false};
};

struct CoreConfig {
  uint32_t slots = 3;               // work slots: the CPU records one while the GPU runs the others
  uint32_t upload_mb = 32;          // per slot: vertices, indices, constants, texture uploads
  uint32_t command_kb = 2048;       // per slot (grows past this with extra blocks)
  uint32_t pre_kb = 512;            // per slot: the pre-list (0 = none)
  uint32_t code_mb = 16;            // shader code, bump allocated
  uint32_t image_chunk_mb = 32;     // image heap chunks
  uint32_t image_descriptors = 8192;
  uint32_t sampler_descriptors = 1024;
  uint32_t query_kb = 64;           // GPU reports (timestamps)
};

// A suballocation of the current slot's upload ring.
struct Upload {
  uint8_t* cpu = nullptr;
  DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
  explicit operator bool() const { return cpu != nullptr; }
};

// A suballocation of the image heap.
struct ImageMemory {
  DkMemBlock block = nullptr;
  uint32_t offset = 0;
  uint32_t bytes = 0;
  int32_t chunk = -1;
  explicit operator bool() const { return block != nullptr; }
};

struct CoreStats {
  uint64_t works = 0;            // slots closed since the last call
  uint64_t upload_bytes = 0;     // upload ring bytes used over those slots
  uint64_t upload_full = 0;      // reservations that did not fit
  uint64_t command_bytes_max = 0;  // the most command memory one slot needed
  uint64_t command_overflows = 0;  // slots that needed more than command_kb
  uint64_t image_bytes = 0;      // image heap in use (totals)
  uint64_t image_chunks = 0;
  uint64_t code_bytes = 0;
};

// One recording thread's command buffer, work slots, upload ring, image heap, code and descriptors.
class Core {
 public:
  Core() = default;
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core();

  bool Create(Device& device, const CoreConfig& config);
  void Destroy();

  DkDevice device() const { return device_ ? device_->device() : nullptr; }
  Device& owner() const { return *device_; }
  DkCmdBuf commands() const { return commands_; }
  DkCmdBuf PreCommands() {
    pre_used_ = true;
    return pre_;
  }
  const CoreConfig& config() const { return config_; }

  // Starts recording work number `work` in slot work % slots: waits for that slot's previous fence, frees the
  // image memory released while it was recorded, rewinds its upload ring and its command memory.
  void BeginWork(uint64_t work);
  // Signals the slot's fence after everything recorded, submits it and flushes the queue.
  void EndWork();
  bool WorkOpen() const { return open_; }
  bool WorkDone(uint64_t work);
  uint64_t NsWaitingForFences() const { return ns_waiting_; }
  void WaitIdle() { device_->WaitIdle(); }

  // `bytes` of the current slot's upload ring (CPU-uncached: written once, in order). Empty when full.
  Upload Reserve(uint32_t bytes, uint32_t alignment);
  uint32_t UploadFree() const;
  ImageMemory ReserveImage(uint32_t bytes, uint32_t alignment);
  // Returns it to the heap once the GPU has finished the slot being recorded now.
  void ReleaseImageLater(const ImageMemory& memory);
  // CPU-cached, GPU-uncached memory the CPU reads back (flush its cache before the GPU writes and before reading).
  DkMemBlock CreateReadbackBlock(uint32_t bytes, const char* name);

  // Copies a DKSH's code into code memory and initializes `shader` from it.
  bool LoadShader(DkShader& shader, std::span<const uint8_t> dksh, const char* name);
  void InvalidateCodeIfNeeded();

  DkGpuAddr ImageDescriptors() const;
  DkGpuAddr SamplerDescriptors() const;
  void BindDescriptors();

  uint8_t* QueriesCpu() const { return queries_cpu_; }
  DkGpuAddr QueriesGpu() const { return queries_gpu_; }

  CoreStats TakeStats();
  void MoreCommandMemory(DkCmdBuf which, size_t minimum);

 private:
  struct SlotMemory {
    uint32_t used = 0;
    std::vector<std::pair<DkMemBlock, uint32_t>> overflow;
    size_t overflow_used = 0;
  };
  struct Slot {
    DkFence fence{};
    bool fenced = false;
    uint32_t upload_used = 0;
    SlotMemory commands, pre;
    std::vector<ImageMemory> to_release;
  };
  struct Chunk {
    DkMemBlock block = nullptr;
    uint32_t bytes = 0;
    std::vector<std::pair<uint32_t, uint32_t>> free;  // offset, bytes; sorted by offset
  };

  DkCmdList ClosePre();
  void AddChunk(uint32_t bytes);
  static void ReturnToChunk(Chunk& chunk, uint32_t offset, uint32_t bytes);

  CoreConfig config_;
  Device* device_ = nullptr;
  DkCmdBuf commands_ = nullptr;
  DkCmdBuf pre_ = nullptr;
  bool pre_used_ = false;
  uint64_t ns_waiting_ = 0;
  DkMemBlock pre_memory_ = nullptr;
  DkMemBlock upload_ = nullptr, command_memory_ = nullptr, code_ = nullptr, descriptors_ = nullptr,
             queries_ = nullptr;
  std::vector<DkMemBlock> readback_blocks_;
  uint8_t* upload_cpu_ = nullptr;
  DkGpuAddr upload_gpu_ = 0;
  uint8_t* queries_cpu_ = nullptr;
  DkGpuAddr queries_gpu_ = 0;
  std::vector<Slot> slots_;
  uint32_t slot_ = 0;
  bool open_ = false;
  uint32_t code_used_ = 0;
  bool code_new_ = false;
  std::vector<Chunk> chunks_;
  uint64_t commands_this_work_ = 0;
  CoreStats stats_;
};

// Before dkImageLayoutInitialize of a block-linear 2D image whose level 0 is `rows` rows tall (compressed formats:
// rows of blocks): deko3d 0.5.0 gives images of 6 to 8 rows of 8-row blocks a tile height its layout and its
// descriptor disagree on (nfsmw-nx HowToDeko3d 6.3). This sets the tile its level 0 really uses.
void FixTileSize(DkImageLayoutMaker& maker, uint32_t rows);

// A swapchain of kImages RGBA8 images on `window` (nullptr: the default window), in its own block.
class Swapchain {
 public:
  static constexpr uint32_t kImages = 3;
  Swapchain() = default;
  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;
  ~Swapchain();

  bool Create(Device& device, uint32_t width, uint32_t height, uint32_t interval, NWindow* window = nullptr);
  void Destroy();
  int Acquire();
  void Present(int image);
  DkImage& Image(int i) { return images_[i]; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  bool created() const { return swapchain_ != nullptr; }

 private:
  Device* device_ = nullptr;
  DkSwapchain swapchain_ = nullptr;
  DkMemBlock memory_ = nullptr;
  DkImage images_[kImages];
  uint32_t width_ = 0, height_ = 0;
};

}  // namespace rex::graphics::gta4_native::dk
