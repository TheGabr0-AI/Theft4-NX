// gta4_native deko3d back end: the uam compile worker. See dk_compiler.h (ported from nfsmw-nx's
// nfsmw_dk_compilador.cpp, GPL-3.0).

#include "dk_compiler.h"

#include <switch.h>

#include <rex/logging.h>
#include <rex/logging/api.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>

#include <uam_api.h>

#include "dk_core.h"

namespace rex::graphics::gta4_native::dk {
namespace {
constexpr size_t kStack = 8u << 20;
constexpr int kPriority = 0x3B;
constexpr int kCore = 2;
constexpr unsigned kBanjoFlags = 1471;  // tools/deko3d/gta4_deko3d_shaders.py's uam

// What the game's shaders use (a UBO, a texture, a dynamic loop): uam is known to work here at start.
constexpr char kSelfTest[] = R"(#version 460
layout(std140, binding = 0) uniform Block { vec4 v[4]; } b;
layout(binding = 0) uniform sampler2D t;
layout(location = 0) in vec4 input0;
layout(location = 0) out vec4 output0;
void main() {
  vec4 r = texture(t, input0.xy);
  for (int i = 0; i < int(b.v[0].x); ++i) r += b.v[1 + (i & 1)];
  output0 = r * b.v[3];
}
)";
}  // namespace

struct Compiler::Data {
  struct Job {
    bool vertex;
    std::string glsl;
    Done done;
  };
  Thread thread{};
  bool started = false;
  mutable std::mutex mutex;
  std::condition_variable work;
  std::deque<Job> queue;
  bool stop = false;
  uint32_t pending = 0;

  static void Entry(void* self) { static_cast<Data*>(self)->Run(); }

  void Run() {
    const uint64_t t0 = NowNs();
    uam::setBanjoFlags(kBanjoFlags);
    uam::init(true);
    const uam::Result test = uam::compile(uam::Stage::Fragment, kSelfTest);
    if (test.ok) {
      REXLOG_INFO("deko3d: uam ready on core {} priority 0x{:X}: self-test compiled in {:.1f} ms ({} bytes of DKSH)",
                  kCore, kPriority, double(NowNs() - t0) / 1e6, test.dksh.size());
    } else {
      REXLOG_ERROR("deko3d: uam SELF-TEST FAILED: {}", test.log);
    }
    rex::FlushLogging();
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        work.wait(lock, [this] { return stop || !queue.empty(); });
        if (stop) {
          break;
        }
        job = std::move(queue.front());
        queue.pop_front();
      }
      const uint64_t start = NowNs();
      uam::Result r = uam::compile(job.vertex ? uam::Stage::Vertex : uam::Stage::Fragment, job.glsl.c_str());
      job.done(Result{r.ok, std::move(r.dksh), std::move(r.log), NowNs() - start});
      std::lock_guard<std::mutex> lock(mutex);
      --pending;
    }
    uam::shutdown();
  }
};

Compiler::Compiler() : data_(std::make_unique<Data>()) {
  if (R_FAILED(threadCreate(&data_->thread, Data::Entry, data_.get(), nullptr, kStack, kPriority, kCore)) ||
      R_FAILED(threadStart(&data_->thread))) {
    REXLOG_ERROR("deko3d: the uam thread was not created (8 MiB stack, priority 0x{:X}, core {})", kPriority, kCore);
    return;
  }
  data_->started = true;
}

Compiler::~Compiler() {
  if (!data_->started) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(data_->mutex);
    data_->stop = true;
  }
  data_->work.notify_all();
  // A compile in progress ends only when uam is done with it: run 39 (and 37) waited here for over a minute at the
  // exit, with the picture already gone (the recorder is destroyed first), and the game never left. The wait is
  // bounded; a compile that is still running then keeps its Data (the thread uses it until it returns) and the exit
  // goes on (exit(0) as mode A does with the game's threads: devlog section I).
  constexpr uint64_t kJoinNs = 3'000'000'000ull;
  if (R_FAILED(svcWaitSynchronizationSingle(data_->thread.handle, kJoinNs))) {
    REXLOG_WARN("deko3d: the uam thread is still compiling after 3 s ({} jobs): not waited for, the exit goes on",
                Pending());
    rex::FlushLogging();
    data_.release();  // deliberately kept: the running thread owns it until the process ends
    return;
  }
  threadWaitForExit(&data_->thread);
  threadClose(&data_->thread);
}

bool Compiler::started() const { return data_->started; }

void Compiler::Queue(bool vertex, std::string glsl, Done done) {
  if (!data_->started) {
    done(Result{false, {}, "the uam thread did not start", 0});
    return;
  }
  {
    std::lock_guard<std::mutex> lock(data_->mutex);
    data_->queue.push_back({vertex, std::move(glsl), std::move(done)});
    ++data_->pending;
  }
  data_->work.notify_one();
}

Compiler::Result Compiler::Compile(bool vertex, std::string glsl) {
  std::promise<Result> promise;
  std::future<Result> future = promise.get_future();
  Queue(vertex, std::move(glsl), [&promise](Result r) { promise.set_value(std::move(r)); });
  return future.get();
}

uint32_t Compiler::Pending() const {
  std::lock_guard<std::mutex> lock(data_->mutex);
  return data_->pending;
}

}  // namespace rex::graphics::gta4_native::dk
