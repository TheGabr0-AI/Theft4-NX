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
#include <string>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_INT32(pc_native_hooks_frames, 0, "pc-eu-base",
                     "N1: frames of per-frame counts of the game's Direct3D calls written to "
                     "native-hooks.csv; 0 = off");
REXCVAR_DEFINE_INT32(pc_capture_every_s, 0, "pc-eu-base",
                     "M0: the presenter's guest output every N seconds as cap-tNNNN.ppm; 0 = off");
REXCVAR_DEFINE_STRING(pc_capture_dir, ".", "pc-eu-base", "M0: folder of the pc_capture_every_s captures");

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
// Time of each Swap since start (t_ms): frames per second for either renderer.
const auto g_start = std::chrono::steady_clock::now();
uint64_t g_frames = 0, g_last[kHookCount] = {};
bool g_done = false;

// M0 (16-deko3d-devlog.md): the presenter's guest output every pc_capture_every_s seconds as
// PPM, for both GPU plugins, so the emulated and native renderers are compared on the frames
// the game handed over (screen grabs of the window came out black after the first run).
void CaptureEvery() {
  const int32_t every = REXCVAR_GET(pc_capture_every_s);
  if (every <= 0) return;
  static auto next = g_start + std::chrono::seconds(every);
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(every);
  auto* runtime = rex::Runtime::instance();
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  auto* presenter = graphics ? graphics->presenter() : nullptr;
  rex::ui::RawImage image;
  if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) return;
  const int t = int(std::chrono::duration_cast<std::chrono::seconds>(now - g_start).count());
  char name[64];
  std::snprintf(name, sizeof(name), "/cap-t%04d.ppm", t);
  std::FILE* out = std::fopen((REXCVAR_GET(pc_capture_dir) + name).c_str(), "wb");
  if (!out) return;
  std::fprintf(out, "P6\n%u %u\n255\n", image.width, image.height);
  std::vector<uint8_t> row(size_t(image.width) * 3);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3 + 0] = src[x * 4 + 0];
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 2];
    }
    std::fwrite(row.data(), 1, row.size(), out);
  }
  std::fclose(out);
}

void EndFrame() {
  CaptureEvery();
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
               "hook_pixel_shaders,hook_vertex_shaders,hook_gpu_poll_calls,hook_gpu_poll_ms,t_ms\n",
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
               ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%.1f\n",
               g_frames, d[kDrawIndexed], d[kDrawIndexed2], d[kDrawNotIndexed],
               d[kDrawIndexed] + d[kDrawIndexed2] + d[kDrawNotIndexed], d[kResolve],
               d[kRenderTarget], d[kDepthStencil], d[kPixelShader], d[kVertexShader],
               wait_calls - g_last_wait_calls, double(wait_ns - g_last_wait_ns) / 1e6,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count());
  g_last_wait_calls = wait_calls;
  g_last_wait_ns = wait_ns;
  if (++g_frames >= uint64_t(wanted)) {
    std::fclose(g_csv);
    g_csv = nullptr;
    g_done = true;
  }
}

}  // namespace

#define THEFT4_NATIVE_HOOK_BEFORE(addr, index, before)                                  \
  extern "C" void __real___imp__sub_##addr(PPCContext& ctx, uint8_t* base);            \
  extern "C" void __real_sub_##addr(PPCContext& ctx, uint8_t* base);                   \
  extern "C" void __wrap___imp__sub_##addr(PPCContext& ctx, uint8_t* base) {           \
    g_calls[index].fetch_add(1, std::memory_order_relaxed);                            \
    before;                                                                            \
    __real___imp__sub_##addr(ctx, base);                                               \
  }                                                                                    \
  extern "C" void __wrap_sub_##addr(PPCContext& ctx, uint8_t* base) {                  \
    g_calls[index].fetch_add(1, std::memory_order_relaxed);                            \
    before;                                                                            \
    __real_sub_##addr(ctx, base);                                                      \
  }

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

// gta4_native on the European base (15-kabba-port.md): the native renderer
// finds each shader in its cache by XXH3 of the guest container (virtual +
// physical size at +4/+8), hashed before the creation call as
// RegisterNativeShader does. Each new hash is written once to
// shader-hashes.csv, to compare with the keys of a cache built from v8.
REXCVAR_DEFINE_BOOL(pc_shader_hash_log, false, "pc-eu-base",
                    "Write the XXH3 of every shader container created to shader-hashes.csv");
// With a folder, each new container is also written there as <hash>.<vs|ps>.bin: the
// input XenosRecomp needs for the shaders a cache built from v8 lacks
// (16-deko3d-devlog.md, M0). Game data: the folder stays outside the repository.
REXCVAR_DEFINE_STRING(pc_shader_dump_dir, "", "pc-eu-base",
                      "Folder for the shader containers seen by pc_shader_hash_log (empty: none)");

namespace {

void LogShaderHash(PPCContext& ctx, uint8_t* base, const char* stage) {
  if (!REXCVAR_GET(pc_shader_hash_log)) return;
  const uint32_t container = ctx.r3.u32;
  if (!container) return;
  const uint32_t virtual_size = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + container + 4));
  const uint32_t physical_size = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + container + 8));
  if (virtual_size > UINT32_MAX - physical_size || !(virtual_size + physical_size)) return;
  const uint64_t hash = XXH3_64bits(base + container, virtual_size + physical_size);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  static std::FILE* csv = nullptr;
  std::lock_guard<std::mutex> lock(mutex);
  if (!seen.insert(hash).second) return;
  if (!csv) {
    csv = std::fopen("shader-hashes.csv", "w");
    if (!csv) return;
    std::fputs("hash,stage,size\n", csv);
  }
  std::fprintf(csv, "%016" PRIX64 ",%s,%u\n", hash, stage, virtual_size + physical_size);
  std::fflush(csv);
  const std::string dump_dir = REXCVAR_GET(pc_shader_dump_dir);
  if (!dump_dir.empty()) {
    char name[64];
    std::snprintf(name, sizeof(name), "/%016" PRIX64 ".%s.bin", hash, stage);
    if (std::FILE* out = std::fopen((dump_dir + name).c_str(), "wb")) {
      std::fwrite(base + container, 1, virtual_size + physical_size, out);
      std::fclose(out);
    }
  }
}

}  // namespace

THEFT4_NATIVE_HOOK_BEFORE(829D1708, kPixelShader, LogShaderHash(ctx, base, "ps"))
THEFT4_NATIVE_HOOK_BEFORE(829D1818, kVertexShader, LogShaderHash(ctx, base, "vs"))
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
