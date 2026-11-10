// gta4_native deko3d back end: the graphics system the title talks to (docs/switch-port/16-deko3d-devlog.md, M3 B).
// Switch only.
//
// It answers GetTitleCommandAbi with Liberty Recompiled's ABI, so LibertyRecompAndroid's native hooks send their
// title commands here, as they do to Gta4NativeGraphicsSystem on the PC. The front end (native_front_end.h) runs on
// the title's threads and captures what each command needs; the packets go in order to one recorder thread, which
// creates the deko3d device, opens the shader package, starts the uam thread and records with DkRecorder. After a
// Present is recorded and submitted, the guest device's completed-frame word is written (Gta4NativeGraphicsSystem's
// SignalGuestFrameComplete), which is what the title waits for before it reuses its buffers; at most two frames are
// queued (the title waits at its Present beyond that).
//
// The guest GPU services the title also uses (the Vd* exports: interrupt callback, ring buffer, read pointer
// write-back) stay with the null GPU this system wraps, so whatever PM4 the title still writes is consumed as in
// run 35. If the recorder cannot start (no package, a failed creation), the commands are still taken and every
// frame is signalled complete: the game runs on, without a picture, and the log says why.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/system/interfaces/graphics.h>

#include "../native_front_end.h"
#include "../native_packet_ring.h"
#include "dk_shader_package.h"

namespace rex::memory {
class Memory;
}

namespace rex::graphics::gta4_native::dk {

struct GraphicsSystemOptions {
  std::function<int()> stage;    // nx_dk_stage
  std::function<float()> gamma;  // nx_dk_gamma
  int capture_seconds = 0;       // nx_dk_capture_s
  std::string capture_dir = "sdmc:/switch/theft4/captures";
  std::string package_dir = "sdmc:/switch/theft4/deko3d";
  uint32_t queued_frames = 2;
  bool skip_state_commands = true;  // nx_dk_skip_state_commands (run 45): the hooks do not send the four state commands
  bool blend_alpha_fix = true;      // nx_dk_blend_alpha_fix (run 48)
  bool mixup_from_memory = true;    // nx_dk_mixup_from_memory (run 48)
  bool constant_versions = true;    // nx_dk_constant_versions (run 50)
  uint32_t geometry_mb = 96;        // nx_dk_geometry_mb (run 52)
  bool geometry_trust = true;       // nx_dk_geometry_trust (run 53)
  bool geometry_signals = true;     // nx_dk_geometry_signals (run 64)
  uint32_t geometry_trust_frames = 8;  // nx_dk_geometry_trust_frames (run 65)
  bool texture_lock_readback = true;  // nx_dk_texture_lock_readback (run 54)
  uint32_t skin_diff = 1;           // nx_dk_skin_diff (run 54): GPU skinning read-backs to take (0 = none)
  bool depth_bias = true;           // nx_dk_depth_bias (run 55)
  bool depth_only = true;           // nx_dk_depth_only (run 55)
  bool vs_prune = true;             // nx_dk_vs_prune (run 56)
  bool anatomy = false;             // nx_dk_anatomy (run 56)
  bool character_snapshot = false;  // nx_dk_character_snapshot (run 65)
  bool placement = true;            // nx_dk_placement (run 57)
  bool keep_targets = true;         // nx_dk_keep_targets (run 57)
  bool ab_phases = false;           // nx_dk_ab (run 58, dk_ab.h)
  uint32_t ab_phase_mask = 0;       // nx_dk_ab_phases (run 59, dk_ab.h AbPhaseBit; 0 = run 58's six)
};

class Deko3dGraphicsSystem final : public system::IGraphicsSystem {
 public:
  Deko3dGraphicsSystem(std::unique_ptr<system::IGraphicsSystem> guest_gpu, GraphicsSystemOptions options);
  ~Deko3dGraphicsSystem() override;

  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher, system::KernelState* kernel_state) override;
  bool has_presentation() const override { return false; }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;

  uint32_t GetTitleCommandAbi(uint32_t title_id) const override;
  bool SubmitTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command, size_t command_size) override;
  bool ExecuteTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command, size_t command_size,
                           void* result, size_t result_size) override;

  // The runtime's shutdown: Stop, then the null GPU's.
  void Shutdown() override;

  // Ends the recorder thread (the queue is drained first) and tears everything down in order. Safe to call twice.
  void Stop();
  system::IGraphicsSystem* guest_gpu() const { return guest_gpu_.get(); }

 private:
  // 4096 packets of about 2.5 KB (10 MB, made once). Run 45 queued up to 6,661 in play; when the ring is full the title's
  // thread waits in its push, which is what it did behind the recorder anyway (counted in the report).
  static constexpr size_t kRingSlots = 4096;

  void RecorderMain();
  void SignalFrameComplete(uint32_t device, uint32_t frame);

  std::unique_ptr<system::IGraphicsSystem> guest_gpu_;
  GraphicsSystemOptions options_;
  memory::Memory* memory_ = nullptr;
  // Opened in SetupGuestGpu, before any command: the front end reads its texture masks, the recorder its DKSH.
  ShaderPackage package_;

  std::mutex front_mutex_;  // the front end runs on the title's threads, one at a time
  std::unique_ptr<NativeFrontEnd> front_end_;
  std::vector<FrontEndPacket> scratch_;

  // Run 46: the packets go through a ring of slots made once (native_packet_ring.h), not a deque: no allocation per packet,
  // and the recorder reads the slots in place. queue_mutex_ keeps the Present count, the stop flag and the report's counters.
  PacketRing<FrontEndPacket> ring_{kRingSlots};
  std::mutex queue_mutex_;
  std::condition_variable queue_drained_;   // the title waits at a Present beyond queued_frames
  uint32_t queued_presents_ = 0;
  NativeFrontEnd::DataStats data_snapshot_;  // run 49: the same for the copies at submit
  NativeFrontEnd::Stats stats_snapshot_;  // the front end's counters, copied under queue_mutex_ for the report
  bool stopping_ = false;
  struct LockStats {
    std::atomic<uint64_t> locks{0}, copied{0}, timeouts{0}, wait_ns{0};
    std::atomic<uint64_t> immediate{0};  // run 55: never a resolve destination: answered without the wait
  } lock_stats_;  // run 54: the title's texture locks
  // Run 55: every texture object a resolve submitted so far wrote (front_mutex_): a lock of any other one is answered at
  // once, as the recorder would answer it, instead of waiting for the ring to drain to it (run 54: 53 locks, none of a
  // GPU-produced texture, 1.97 s of the title's time).
  std::unordered_set<uint32_t> resolve_destinations_;
  std::thread recorder_thread_;
  std::atomic<bool> recorder_failed_{false};
  std::atomic<uint64_t> frames_signalled_{0};

  // Where the title's threads and the recorder thread spend their time (run 44, read-only measures; run 45: one call in 16 is
  // timed, so the guest threads pay for the clock reads only 1/16 of the time, and the thread CPU ticks of the sampled calls say
  // how much of a call was on a CPU): running totals in nanoseconds, written with relaxed adds and reported as differences
  // every 10 s. The sampled calls' times are multiplied by 16 (estimates). A title thread's call is split into the wait for the
  // front end's lock, the front end itself (capture of the draw state), the queue lock and push, and the wait at a Present for
  // the recorder to catch up; the recorder thread's loop into the time it waits for a packet and the time it records one.
  // Several title threads may be inside at once, so the sum of their calls can exceed the wall time of the interval.
  struct ThreadTimes {
    std::atomic<uint64_t> calls{0}, call_ns{0}, front_lock_ns{0}, front_end_ns{0}, queue_ns{0}, present_wait_ns{0};
    std::atomic<uint64_t> present_waits{0}, recorder_idle_ns{0}, recorder_busy_ns{0}, packets{0};
    std::atomic<uint64_t> sampled_wall_ns{0}, sampled_cpu_ns{0}, sampled_calls{0};  // raw, not scaled
    std::atomic<uint32_t> queue_depth_max{0};
  } times_;
};

}  // namespace rex::graphics::gta4_native::dk
