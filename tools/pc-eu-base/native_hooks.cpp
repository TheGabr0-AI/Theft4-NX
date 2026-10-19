// Native renderer, step N1 (PC): the game's own Direct3D calls at the nine
// European addresses of docs/switch-port/06-game-boot-plan.md section 11 b,
// counted per frame through lld-link -wrap (CMakeLists.txt, option
// THEFT4_NATIVE_HOOKS) and written to native-hooks.csv beside the PM4
// reader's native-observe.csv (tools/native_observe_report.py joins them).
// The frame ends at the Swap (829D5338). As in the Direct3D census, only
// calls from another translation unit go through a wrapper: a call between
// two functions of the same generated file is direct and not counted.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_INT32(pc_native_hooks_frames, 0, "pc-eu-base",
                     "N1: frames of per-frame counts of the game's Direct3D calls written to "
                     "native-hooks.csv; 0 = off");

namespace {

enum Hook : int {
  kPixelShader = 0,   // 829D1708 creation of a pixel shader
  kVertexShader,      // 829D1818 creation of a vertex shader
  kDrawIndexed,       // 829CCEA8
  kDrawIndexed2,      // 829CC610
  kDrawNotIndexed,    // 829CCAC0
  kRenderTarget,      // 829CAAB0 SetRenderTarget
  kDepthStencil,      // 829CAE18 SetDepthStencilSurface
  kResolve,           // 829CB7C8
  kSwap,              // 829D5338
  kHookCount
};

std::atomic<uint64_t> g_calls[kHookCount];
// Run 34 (C): the game's poll of a value the GPU side writes (829CFE80).
std::atomic<uint64_t> g_wait_calls{0}, g_wait_ns{0};
uint64_t g_last_wait_calls = 0, g_last_wait_ns = 0;
std::mutex g_mutex;
std::FILE* g_csv = nullptr;
uint64_t g_frames = 0, g_last[kHookCount] = {};
bool g_done = false;

void EndFrame() {
  const int32_t wanted = REXCVAR_GET(pc_native_hooks_frames);
  if (wanted <= 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_done) return;
  if (!g_csv) {
    g_csv = std::fopen("native-hooks.csv", "w");
    if (!g_csv) {
      g_done = true;
      return;
    }
    std::fputs("frame,hook_draw_indexed,hook_draw_indexed2,hook_draw_not_indexed,hook_draws,"
               "hook_resolves,hook_set_render_target,hook_set_depth_stencil,"
               "hook_pixel_shaders,hook_vertex_shaders,hook_gpu_poll_calls,hook_gpu_poll_ms\n",
               g_csv);
  }
  uint64_t d[kHookCount];
  for (int i = 0; i < kHookCount; ++i) {
    const uint64_t now = g_calls[i].load(std::memory_order_relaxed);
    d[i] = now - g_last[i];
    g_last[i] = now;
  }
  const uint64_t wait_calls = g_wait_calls.load(std::memory_order_relaxed);
  const uint64_t wait_ns = g_wait_ns.load(std::memory_order_relaxed);
  std::fprintf(g_csv,
               "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
               ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f\n",
               g_frames, d[kDrawIndexed], d[kDrawIndexed2], d[kDrawNotIndexed],
               d[kDrawIndexed] + d[kDrawIndexed2] + d[kDrawNotIndexed], d[kResolve],
               d[kRenderTarget], d[kDepthStencil], d[kPixelShader], d[kVertexShader],
               wait_calls - g_last_wait_calls, double(wait_ns - g_last_wait_ns) / 1e6);
  g_last_wait_calls = wait_calls;
  g_last_wait_ns = wait_ns;
  if (++g_frames >= uint64_t(wanted)) {
    std::fclose(g_csv);
    g_csv = nullptr;
    g_done = true;
  }
}

}  // namespace

#define THEFT4_NATIVE_HOOK(addr, index, after)                                          \
  extern "C" void __real___imp__sub_##addr(PPCContext& ctx, uint8_t* base);            \
  extern "C" void __real_sub_##addr(PPCContext& ctx, uint8_t* base);                   \
  extern "C" void __wrap___imp__sub_##addr(PPCContext& ctx, uint8_t* base) {           \
    g_calls[index].fetch_add(1, std::memory_order_relaxed);                            \
    __real___imp__sub_##addr(ctx, base);                                               \
    after;                                                                             \
  }                                                                                    \
  extern "C" void __wrap_sub_##addr(PPCContext& ctx, uint8_t* base) {                  \
    g_calls[index].fetch_add(1, std::memory_order_relaxed);                            \
    __real_sub_##addr(ctx, base);                                                      \
    after;                                                                             \
  }

THEFT4_NATIVE_HOOK(829D1708, kPixelShader, (void)0)
THEFT4_NATIVE_HOOK(829D1818, kVertexShader, (void)0)
THEFT4_NATIVE_HOOK(829CCEA8, kDrawIndexed, (void)0)
THEFT4_NATIVE_HOOK(829CC610, kDrawIndexed2, (void)0)
THEFT4_NATIVE_HOOK(829CCAC0, kDrawNotIndexed, (void)0)
THEFT4_NATIVE_HOOK(829CAAB0, kRenderTarget, (void)0)
THEFT4_NATIVE_HOOK(829CAE18, kDepthStencil, (void)0)
THEFT4_NATIVE_HOOK(829CB7C8, kResolve, (void)0)
THEFT4_NATIVE_HOOK(829D5338, kSwap, EndFrame())

#define THEFT4_TIMED_HOOK(addr)                                                        \
  extern "C" void __real___imp__sub_##addr(PPCContext& ctx, uint8_t* base);           \
  extern "C" void __real_sub_##addr(PPCContext& ctx, uint8_t* base);                  \
  extern "C" void __wrap___imp__sub_##addr(PPCContext& ctx, uint8_t* base) {          \
    const auto start = std::chrono::steady_clock::now();                              \
    __real___imp__sub_##addr(ctx, base);                                              \
    g_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                     std::chrono::steady_clock::now() - start)         \
                                     .count()),                                        \
                        std::memory_order_relaxed);                                    \
    g_wait_calls.fetch_add(1, std::memory_order_relaxed);                              \
  }                                                                                    \
  extern "C" void __wrap_sub_##addr(PPCContext& ctx, uint8_t* base) {                 \
    const auto start = std::chrono::steady_clock::now();                              \
    __real_sub_##addr(ctx, base);                                                     \
    g_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                     std::chrono::steady_clock::now() - start)         \
                                     .count()),                                        \
                        std::memory_order_relaxed);                                    \
    g_wait_calls.fetch_add(1, std::memory_order_relaxed);                              \
  }

THEFT4_TIMED_HOOK(829CFE80)
