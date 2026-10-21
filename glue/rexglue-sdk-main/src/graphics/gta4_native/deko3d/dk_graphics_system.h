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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/system/interfaces/graphics.h>

#include "../native_front_end.h"
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

  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;     // the recorder waits for packets
  std::condition_variable queue_drained_;   // the title waits at a Present beyond queued_frames
  std::deque<FrontEndPacket> queue_;
  uint32_t queued_presents_ = 0;
  NativeFrontEnd::Stats stats_snapshot_;  // the front end's counters, copied under queue_mutex_ for the report
  bool stopping_ = false;
  std::thread recorder_thread_;
  std::atomic<bool> recorder_failed_{false};
  std::atomic<uint64_t> frames_signalled_{0};
};

}  // namespace rex::graphics::gta4_native::dk
