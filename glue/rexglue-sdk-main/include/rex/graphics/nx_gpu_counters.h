// Counters of the Vulkan backend that switch-game prints in its heartbeat
// (docs/switch-port/06-game-boot-plan.md section 25, run 19). Plain relaxed
// atomics, read from the UI thread; nothing here allocates or locks.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::graphics {

// Run 31 (C.2): phases of the command processor's work. The draw phases are
// consecutive pieces of VulkanCommandProcessor::IssueDraw; kNxCpCopy is
// IssueCopy (resolves) called from it; kNxCpReplay is the replay of the
// deferred command buffer into the real one (the vkCmd* calls into NVK) at
// the end of a submission. Whatever is left of the thread's CPU time is the
// PM4 parsing, register writes, swaps and the rest of the packet loop.
enum NxCpPhase : int {
  kNxCpDrawPrimitives = 0,  // BeginSubmission, primitive and index processing
  kNxCpDrawShaders,         // ucode analysis, modifications, translation lookup, samplers
  kNxCpDrawTextures,        // texture cache RequestTextures (uploads, untiling dispatches)
  kNxCpDrawRenderTargets,   // render target cache Update and pending draw-pass transfers (EDRAM)
  kNxCpDrawPipeline,        // ConfigurePipeline, bind, layout compatibility
  kNxCpDrawState,           // viewport, dynamic state, system constants, UpdateBindings
  kNxCpDrawMemory,          // vertex buffers, memexport, shared memory use, barriers, render pass
  kNxCpDrawRecord,          // the draw itself into the deferred command buffer, memexport tail
  kNxCpCopy,
  kNxCpReplay,
  kNxCpPhaseCount
};

struct NxGpuCounters {
  std::atomic<uint64_t> presents{0};          // vkQueuePresentKHR == VK_SUCCESS/SUBOPTIMAL
  std::atomic<uint64_t> present_failures{0};  // any other result
  std::atomic<int32_t> last_present_failure{0};
  std::atomic<uint64_t> submits{0};           // vkQueueSubmit of the command processor and presenter
  std::atomic<uint64_t> submit_failures{0};
  std::atomic<int32_t> last_submit_failure{0};
  std::atomic<uint64_t> draws{0};             // VulkanCommandProcessor::IssueDraw calls
  // Run 21 (B.1): time spent inside the calls, in ns, by the calling thread.
  std::atomic<uint64_t> submit_ns{0};         // vkQueueSubmit of both, as counted above
  std::atomic<uint64_t> present_ns{0};        // vkQueuePresentKHR
  std::atomic<uint64_t> fence_waits{0};       // blocking vkWaitForFences of the command processor
  std::atomic<uint64_t> fence_wait_ns{0};
  // Run 22 (C.2): vkCreateGraphicsPipelines and shader translation (SPIR-V +
  // module), in all threads and in the command processor's own thread.
  std::atomic<uint64_t> pipelines{0}, pipeline_ns{0};
  std::atomic<uint64_t> pipelines_cp{0}, pipeline_ns_cp{0};
  std::atomic<uint64_t> shaders{0}, shader_ns{0};
  std::atomic<uint64_t> shaders_cp{0}, shader_ns_cp{0};
  // Run 26 (B.1): Vulkan memory by kind (vkAllocateMemory/vkFreeMemory, counted
  // in VulkanDevice), live allocations and bytes; see NxVkMemoryKind.
  std::atomic<int64_t> vk_live[4]{}, vk_bytes[4]{};
  std::atomic<uint64_t> vk_allocs{0}, vk_frees{0}, vk_alloc_failures{0};
  // The texture cache (live textures, their host memory as the cache counts
  // it for its limits, and what its limits and the low-memory trim destroyed).
  std::atomic<int64_t> textures_live{0}, texture_bytes{0};
  std::atomic<uint64_t> textures_evicted{0}, texture_bytes_evicted{0};
  // Upload buffer pool pages created (they are freed only when the pool is
  // destroyed, so this is also what is live while the game runs).
  std::atomic<int64_t> upload_pages{0}, upload_bytes{0};
  // Run 26 (B.4): the low-memory cushion. switch-game sets host_memory_low
  // when the host heap headroom is under nx_mem_warn_mib; the texture cache
  // then destroys textures unused for 10 s, and a std::bad_alloc in the GPU
  // Commands thread skips the packet instead of terminating.
  std::atomic<bool> host_memory_low{false};
  std::atomic<uint64_t> trims{0}, trim_textures{0}, trim_bytes{0};
  std::atomic<uint64_t> bad_alloc_packets{0};
  // Run 31 (C.2): wall time of the command processor by NxCpPhase, in ns,
  // without the blocking fence waits, shader translations and pipeline
  // creations of its thread that fall inside a phase (counted above).
  std::atomic<uint64_t> cp_phase_ns[kNxCpPhaseCount]{};
  // N3: GPU time of the completed submissions (timestamps, ns) and the frames
  // they close; the native render target counters (src/graphics/native).
  std::atomic<uint64_t> gpu_elapsed_ns{0}, gpu_frames{0};
  std::atomic<uint64_t> native_frames{0}, native_transfers_not_made{0};
  std::atomic<uint64_t> native_resolves_direct{0}, native_resolves_edram{0};
};

// Kinds of vkAllocateMemory (from VkMemoryDedicatedAllocateInfo and the
// memory type): a dedicated image (textures, render targets), a dedicated
// host-visible buffer (upload pages, readback), a dedicated device-local
// buffer (the 512 MiB shared memory, scratch), anything else (VMA blocks).
enum NxVkMemoryKind : int { kNxVkImage = 0, kNxVkHostBuffer, kNxVkDeviceBuffer, kNxVkOther };

// True on the command processor's worker thread (CommandProcessor::WorkerThreadMain).
inline thread_local bool nx_gpu_on_command_thread = false;

inline NxGpuCounters& nx_gpu_counters() {
  static NxGpuCounters counters;
  return counters;
}

// Monotonic ns for the timings above (steady_clock: the system tick on NX).
inline uint64_t NxGpuNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

inline void NxGpuAddTime(std::atomic<uint64_t>& total, uint64_t start_ns) {
  total.fetch_add(NxGpuNowNs() - start_ns, std::memory_order_relaxed);
}

// Adds one call of duration (now - start_ns) to a total/count pair, and to
// the command-thread pair when the caller is that thread.
inline void NxGpuAddCall(std::atomic<uint64_t>& count, std::atomic<uint64_t>& total,
                         std::atomic<uint64_t>& count_cp, std::atomic<uint64_t>& total_cp,
                         uint64_t start_ns) {
  const uint64_t ns = NxGpuNowNs() - start_ns;
  count.fetch_add(1, std::memory_order_relaxed);
  total.fetch_add(ns, std::memory_order_relaxed);
  if (nx_gpu_on_command_thread) {
    count_cp.fetch_add(1, std::memory_order_relaxed);
    total_cp.fetch_add(ns, std::memory_order_relaxed);
  }
}

// Run 31 (C.2): consecutive laps of one IssueDraw call into cp_phase_ns. Each
// lap leaves out the fence waits, shader translations and pipeline creations
// of this thread since the previous lap; the totals are added once, when the
// timer goes out of scope (early returns included; time after the last lap of
// an early return is not counted).
class NxCpLapTimer {
 public:
  NxCpLapTimer() : last_ns_(NxGpuNowNs()), last_excluded_ns_(Excluded()) {}
  ~NxCpLapTimer() {
    auto& c = nx_gpu_counters();
    for (int i = 0; i < kNxCpPhaseCount; ++i) {
      if (acc_ns_[i]) {
        c.cp_phase_ns[i].fetch_add(acc_ns_[i], std::memory_order_relaxed);
      }
    }
  }
  void Lap(NxCpPhase phase) {
    const uint64_t now_ns = NxGpuNowNs();
    const uint64_t excluded_ns = Excluded();
    const uint64_t lap_ns = now_ns - last_ns_;
    const uint64_t lap_excluded_ns = excluded_ns - last_excluded_ns_;
    acc_ns_[phase] += lap_ns > lap_excluded_ns ? lap_ns - lap_excluded_ns : 0;
    last_ns_ = now_ns;
    last_excluded_ns_ = excluded_ns;
  }

 private:
  static uint64_t Excluded() {
    auto& c = nx_gpu_counters();
    return c.fence_wait_ns.load(std::memory_order_relaxed) +
           c.shader_ns_cp.load(std::memory_order_relaxed) +
           c.pipeline_ns_cp.load(std::memory_order_relaxed);
  }
  uint64_t last_ns_;
  uint64_t last_excluded_ns_;
  uint64_t acc_ns_[kNxCpPhaseCount] = {};
};

// One line per failure, the first 32 only (REXGPU_ERROR at the call site).
inline bool NxGpuCountSubmit(int32_t result) {
  auto& c = nx_gpu_counters();
  if (result == 0) {
    c.submits.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  c.last_submit_failure.store(result, std::memory_order_relaxed);
  return c.submit_failures.fetch_add(1, std::memory_order_relaxed) < 32;
}

// VK_SUBOPTIMAL_KHR (1000001003) is a presented image.
inline bool NxGpuCountPresent(int32_t result) {
  auto& c = nx_gpu_counters();
  if (result == 0 || result == 1000001003) {
    c.presents.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  c.last_present_failure.store(result, std::memory_order_relaxed);
  return c.present_failures.fetch_add(1, std::memory_order_relaxed) < 32;
}

}  // namespace rex::graphics
