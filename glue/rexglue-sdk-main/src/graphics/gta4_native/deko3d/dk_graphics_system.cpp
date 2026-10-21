// gta4_native deko3d back end: the graphics system the title talks to. See dk_graphics_system.h.

#include "dk_graphics_system.h"

#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include <rex/graphics/gta4_native/temporal_commands.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

#include <chrono>
#include <cstring>

#include "dk_compiler.h"
#include "dk_core.h"
#include "dk_recorder.h"

namespace rex::graphics::gta4_native::dk {
namespace {
constexpr uint32_t kCompletedFrameOffset = 16552;  // Gta4NativeGraphicsSystem's
constexpr uint64_t kReportNs = 10'000'000'000ull;
}  // namespace

Deko3dGraphicsSystem::Deko3dGraphicsSystem(std::unique_ptr<system::IGraphicsSystem> guest_gpu,
                                           GraphicsSystemOptions options)
    : guest_gpu_(std::move(guest_gpu)), options_(std::move(options)) {}

Deko3dGraphicsSystem::~Deko3dGraphicsSystem() { Stop(); }

X_STATUS Deko3dGraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  // The recorder presents on the default window itself; the null GPU has nothing to set up for it.
  return guest_gpu_ ? guest_gpu_->SetupPresentation(app_context) : X_STATUS_SUCCESS;
}

X_STATUS Deko3dGraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                             system::KernelState* kernel_state) {
  if (memory_) {
    return X_STATUS_SUCCESS;
  }
  if (guest_gpu_) {
    const X_STATUS status = guest_gpu_->SetupGuestGpu(function_dispatcher, kernel_state);
    if (XFAILED(status)) {
      return status;
    }
  }
  memory_ = function_dispatcher ? function_dispatcher->memory() : nullptr;
  if (!memory_) {
    return X_STATUS_UNSUCCESSFUL;
  }
  if (!package_.Open(options_.package_dir)) {
    recorder_failed_ = true;  // the commands are still taken; nothing is drawn
  }
  front_end_ = std::make_unique<NativeFrontEnd>(memory_, [this](uint64_t hash) {
    const PackageShader* s = package_.Find(hash, false);
    return s ? s->used_texture_mask : UINT32_MAX;
  });
  recorder_thread_ = std::thread([this] { RecorderMain(); });
  REXLOG_INFO("deko3d: graphics system: Liberty Recompiled's title commands (ABI {}) to the deko3d recorder, stage "
              "{} (nx_dk_stage); the null GPU keeps the guest GPU services",
              kTitleCommandAbi, options_.stage ? options_.stage() : 0);
  return X_STATUS_SUCCESS;
}

void Deko3dGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  if (guest_gpu_) {
    guest_gpu_->SetInterruptCallback(callback, user_data);
  }
}

void Deko3dGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  if (guest_gpu_) {
    guest_gpu_->InitializeRingBuffer(ptr, size_log2);
  }
}

void Deko3dGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  if (guest_gpu_) {
    guest_gpu_->EnableReadPointerWriteBack(ptr, block_size_log2);
  }
}

uint32_t Deko3dGraphicsSystem::GetTitleCommandAbi(uint32_t title_id) const {
  // Native mode from the start (the hooks ask at every call; the answer never changes in a session).
  return title_id == kTitleId && front_end_ ? kTitleCommandAbi : 0;
}

bool Deko3dGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                                              size_t command_size) {
  if (title_id != kTitleId || !front_end_) {
    return false;
  }
  const void* inner = command;
  size_t inner_size = command_size;
  uint32_t abi = abi_version;
  if (abi == kGpuPassEnvelopeAbi) {
    GpuPassOrigin origin{};
    if (!UnpackGpuPassEnvelope(inner, inner_size, abi, origin)) {
      return false;
    }
  }
  if (abi != kTitleCommandAbi) {
    return false;  // the diagnostic envelopes (fire, TV, phone traces) are not taken
  }
  CommandHeader header;
  if (inner_size < sizeof(header)) {
    return false;
  }
  std::memcpy(&header, inner, sizeof(header));
  bool accepted;
  {
    std::lock_guard<std::mutex> lock(front_mutex_);
    scratch_.clear();
    accepted = front_end_->Submit(inner, inner_size, scratch_);
    if (!scratch_.empty()) {
      std::unique_lock<std::mutex> queue_lock(queue_mutex_);
      if (stopping_) {
        return false;
      }
      stats_snapshot_ = front_end_->stats();
      for (FrontEndPacket& p : scratch_) {
        if (std::holds_alternative<RecorderPresent>(p)) {
          ++queued_presents_;
        }
        queue_.push_back(std::move(p));
      }
      queue_ready_.notify_one();
      // Backpressure at the Present: at most queued_frames frames wait for the recorder.
      if (header.type == CommandType::kPresent) {
        queue_drained_.wait(queue_lock, [this] { return stopping_ || queued_presents_ <= options_.queued_frames; });
      }
    }
  }
  return accepted;
}

bool Deko3dGraphicsSystem::ExecuteTitleCommand(uint32_t title_id, uint32_t abi_version, const void* command,
                                               size_t command_size, void* result, size_t result_size) {
  if (title_id != kTitleId || abi_version != kTitleCommandAbi || !command || !result ||
      command_size < sizeof(CommandHeader)) {
    return false;
  }
  CommandHeader header;
  std::memcpy(&header, command, sizeof(header));
  switch (header.type) {
    case CommandType::kQueryDeviceCapabilities: {
      if (result_size != sizeof(DeviceCapabilitiesResult)) {
        return false;
      }
      DeviceCapabilitiesResult r{};
      r.max_image_dimension_2d = 16384;  // deko3d's (Maxwell's) 2D limit
      r.capabilities = 0;
      std::memcpy(result, &r, sizeof(r));
      return true;
    }
    case CommandType::kTextureLock: {
      if (result_size != sizeof(TextureLockResult)) {
        return false;
      }
      // Not read back to guest memory yet (the GPU-produced textures the title reads on the CPU: B4). The title
      // locks the guest copy as it is.
      TextureLockResult r{};
      std::memcpy(result, &r, sizeof(r));
      return true;
    }
    default:
      return false;  // the temporal upscaler query: none on deko3d
  }
}

void Deko3dGraphicsSystem::SignalFrameComplete(uint32_t device, uint32_t frame) {
  // Monotonic, big-endian, at device + 16552 (Gta4NativeGraphicsSystem::SignalGuestFrameComplete).
  uint32_t* completed = memory_ ? memory_->TranslateVirtual<uint32_t*>(device + kCompletedFrameOffset) : nullptr;
  if (!completed) {
    return;
  }
  const uint32_t current = __builtin_bswap32(*completed);
  if (int32_t(frame - current) > 0 || current == 0) {
    *completed = __builtin_bswap32(frame);
  }
  frames_signalled_.fetch_add(1);
}

void Deko3dGraphicsSystem::RecorderMain() {
  std::unique_ptr<Device> device;
  std::unique_ptr<Compiler> compiler;
  std::unique_ptr<DkRecorder> recorder;
  if (!recorder_failed_) {
    device = std::make_unique<Device>();
    if (!device->Create(false)) {
      recorder_failed_ = true;
    } else {
      compiler = std::make_unique<Compiler>();
      RecorderOptions ro;
      ro.stage = options_.stage;
      ro.gamma = options_.gamma;
      ro.capture_seconds = options_.capture_seconds;
      ro.capture_dir = options_.capture_dir;
      recorder = std::make_unique<DkRecorder>(*device, memory_, package_, *compiler, ro);
      if (!compiler->started() || !recorder->Initialize()) {
        recorder_failed_ = true;
      }
    }
  }
  if (recorder_failed_) {
    REXLOG_ERROR("deko3d: the recorder did not start (see above): the game runs on without a picture; its frames "
                 "are signalled complete");
    rex::FlushLogging();
  }
  uint64_t next_report = NowNs() + kReportNs;
  for (;;) {
    FrontEndPacket packet;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_ready_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          break;
        }
      } else {
        packet = std::move(queue_.front());
        queue_.pop_front();
      }
    }
    if (const auto* present = std::get_if<RecorderPresent>(&packet)) {
      if (recorder && !recorder_failed_) {
        recorder->Present(*present);
      }
      SignalFrameComplete(present->command.device, present->command.submitted_frame);
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queued_presents_) {
          --queued_presents_;
        }
      }
      queue_drained_.notify_all();
    } else if (recorder && !recorder_failed_) {
      if (const auto* draw = std::get_if<RecorderDraw>(&packet)) {
        recorder->Draw(*draw);
      } else if (const auto* clear = std::get_if<RecorderClear>(&packet)) {
        recorder->Clear(*clear);
      } else if (const auto* resolve = std::get_if<RecorderResolve>(&packet)) {
        recorder->Resolve(*resolve);
      } else if (const auto* event = std::get_if<RecorderResourceEvent>(&packet)) {
        recorder->ResourceEvent(*event);
      }
    }
    if (NowNs() >= next_report) {
      next_report += kReportNs;
      NativeFrontEnd::Stats s;
      {
        // Not front_mutex_: a title thread may hold it while it waits for this thread at a Present.
        std::lock_guard<std::mutex> lock(queue_mutex_);
        s = stats_snapshot_;
      }
      REXLOG_INFO("deko3d: front end: commands {}, draws {}, clears {}, resolves {}, presents {}, rejected {}; "
                  "constant registers sent {}, DrawPrimitiveUp bytes {}; frames signalled complete {}",
                  s.commands, s.draws, s.clears, s.resolves, s.presents, s.rejected, s.constant_registers,
                  s.up_bytes, frames_signalled_.load());
      if (recorder && !recorder_failed_) {
        recorder->Report();
      }
    }
  }
  // Teardown: the recorder (GPU idle, its images, core and swapchain), the uam thread (joined), the device last.
  REXLOG_INFO("deko3d: teardown: recorder");
  rex::FlushLogging();
  recorder.reset();
  REXLOG_INFO("deko3d: teardown: uam thread");
  compiler.reset();
  REXLOG_INFO("deko3d: teardown: device ({} blocks alive)", device ? device->live_blocks() : 0);
  rex::FlushLogging();
  device.reset();
  REXLOG_INFO("deko3d: teardown done");
  rex::FlushLogging();
}

void Deko3dGraphicsSystem::Shutdown() {
  Stop();
  if (guest_gpu_) {
    guest_gpu_->Shutdown();
  }
}

void Deko3dGraphicsSystem::Stop() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stopping_ = true;
  }
  queue_ready_.notify_all();
  queue_drained_.notify_all();
  if (recorder_thread_.joinable()) {
    recorder_thread_.join();
  }
}

}  // namespace rex::graphics::gta4_native::dk
