// What Renut's renderer (thirdparty/renut/renut_engine, from sa2-nx) asks of the application around it, for GTA IV:
// the headers in compat/ and their code. docs/switch-port/18-renut-migration.md, step R2.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/ui/presenter.h>

#include "renut_engine/gpu/banjo_renut_capture.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "cpu/sa2_enhancements.h"
#include "me/sa2_render_mode.h"
#include "rex/input/read_hook.h"
#include "sa2_capture.h"

#include "renut_engine/gpu/banjo_renut_select.h"

#include "gta4_renut_select.h"

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(gta4_renut_cpu_enhancements, true, "GTA IV enhancements",
                    "Master of the CPU-side keys of Renut's renderer (banjo_renut_* keys of the CPU group)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(gta4_renut_noncpu_enhancements, true, "GTA IV enhancements",
                    "Master of the non-CPU keys of Renut's renderer (banjo_renut_* keys of the non-CPU group)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(gta4_renut_render_height, 720, "GTA IV",
                     "Render height of Renut's renderer in lines, docked and handheld (720 = the game's own)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::atomic<uint64_t> g_sa2_game_frames{0};

// GTA IV's generated code does not mark store cards (sa2-nx's tools/store_cards.py pass is not run here, and this SDK
// keeps no card table): banjo_renut_store_cards stays off, Renut hashes its cached copies.
extern "C" const bool g_banjo_store_cards = false;

#if !REX_PLATFORM_SWITCH
// The Switch profiler's counters (frame_stats.cpp counts the render thread's frames): no-ops on the PC, as sa2-nx's
// src/me/me_pc_stubs.cpp.
extern "C" void RexSwitchPerfCount(unsigned) {}
#else
// On the console too: switch-game's profiler (nx_profiler.cpp) has no counter for the renderer's frames.
extern "C" void RexSwitchPerfCount(unsigned) {}

// me/me_platform_compat.h: the thread helpers sa2-nx's SDK has (threading_switch.cpp), on libnx's system calls.
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority) {
  return priority >= 0 && priority < 64 && R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, u32(priority)));
}

extern "C" bool RexSwitchPinCurrentThreadToCore(int core) {
  u64 process_mask = 0;
  if (core < 0 || core > 3 || R_FAILED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
      !(process_mask & (u64(1) << core)))
    return false;  // a core the process may not use
  return R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, u32(1) << core));
}

extern "C" int RexSwitchCurrentCore(void) { return int(svcGetCurrentProcessorNumber()); }

// device.cpp's sa2_renut_core_layout calls sa2-nx's SDK (system/xthread.cpp) at the first Swap. This SDK has no such
// function and places the guest threads itself (rex::thread::nx_guest_core_mask): SA2's layout, made for SA2's
// threads, is not ported. 0 threads moved, so the renderer's log line says "(0 threads set)".
extern "C" uint32_t RexSetGuestThreadCoreLayout(int, uint32_t, int) { return 0; }

// plume with PLUME_SWITCH_NO_NVK (the only Switch configuration of this renderer: CMake refuses it with NVK) expects
// the parent to define the two ICD entry points as "no driver" (sa2-nx's SDK does under REXGLUE_SWITCH_NO_NVK; this
// one does not). plume's VulkanInterface then finds no vkCreateInstance and stays invalid; deko3d is the back end.
extern "C" void* vk_icdGetInstanceProcAddr(void*, const char*) { return nullptr; }
extern "C" int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t*) { return -9; }  // VK_ERROR_INCOMPATIBLE_DRIVER
#endif

namespace sa2::enh {

namespace {
std::mutex& Mutex() {
  static std::mutex m;
  return m;
}
std::vector<Enhancement*>& Registered() {
  static std::vector<Enhancement*> v;
  return v;
}
}  // namespace

// Called from static initialisers: the key's cvar may not be read yet, so `active` is set by Resolve().
bool Register(Enhancement* e) {
  std::lock_guard lock(Mutex());
  Registered().push_back(e);
  return true;
}

void Resolve() {
  std::lock_guard lock(Mutex());
  for (Enhancement* e : Registered())
    e->active = e->own && e->own() && (e->group == kCpu ? CpuMaster() : NonCpuMaster());
}

bool CpuMaster() { return REXCVAR_GET(gta4_renut_cpu_enhancements); }
bool NonCpuMaster() { return REXCVAR_GET(gta4_renut_noncpu_enhancements); }
void StartReport() {}
void VerifyCheck(uint64_t) {}
void VerifyDifference() {}
bool VerifyMode() { return false; }

}  // namespace sa2::enh

// sa2-nx's relaxed vertical sync key (its native renderer's; sa2_renut_pacing.cpp reads `active`): off here.
::sa2::enh::Enhancement g_vsync_relaxed{"gta4_renut_vsync_relaxed", ::sa2::enh::kNonCpu,
                                        "sa2-nx's relaxed vertical sync (not ported)", [] { return false; }};

// ---------------------------------------------------------------------------------------------------------------
// Frame captures for tests (PC and console): the presented image every gta4_renut_capture_s seconds, as PNG files in
// gta4_renut_capture_dir. The comparison of docs/switch-port/18 (run 65's shots against ReNut's) is made from them.
// Each capture costs a GPU sync (banjo_renut_capture.h): a test tool, off by default.
// ---------------------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_INT32(gta4_renut_capture_s, 0, "GTA IV",
                     "Renut's renderer: save the presented frame every N seconds as a PNG (0 = off; a test tool)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(gta4_renut_capture_dir, "captures", "GTA IV", "Folder of gta4_renut_capture_s's PNG files")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace theft4::renut {
namespace {

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  Put32(png, uint32_t(data.size()));
  const size_t at = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  Put32(png, Crc32(png.data() + at, png.size() - at));
}

// An RGB PNG with stored (uncompressed) deflate blocks: no compressor needed, about 2.7 MB at 1280x720.
bool WritePng(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(image.height) * (1 + size_t(image.width) * 3));
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x)
      raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last)
      break;
  }
  Put32(z, (b << 16) | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, image.width);
  Put32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  return bool(f);
}

std::once_flag g_capture_once;

// The capture thread is joined at shutdown (StopCaptures, before the renderer goes), never detached:
// docs/switch-port/09-rules.md section 3. On the console std::thread::detach() throws std::system_error (ENOSYS: this
// libnx has no __syscall_thread_detach, so pthread_detach always fails), and run 67 ended there, in std::terminate at
// the first Swap. The thread object lives on the heap and is deleted only after its join, so a session that ends
// without StopCaptures never destroys a joinable std::thread (std::terminate too).
std::mutex g_capture_mutex;
std::condition_variable g_capture_wake;
bool g_capture_stop = false;            // under g_capture_mutex
std::thread *g_capture_thread = nullptr;  // under g_capture_mutex

void CaptureMain(int every) {
  const std::filesystem::path dir = std::string(REXCVAR_GET(gta4_renut_capture_dir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto start = std::chrono::steady_clock::now();
  for (int n = 1;; ++n) {
    {
      std::unique_lock lock(g_capture_mutex);
      if (g_capture_wake.wait_until(lock, start + std::chrono::seconds(every * n), [] { return g_capture_stop; }))
        return;
    }
    rex::ui::RawImage image;
    if (!::renut::gpu::CaptureFrame(image, 3000) || !image.width) {
      REXLOG_WARN("gta4_renut_capture_s: no frame at {} s", every * n);
      continue;
    }
    const auto file = dir / fmt::format("renut-{:04}s-f{:06}.png", every * n,
                                        g_sa2_game_frames.load(std::memory_order_relaxed));
    REXLOG_INFO("gta4_renut_capture_s: {} ({}x{}){}", file.string(), image.width, image.height,
                WritePng(file, image) ? "" : " NOT WRITTEN");
  }
}

void StartCaptures() {
  const int every = REXCVAR_GET(gta4_renut_capture_s);
  if (every <= 0)
    return;
  std::lock_guard lock(g_capture_mutex);
  if (!g_capture_stop && !g_capture_thread)
    g_capture_thread = new std::thread(CaptureMain, every);  // a test tool; it only waits and reads served frames
}

}  // namespace

void StopCaptures() {
  std::thread *thread = nullptr;
  {
    std::lock_guard lock(g_capture_mutex);
    g_capture_stop = true;
    thread = std::exchange(g_capture_thread, nullptr);
  }
  g_capture_wake.notify_all();
  if (thread) {
    const auto start = std::chrono::steady_clock::now();
    thread->join();  // at most one capture in flight: CaptureFrame waits 3 s at most
    delete thread;
    REXLOG_INFO("gta4_renut_capture_s: capture thread joined in {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
  }
}
}  // namespace theft4::renut

namespace sa2::render {

bool Docked() {
#if REX_PLATFORM_SWITCH
  return appletGetOperationMode() == AppletOperationMode_Console;
#else
  return false;
#endif
}

int32_t RenderHeightOf(bool) { return REXCVAR_GET(gta4_renut_render_height); }

void OnPresented(uint64_t) { std::call_once(theft4::renut::g_capture_once, theft4::renut::StartCaptures); }

}  // namespace sa2::render

namespace sa2::capture {

bool RenutLockstepOn() { return false; }
void RenutLockstepFrame(uint64_t, const rex::ui::RawImage*) {}

}  // namespace sa2::capture

namespace rex::input {

namespace {
std::atomic<ReadHook> g_read_hook{nullptr};
}
void SetReadHook(ReadHook hook) { g_read_hook.store(hook, std::memory_order_release); }

}  // namespace rex::input

#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
namespace {
// PC test aid (step R1's, kept): the first access violations and other fatal exceptions with the faulting instruction
// as an offset into the executable (the link map, /MAP, names the function), written at once to renut-faults.txt.
// The runtime's own handlers run after this one and are not changed.
LONG CALLBACK LogFault(EXCEPTION_POINTERS* info) {
  static std::atomic<int> count{0};
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  const DWORD code = r->ExceptionCode;
  const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                     code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                     code == 0xC0000409 /* fast fail */ || code == 0xE06D7363 /* C++ throw */;
  if (fatal && count.fetch_add(1) < 16) {
    if (FILE* f = std::fopen("renut-faults.txt", "a")) {
      const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      const auto rip = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
      std::fprintf(f, "exception %08lX: rip exe+%llX (va %llX), %s address %llX, thread %lu\n", code,
                   static_cast<unsigned long long>(rip - module),
                   static_cast<unsigned long long>(rip - module + 0x140000000ull),
                   r->NumberParameters > 0 && r->ExceptionInformation[0] ? "write" : "read",
                   static_cast<unsigned long long>(r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0),
                   GetCurrentThreadId());
      std::fclose(f);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace
#endif

namespace theft4::renut {

bool Selected() {
  static const bool selected = [] {
    const bool on = ::banjo::renut_gpu::Active();
#if defined(_WIN32)
    if (on)
      AddVectoredExceptionHandler(1, LogFault);
#endif
    return on;
  }();
  return selected;
}

}  // namespace theft4::renut
