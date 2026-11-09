// gta4_native deko3d back end: the graphics system the title talks to. See dk_graphics_system.h.

#include "dk_graphics_system.h"

#include <rex/graphics/gta4_native/gpu_pass_origin.h>
#include <rex/graphics/gta4_native/temporal_commands.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

#include <switch.h>

#include <chrono>
#include <cstring>
#include <exception>

#include "dk_compiler.h"
#include "dk_core.h"
#include "dk_recorder.h"

// The Horizon handle of the recorder thread (run 44): the game's CPU line (tools/switch-game/main.cpp, LogCpu) lists only
// XThreads, so until now nothing said which row, if any, was the recorder; it adds a row for this handle.
extern "C" {
unsigned int g_theft4_dk_recorder_thread = 0;
// Run 45: 1 once this back end is the title's native renderer. The title hooks (gta4_native_hooks.cpp, SubmitNativeCommand) then do
// not send the four commands this front end never reads (SetRenderState, SetTexture, SetRenderTarget, SetDepthStencil: it reads
// the guest device at each draw): about 80 % of the 150-260k calls per 10 s of run 44 were these.
unsigned int g_theft4_dk_skip_state_commands = 0;
}

namespace rex::graphics::gta4_native::dk {
namespace {
constexpr uint32_t kCompletedFrameOffset = 16552;  // Gta4NativeGraphicsSystem's
constexpr uint64_t kReportNs = 10'000'000'000ull;
constexpr size_t kWakeBatch = 16;  // draws queued before the recorder is woken (events wake it at once)

// The calling thread's CPU time in ticks (0: unknown): the libnx thread handle when the thread has one, the current-thread
// pseudo handle otherwise.
uint64_t ThreadCpuTicksNow() {
  static thread_local Handle handle = 0;
  if (!handle) {
    const Thread* self = threadGetSelf();
    handle = self && self->handle ? self->handle : CUR_THREAD_HANDLE;
  }
  uint64_t ticks = 0;
  const u32 type = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount : InfoType_ThreadTickCountDeprecated;
  if (R_FAILED(svcGetInfo(&ticks, type, handle, UINT64_MAX))) {
    return 0;
  }
  return ticks;
}
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
  // Run 36 died here with an exception that reached nothing on the main thread and ended in the unwinder
  // (docs/switch-port/16-deko3d-devlog.md): whatever the package or the set-up throws is logged and means "no
  // picture", never a crash of the title's own thread. Each step is on the card before the next one starts.
  try {
    if (!package_.Open(options_.package_dir)) {
      recorder_failed_ = true;  // the commands are still taken; nothing is drawn
    }
  } catch (const std::exception& e) {
    REXLOG_ERROR("deko3d: the shader package threw ({}): the recorder will not start", e.what());
    recorder_failed_ = true;
  }
  REXLOG_INFO("deko3d: set-up: shader package {}", recorder_failed_ ? "NOT loaded" : "loaded");
  rex::FlushLogging();
  try {
    front_end_ = std::make_unique<NativeFrontEnd>(memory_, [this](uint64_t hash) {
      const PackageShader* s = package_.Find(hash, false);
      return s ? s->used_texture_mask : UINT32_MAX;
    });
    recorder_thread_ = std::thread([this] { RecorderMain(); });
    g_theft4_dk_skip_state_commands = options_.skip_state_commands ? 1 : 0;
  } catch (const std::exception& e) {
    // No front end: GetTitleCommandAbi answers 0 and the title runs on the null GPU alone.
    REXLOG_ERROR("deko3d: the front end or the recorder thread could not be created ({}): the title runs on the "
                 "null GPU without a picture",
                 e.what());
    front_end_.reset();
    rex::FlushLogging();
    return X_STATUS_SUCCESS;
  }
  REXLOG_INFO("deko3d: graphics system: Liberty Recompiled's title commands (ABI {}) to the deko3d recorder, stage "
              "{} (nx_dk_stage); the null GPU keeps the guest GPU services; the four state commands are {} "
              "(nx_dk_skip_state_commands)",
              kTitleCommandAbi, options_.stage ? options_.stage() : 0,
              options_.skip_state_commands ? "not sent by the hooks" : "sent and dropped by the front end");
  rex::FlushLogging();
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
  // One call in 16 is timed, with the thread's CPU ticks (run 45): the clock reads and atomics of every call cost the title's
  // threads about as much as the cheap commands themselves (150-260k calls per 10 s). Times of sampled calls count x16.
  static thread_local uint32_t t_call_count = 0;
  const bool sampled = (++t_call_count & 15u) == 0;
  uint64_t t_call = 0, cpu_call = 0;
  if (sampled) {
    t_call = NowNs();
    cpu_call = ThreadCpuTicksNow();
  }
  bool accepted;
  bool wait_at_present = false;
  std::unique_lock<std::mutex> front_lock(front_mutex_);
  const uint64_t t_locked = sampled ? NowNs() : 0;
  scratch_.clear();
  accepted = front_end_->Submit(inner, inner_size, scratch_);
  const uint64_t t_front = sampled ? NowNs() : 0;
  if (!scratch_.empty()) {
    bool event = false;  // anything that is not a draw: the recorder is woken at once
    for (FrontEndPacket& p : scratch_) {
      if (const auto* resolve = std::get_if<RecorderResolve>(&p); resolve && resolve->command.destination_texture) {
        resolve_destinations_.insert(resolve->command.destination_texture);  // run 55: locks of these wait
      }
      if (std::holds_alternative<RecorderPresent>(p)) {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        if (stopping_) {
          return false;
        }
        ++queued_presents_;  // before the push: the recorder's decrement follows it
      }
      const bool is_event = !std::holds_alternative<RecorderDraw>(p);
      event = event || is_event;
      // The recorder sleeps only when it found the ring empty (it waits at most 2 ms): a lone draw does not wake it, a batch
      // of 16 or any event does (run 45: one kernel wake-up per packet was 40-80 us on a saturated CPU).
      if (!ring_.Push(std::move(p), is_event, kWakeBatch)) {
        return false;  // stopping
      }
    }
    if (event) {
      std::lock_guard<std::mutex> queue_lock(queue_mutex_);
      stats_snapshot_ = front_end_->stats();  // the report's counters: refreshed at the events, not at every draw
      data_snapshot_ = front_end_->data_stats();
    }
    const uint32_t depth = uint32_t(ring_.depth());
    if (depth > times_.queue_depth_max.load(std::memory_order_relaxed)) {
      times_.queue_depth_max.store(depth, std::memory_order_relaxed);
    }
    wait_at_present = header.type == CommandType::kPresent;
  }
  front_lock.unlock();  // the other title threads may submit while this one waits at a Present (run 45)
  const uint64_t t_queued = sampled ? NowNs() : 0;
  if (wait_at_present) {
    // Backpressure at the Present: at most queued_frames frames wait for the recorder.
    const uint64_t t_wait = NowNs();
    std::unique_lock<std::mutex> queue_lock(queue_mutex_);
    queue_drained_.wait(queue_lock, [this] { return stopping_ || queued_presents_ <= options_.queued_frames; });
    queue_lock.unlock();
    times_.present_wait_ns.fetch_add(NowNs() - t_wait, std::memory_order_relaxed);
    times_.present_waits.fetch_add(1, std::memory_order_relaxed);
  }
  if (sampled) {
    const uint64_t t_end = NowNs();
    const uint64_t cpu_end = ThreadCpuTicksNow();
    times_.sampled_calls.fetch_add(1, std::memory_order_relaxed);
    times_.sampled_wall_ns.fetch_add(t_end - t_call, std::memory_order_relaxed);
    if (cpu_call && cpu_end >= cpu_call) {
      times_.sampled_cpu_ns.fetch_add(armTicksToNs(cpu_end - cpu_call), std::memory_order_relaxed);
    }
    times_.calls.fetch_add(16, std::memory_order_relaxed);
    times_.call_ns.fetch_add((t_end - t_call) * 16, std::memory_order_relaxed);
    times_.front_lock_ns.fetch_add((t_locked - t_call) * 16, std::memory_order_relaxed);
    times_.front_end_ns.fetch_add((t_front - t_locked) * 16, std::memory_order_relaxed);
    times_.queue_ns.fetch_add((t_queued - t_front) * 16, std::memory_order_relaxed);
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
      if (result_size != sizeof(TextureLockResult) || command_size != sizeof(TextureLockCommand)) {
        return false;
      }
      TextureLockCommand lock;
      std::memcpy(&lock, command, sizeof(lock));
      TextureLockResult r{};
      // Through run 53 the lock was answered at once: the title read guest memory as it was, never what the GPU wrote
      // (the scene luminance of the exposure among it). Run 54: the lock goes into the ring after the draws already
      // submitted and this thread waits until the recorder has copied the texture back (DkRecorder::TextureLock), as
      // the Vulkan path's synchronous lock does. 5 s at most: then the guest copy as before (logged).
      if (!options_.texture_lock_readback || recorder_failed_ || !lock.texture || lock.flags != 0) {
        std::memcpy(result, &r, sizeof(r));
        return true;
      }
      auto wait = std::make_shared<TextureLockWait>();
      const uint64_t t_start = NowNs();
      {
        std::unique_lock<std::mutex> front_lock(front_mutex_);
        if (!resolve_destinations_.contains(lock.texture)) {
          // Run 55: no resolve wrote it, so the recorder would answer "not GPU-produced, the guest copy" (as the
          // Vulkan path's ReadbackTextureToGuest does): the same answer now, without waiting for the ring to drain.
          lock_stats_.immediate.fetch_add(1, std::memory_order_relaxed);
          std::memcpy(result, &r, sizeof(r));
          return true;
        }
        {
          std::lock_guard<std::mutex> queue_lock(queue_mutex_);
          if (stopping_) {
            std::memcpy(result, &r, sizeof(r));
            return true;
          }
        }
        if (!ring_.Push(FrontEndPacket{RecorderTextureLock{lock, wait}}, true, kWakeBatch)) {
          std::memcpy(result, &r, sizeof(r));
          return true;
        }
      }
      bool done = false;
      {
        std::unique_lock<std::mutex> wait_lock(wait->mutex);
        done = wait->done_cv.wait_for(wait_lock, std::chrono::seconds(5), [&] { return wait->done; });
        if (done) {
          r = wait->result;
        }
      }
      lock_stats_.locks.fetch_add(1, std::memory_order_relaxed);
      lock_stats_.wait_ns.fetch_add(NowNs() - t_start, std::memory_order_relaxed);
      if (!done) {
        if (lock_stats_.timeouts.fetch_add(1, std::memory_order_relaxed) < 8) {
          REXLOG_WARN("deko3d: texture lock {:08X} not completed by the recorder within 5 s: the guest copy as it is",
                      lock.texture);
        }
      } else if (r.copied_to_guest) {
        lock_stats_.copied.fetch_add(1, std::memory_order_relaxed);
      }
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
  if (const Thread* self = threadGetSelf()) {
    g_theft4_dk_recorder_thread = self->handle;
  }
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
      ro.blend_alpha_fix = options_.blend_alpha_fix;
      ro.mixup_from_memory = options_.mixup_from_memory;
  ro.constant_versions = options_.constant_versions;
  ro.geometry_mb = options_.geometry_mb;
  ro.geometry_trust = options_.geometry_trust;
  ro.geometry_signals = options_.geometry_signals;
  ro.skin_diff = options_.skin_diff;
  ro.depth_bias = options_.depth_bias;
  ro.depth_only = options_.depth_only;
  ro.vs_prune = options_.vs_prune;
  ro.anatomy = options_.anatomy;
  ro.placement = options_.placement;
  ro.keep_targets = options_.keep_targets;
  ro.ab_phases = options_.ab_phases;
  ro.ab_phase_mask = options_.ab_phase_mask;
      ro.promote_dynamic_buffer = [this](uint32_t handle) { front_end_->PromoteDynamicBuffer(handle); };
      ro.submit_copies = [this](bool on) { front_end_->SetSubmitCopies(on); };
      ro.volatile_pages = [this](bool on) { front_end_->SetVolatilePages(on); };
      ro.handle_promotions = [this](bool on) { front_end_->SetHandlePromotions(on); };
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
  uint64_t report_clock = NowNs();
  uint64_t seen[10] = {};  // the thread-time totals at the previous report
  uint64_t ss[3] = {};     // the sampled-call totals (calls, wall ns, CPU ns) at the previous report
  for (;;) {
    const uint64_t t_wait = NowNs();
    // Up to 64 packets, read in their slots (run 46); at most 2 ms of sleep: the title wakes this thread per batch or event,
    // not per draw. Once stopped, what is left is still recorded.
    const size_t ready = ring_.Wait(std::chrono::milliseconds(2), 64);
    if (!ready && ring_.stopped()) {
      break;
    }
    const uint64_t t_got = NowNs();
    times_.recorder_idle_ns.fetch_add(t_got - t_wait, std::memory_order_relaxed);
    times_.packets.fetch_add(ready, std::memory_order_relaxed);
    for (size_t i = 0; i < ready; ++i) {
      FrontEndPacket& packet = ring_.Peek(i);
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
        if (auto* draw = std::get_if<RecorderDraw>(&packet)) {
          recorder->Draw(*draw);
          // The slot is reused later: what it shares is let go now, here, as when the deque's packet was destroyed.
          draw->up_vertices.reset();
          draw->data.reset();
          draw->declaration.reset();
        } else if (const auto* clear = std::get_if<RecorderClear>(&packet)) {
          recorder->Clear(*clear);
        } else if (const auto* resolve = std::get_if<RecorderResolve>(&packet)) {
          recorder->Resolve(*resolve);
        } else if (const auto* handoff = std::get_if<RecorderDepthHandoff>(&packet)) {
          recorder->DepthHandoff(*handoff);
        } else if (const auto* event = std::get_if<RecorderResourceEvent>(&packet)) {
          recorder->ResourceEvent(*event);
        } else if (const auto* lock = std::get_if<RecorderTextureLock>(&packet)) {
          recorder->TextureLock(*lock);
        }
      } else if (const auto* lock = std::get_if<RecorderTextureLock>(&packet)) {
        if (lock->wait) {
          lock->wait->Complete(true, TextureLockResult{});  // no recorder: never leave the title waiting
        }
      }
    }
    ring_.Release(ready);
    times_.recorder_busy_ns.fetch_add(NowNs() - t_got, std::memory_order_relaxed);
    if (NowNs() >= next_report) {
      next_report += kReportNs;
      {
        // The thread times of the interval (members of times_): ms, differences from the previous report.
        const uint64_t now = NowNs();
        const uint64_t cur[10] = {times_.calls.load(),         times_.call_ns.load(),       times_.front_lock_ns.load(),
                                  times_.front_end_ns.load(),  times_.queue_ns.load(),      times_.present_wait_ns.load(),
                                  times_.present_waits.load(), times_.recorder_idle_ns.load(),
                                  times_.recorder_busy_ns.load(), times_.packets.load()};
        const uint64_t sc[3] = {times_.sampled_calls.load(), times_.sampled_wall_ns.load(), times_.sampled_cpu_ns.load()};
        const auto ms = [&](int i) { return double(cur[i] - seen[i]) / 1e6; };
        REXLOG_INFO("deko3d: run 54 texture locks (totals): {} locks from the title, {} copied back from the GPU into guest "
                    "memory, {} not completed in 5 s; the title waited {:.1f} ms in all (nx_dk_texture_lock_readback {}); "
                    "run 55: {} answered at once (never a resolve destination, no wait)",
                    lock_stats_.locks.load(), lock_stats_.copied.load(), lock_stats_.timeouts.load(),
                    double(lock_stats_.wait_ns.load()) / 1e6, options_.texture_lock_readback ? "on" : "off",
                    lock_stats_.immediate.load());
        REXLOG_INFO("deko3d: threads in the last {:.0f} ms (title threads add up, they may overlap): in the title's "
                    "SubmitTitleCommand {:.0f} ms over {} calls = front-end lock wait {:.0f} + front end {:.0f} + queue "
                    "{:.0f} + waiting at a Present for the recorder {:.0f} ms ({} Presents); recorder thread waiting for a "
                    "packet {:.0f} ms, recording {:.0f} ms ({} packets); queue depth max {} (ring of {}, full {} times "
                    "since start); sampled calls (1 in 16): {} of {:.2f} ms wall each, {:.2f} ms on a CPU each ({:.0f} % "
                    "of the wall)",
                    double(now - report_clock) / 1e6, ms(1), cur[0] - seen[0], ms(2), ms(3), ms(4), ms(5), cur[6] - seen[6],
                    ms(7), ms(8), cur[9] - seen[9], times_.queue_depth_max.exchange(0), ring_.capacity(),
                    ring_.full_waits(), sc[0] - ss[0],
                    sc[0] > ss[0] ? double(sc[1] - ss[1]) / double(sc[0] - ss[0]) / 1e6 : 0.0,
                    sc[0] > ss[0] ? double(sc[2] - ss[2]) / double(sc[0] - ss[0]) / 1e6 : 0.0,
                    sc[1] > ss[1] ? 100.0 * double(sc[2] - ss[2]) / double(sc[1] - ss[1]) : 0.0);
        for (int i = 0; i < 10; ++i) {
          seen[i] = cur[i];
        }
        for (int i = 0; i < 3; ++i) {
          ss[i] = sc[i];
        }
        report_clock = now;
      }
      NativeFrontEnd::Stats s;
      NativeFrontEnd::DataStats ds;
      {
        // Not front_mutex_: a title thread may hold it while it waits for this thread at a Present.
        std::lock_guard<std::mutex> lock(queue_mutex_);
        s = stats_snapshot_;
        ds = data_snapshot_;
      }
      REXLOG_INFO("deko3d: front end, run 49 (totals): draws copied at submit {} ({} MB; {} because a buffer was locked), "
                  "sampled draws hashed {}; buffers copied at submit {} (of which caught changing by the recorder {})",
                  ds.copied_draws, ds.copied_bytes >> 20, ds.copied_locked, ds.hashed_draws, ds.dynamic_buffers,
                  ds.promoted_buffers);
      REXLOG_INFO("deko3d: front end, run 59 (totals): draws copied at submit by reason: locked {}, rewritten after the "
                  "first fill {}, promoted buffer {}, rewritten pages {}; draws on a buffer with a rewritten page {} (their "
                  "range decided); pages rewritten under a waiting draw {}",
                  ds.copied_locked, ds.copied_rewritten, ds.copied_promoted, ds.copied_pages, ds.page_suspects,
                  GlobalVolatilePages().marked());
      REXLOG_INFO("deko3d: front end, run 55 copy pool (totals): copies made {} (each once, run 55), recycled {}, idle now "
                  "{}, storage kept {} MiB (no allocation, free or lock shared with the recorder per draw); run 56 arena: "
                  "{} copies in it, {} without room (copied into the entry's own storage), peak {} of {} MiB in flight",
                  GlobalDrawDataPool().created(), GlobalDrawDataPool().reused(), GlobalDrawDataPool().free_count(),
                  GlobalDrawDataPool().kept_bytes() >> 20, GlobalDrawDataPool().arena_copies(),
                  GlobalDrawDataPool().arena_full(), GlobalDrawDataPool().arena_peak() >> 20,
                  DrawDataPool::kArenaBytes >> 20);
      REXLOG_INFO("deko3d: front end: commands {}, draws {}, clears {}, resolves {}, presents {}, rejected {}; "
                  "constant registers sent {}, DrawPrimitiveUp bytes {}; frames signalled complete {}; commands "
                  "dropped: malformed depth hand-offs {}, virtual resources {}, other {}; depth hand-offs passed on {}; "
                  "packed depth alias registrations passed on {}",
                  s.commands, s.draws, s.clears, s.resolves, s.presents, s.rejected, s.constant_registers,
                  s.up_bytes, frames_signalled_.load(), s.ignored_depth_handoff, s.ignored_virtual_resource,
                  s.ignored_other, s.depth_handoffs, s.packed_depth_aliases);
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
  ring_.Stop();
  queue_drained_.notify_all();
  if (recorder_thread_.joinable()) {
    recorder_thread_.join();
  }
}

}  // namespace rex::graphics::gta4_native::dk
