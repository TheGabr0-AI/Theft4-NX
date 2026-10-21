// switch-game: nx_gpu = deko3d (nx_deko3d.h, docs/switch-port/16-deko3d-devlog.md M2).
//
// Rules from nfsmw-nx's HowToDeko3d.md and devlog, sa2-nx's guide (16-deko3d-devlog.md, "Rules"):
// the render thread is joined, never detached (libnx has no pthread_detach); uam runs on plume's
// one compile thread (not reentrant), at priority 0x3B on core 2; the viewport height is positive
// (deko3d's clip-space y points up, as Direct3D's); the scissor covers the target before a clear.

#include "nx_deko3d.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <plume_deko3d.h>

#include "deko3d/test_shaders.h"

namespace theft4::nx_deko3d {
namespace {

LogFn g_log = nullptr;
RejectedDrawsFn g_rejected = nullptr;
Options g_options;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_errors{0};

void Logf(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (g_log) g_log(text);
}

// plume's messages (deko3d's debug callback in the debug NRO among them).
void PlumeLog(const char* message) {
  if (std::strstr(message, "error") || std::strstr(message, "Error") || std::strstr(message, "fail")) {
    g_errors.fetch_add(1, std::memory_order_relaxed);
  }
  Logf("deko3d: plume: %s", message);
}

uint64_t NowNs() { return armTicksToNs(armGetSystemTick()); }

// The same key as tools/deko3d/gta4_deko3d_shaders.py: FNV-1a 64 of the exact text uam compiled.
uint64_t Fnv1a64(const std::string& text) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : text) h = (h ^ c) * 0x100000001B3ull;
  return h;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return data;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    data.resize(size_t(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  std::fclose(f);
  return data;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
  return v;
}

std::vector<uint8_t> ShaderBlob(DkStage stage, const char* glsl) {
  plume::Deko3DShaderBlob header;
  header.stage = uint32_t(stage);
  header.glslSize = uint32_t(std::strlen(glsl));
  std::vector<uint8_t> blob(sizeof(header) + header.glslSize);
  std::memcpy(blob.data(), &header, sizeof(header));
  std::memcpy(blob.data() + sizeof(header), glsl, header.glslSize);
  return blob;
}

// The console's uam against the PC's on one text: logged, true when identical.
bool CompareDksh(plume::Deko3DDevice* device, DkStage stage, const char* name, const char* glsl,
                 const uint8_t* pc, size_t pc_size) {
  std::vector<uint8_t> dksh;
  std::string log;
  const uint64_t t0 = NowNs();
  const bool ok = device->compile(stage, glsl, dksh, log);
  const uint64_t ms = (NowNs() - t0) / 1'000'000;
  if (!ok) {
    Logf("deko3d: DKSH %s: the console's uam FAILED in %" PRIu64 " ms: %s", name, ms, log.c_str());
    return false;
  }
  if (dksh.size() == pc_size && std::memcmp(dksh.data(), pc, pc_size) == 0) {
    Logf("deko3d: DKSH %s: console == PC, identical (%zu bytes, uam %" PRIu64 " ms)", name, pc_size, ms);
    return true;
  }
  size_t at = 0;
  while (at < dksh.size() && at < pc_size && dksh[at] == pc[at]) ++at;
  Logf("deko3d: DKSH %s: console != PC (console %zu bytes, PC %zu, first difference at byte %zu, uam %" PRIu64
       " ms)",
       name, dksh.size(), pc_size, at, ms);
  return false;
}

// The game's shaders on the SD card (tools/deko3d/gta4_deko3d_package.py):
//   gta4_deko3d_glsl.bin: "GDKG", u32 version 1, u32 count, then count entries of {u32 stage (DkStage),
//     u32 text offset, u32 text size, u32 values offset, u32 value count}, the values (u32) and the texts;
//   gta4_deko3d_shaders.bin: the PC's DKSH cache (tools/deko3d/gta4_deko3d_shaders.py: "GDK1").
struct Package {
  struct Shader {
    DkStage stage;
    std::string text;
    std::vector<uint32_t> values;
  };
  std::vector<Shader> shaders;
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> pc_dksh;  // key -> offset, size
  std::vector<uint8_t> cache;
  size_t variants = 0;
};

bool LoadPackage(Package& p) {
  const std::string dir = g_options.package_dir;
  const std::vector<uint8_t> glsl = ReadFile(dir + "/gta4_deko3d_glsl.bin");
  p.cache = ReadFile(dir + "/gta4_deko3d_shaders.bin");
  if (glsl.size() < 12 || std::memcmp(glsl.data(), "GDKG", 4) != 0 || U32(glsl, 4) != 1) {
    Logf("deko3d: no game shader package in %s (gta4_deko3d_glsl.bin): no background compiles", dir.c_str());
    return false;
  }
  if (p.cache.size() < 16 || std::memcmp(p.cache.data(), "GDK1", 4) != 0) {
    Logf("deko3d: no PC DKSH cache in %s (gta4_deko3d_shaders.bin): no comparison", dir.c_str());
    p.cache.clear();
  } else {
    const uint32_t count = U32(p.cache, 8), tag = U32(p.cache, 12);
    const size_t table = 16 + tag;
    Logf("deko3d: PC DKSH cache: %u variants, tag '%.*s'", count, int(tag), (const char*)p.cache.data() + 16);
    for (uint32_t i = 0; i < count && table + (i + 1) * 16 <= p.cache.size(); ++i) {
      uint64_t key;
      std::memcpy(&key, p.cache.data() + table + i * 16, 8);
      p.pc_dksh[key] = {U32(p.cache, table + i * 16 + 8), U32(p.cache, table + i * 16 + 12)};
    }
  }
  const uint32_t count = U32(glsl, 8);
  for (uint32_t i = 0; i < count && 12 + (i + 1) * 20 <= glsl.size(); ++i) {
    const size_t e = 12 + i * 20;
    Package::Shader s;
    s.stage = DkStage(U32(glsl, e));
    const uint32_t to = U32(glsl, e + 4), ts = U32(glsl, e + 8), vo = U32(glsl, e + 12), vc = U32(glsl, e + 16);
    if (size_t(to) + ts > glsl.size() || size_t(vo) + size_t(vc) * 4 > glsl.size()) break;
    s.text.assign((const char*)glsl.data() + to, ts);
    for (uint32_t k = 0; k < vc; ++k) s.values.push_back(U32(glsl, vo + k * 4));
    p.variants += s.values.size();
    p.shaders.push_back(std::move(s));
  }
  Logf("deko3d: game shader package: %zu shaders, %zu variants", p.shaders.size(), p.variants);
  return !p.shaders.empty();
}

// tools/deko3d/gta4_deko3d_shaders.py variant_text: the define after the #version line.
std::string VariantText(const std::string& text, uint32_t value) {
  const std::string define = "#define SPIRV_CROSS_CONSTANT_ID_0 " + std::to_string(value) + "u";
  if (text.rfind("#version", 0) == 0) {
    const size_t nl = text.find('\n');
    if (nl != std::string::npos) return text.substr(0, nl + 1) + define + "\n" + text.substr(nl + 1);
    return text + "\n" + define;
  }
  return define + "\n" + text;
}

struct CompileStats {
  uint64_t compiled = 0, identical = 0, different = 0, missing_pc = 0, failed = 0;
  std::vector<uint32_t> ms;
};

void LogCompileStats(const char* when, const CompileStats& s, size_t total) {
  std::vector<uint32_t> v = s.ms;
  std::sort(v.begin(), v.end());
  auto pct = [&](double p) { return v.empty() ? 0u : v[std::min(v.size() - 1, size_t(p * v.size()))]; };
  uint64_t sum = 0;
  for (uint32_t m : v) sum += m;
  Logf("deko3d: background uam %s: %" PRIu64 " of %zu variants compiled, %" PRIu64 " identical to the PC, %" PRIu64
       " different, %" PRIu64 " not in the PC cache, %" PRIu64 " failed; uam ms mean %" PRIu64
       " p50 %u p90 %u p99 %u max %u",
       when, s.compiled, total, s.identical, s.different, s.missing_pc, s.failed,
       v.empty() ? 0 : sum / v.size(), pct(0.5), pct(0.9), pct(0.99), v.empty() ? 0u : v.back());
}

void Report(uint64_t frames, const CompileStats& cs, size_t total) {
  const plume::Deko3DStats& st = plume::GetDeko3DStats();
  Logf("deko3d: %" PRIu64 " frames presented; errors %" PRIu64 "; command memory %" PRIu64 " KiB, images %" PRIu64
       " KiB, buffers %" PRIu64 " KiB, shader code %" PRIu64 " KiB; uam compiles %" PRIu64 " (%" PRIu64
       " ms in all, %" PRIu64 " failed), cache hits %" PRIu64 "; draws %" PRIu64 "; barriers full %" PRIu64
       " fragments %" PRIu64 "; rejected draws (null GPU, the game's) %" PRIu64,
       frames, g_errors.load(), uint64_t(st.commandBytes) >> 10, uint64_t(st.imageBytes) >> 10,
       uint64_t(st.bufferBytes) >> 10, uint64_t(st.codeBytes) >> 10, uint64_t(st.shaderCompiles),
       uint64_t(st.shaderCompileNs) / 1'000'000, uint64_t(st.shaderCompileFailures),
       uint64_t(st.shaderCacheHits), uint64_t(st.draws), uint64_t(st.barriersFull),
       uint64_t(st.barriersFragments), g_rejected ? g_rejected() : 0);
  if (total) LogCompileStats("so far", cs, total);
}

void RenderThread() {
  plume::SetDeko3DLogCallback(PlumeLog);
  plume::Deko3DOptions opts;
  opts.uamFlags = 1471;      // uam's default, as the PC tool compiled the DKSH it is compared with
  opts.compileCore = 2;      // off the game's main core
  opts.compilePriority = 0x3B;  // 0x3C is refused for an application (nfsmw-nx)
  plume::Deko3DSetOptions(opts);
  Logf("deko3d: starting (plume's deko3d back end, uam flags %u, compile thread core %d priority 0x%X)",
       opts.uamFlags, opts.compileCore, opts.compilePriority);
  std::unique_ptr<plume::RenderInterface> iface = plume::CreateDeko3DInterface();
  std::unique_ptr<plume::RenderDevice> device = iface ? iface->createDevice("") : nullptr;
  if (!device) {
    Logf("deko3d: the device was not created");
    return;
  }
  auto* dk = static_cast<plume::Deko3DDevice*>(device.get());
  std::unique_ptr<plume::RenderCommandQueue> queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  std::unique_ptr<plume::RenderSwapChain> swapchain = queue->createSwapChain(
      plume::RenderSwapChainDesc(nwindowGetDefault(), plume::RenderFormat::R8G8B8A8_UNORM, 3));
  if (!swapchain || swapchain->isEmpty()) {
    Logf("deko3d: the swapchain was not created");
    return;
  }
  const uint32_t width = swapchain->getWidth(), height = swapchain->getHeight();
  std::unique_ptr<plume::RenderTexture> depth =
      device->createTexture(plume::RenderTextureDesc::DepthTarget(width, height, plume::RenderFormat::D32_FLOAT));
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  for (uint32_t i = 0; i < swapchain->getTextureCount(); ++i) {
    const plume::RenderTexture* color = swapchain->getTexture(i);
    framebuffers.push_back(device->createFramebuffer(plume::RenderFramebufferDesc(&color, 1, depth.get())));
  }
  Logf("deko3d: device, queue, swapchain %ux%u (%u images), depth target", width, height,
       swapchain->getTextureCount());

  // The test pattern's pipeline.
  const std::vector<uint8_t> vs_blob = ShaderBlob(DkStage_Vertex, kTestPattern_vert_glsl);
  const std::vector<uint8_t> ps_blob = ShaderBlob(DkStage_Fragment, kTestPattern_frag_glsl);
  std::unique_ptr<plume::RenderShader> vs =
      device->createShader(vs_blob.data(), vs_blob.size(), "main", plume::RenderShaderFormat::DEKO3D);
  std::unique_ptr<plume::RenderShader> ps =
      device->createShader(ps_blob.data(), ps_blob.size(), "main", plume::RenderShaderFormat::DEKO3D);
  std::unique_ptr<plume::RenderPipelineLayout> layout = device->createPipelineLayout(plume::RenderPipelineLayoutDesc());
  plume::RenderGraphicsPipelineDesc pd;
  pd.pipelineLayout = layout.get();
  pd.vertexShader = vs.get();
  pd.pixelShader = ps.get();
  pd.depthEnabled = true;
  pd.depthWriteEnabled = true;
  pd.depthFunction = plume::RenderComparisonFunction::LESS;
  pd.cullMode = plume::RenderCullMode::NONE;
  pd.renderTargetFormat[0] = plume::RenderFormat::R8G8B8A8_UNORM;
  pd.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  pd.renderTargetCount = 1;
  pd.depthTargetFormat = plume::RenderFormat::D32_FLOAT;
  std::unique_ptr<plume::RenderPipeline> pipeline = vs && ps ? device->createGraphicsPipeline(pd) : nullptr;
  if (!pipeline) Logf("deko3d: the test pattern's pipeline was not created");

  // The DKSH check of the test pattern's shaders (console uam against the PC's bytes).
  const bool vert_same = CompareDksh(dk, DkStage_Vertex, "test pattern vertex shader", kTestPattern_vert_glsl,
                                     kTestPattern_vert_dksh, sizeof(kTestPattern_vert_dksh));
  const bool frag_same = CompareDksh(dk, DkStage_Fragment, "test pattern pixel shader", kTestPattern_frag_glsl,
                                     kTestPattern_frag_dksh, sizeof(kTestPattern_frag_dksh));
  Logf("deko3d: DKSH console == PC for the test pattern: %s", vert_same && frag_same ? "yes" : "NO");

  std::unique_ptr<plume::RenderCommandList> list = queue->createCommandList();
  std::unique_ptr<plume::RenderCommandFence> fence = device->createCommandFence();

  Package package;
  const bool have_package = g_options.compile_seconds > 0 && LoadPackage(package);
  CompileStats cs;
  size_t shader_index = 0, value_index = 0;

  const uint64_t start = NowNs();
  uint64_t frames = 0, next_report = start + 10'000'000'000ull;
  bool pattern_logged = false, compile_done = !have_package;
  while (!g_stop.load(std::memory_order_relaxed)) {
    const uint64_t now = NowNs();
    const bool pattern = now - start < uint64_t(g_options.test_pattern_seconds) * 1'000'000'000ull;
    if (!pattern && !pattern_logged) {
      pattern_logged = true;
      Logf("deko3d: test pattern done after %" PRIu64 " frames; the game's frames are not drawn in this build "
           "(Liberty Recompiled's renderer is not on deko3d yet): a dark blue screen from now on",
           frames);
    }
    if (plume::Deko3DDeviceLost(device.get())) {
      Logf("deko3d: the queue is in an error state (GPU fault): stopping the render thread");
      break;
    }
    uint32_t index = 0;
    if (!swapchain->acquireTexture(nullptr, &index)) {
      Logf("deko3d: acquireTexture failed");
      break;
    }
    plume::RenderTexture* target = swapchain->getTexture(index);
    list->begin();
    plume::RenderTextureBarrier to_target[2] = {
        plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE),
        plume::RenderTextureBarrier(depth.get(), plume::RenderTextureLayout::DEPTH_WRITE)};
    list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, to_target, 2);
    list->setFramebuffer(framebuffers[index].get());
    const plume::RenderRect full(0, 0, int32_t(width), int32_t(height));
    list->setScissors(&full, 1);  // clears respect the scissor
    list->clearColor(0, pattern ? plume::RenderColor(0.15f, 0.15f, 0.15f, 1.0f)
                                : plume::RenderColor(0.02f, 0.04f, 0.12f, 1.0f));
    list->clearDepthStencil(true, false, 1.0f, 0);
    if (pattern && pipeline) {
      const plume::RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
      list->setViewports(&viewport, 1);
      list->setScissors(&full, 1);
      list->setGraphicsPipelineLayout(layout.get());
      list->setPipeline(pipeline.get());
      list->drawInstanced(36, 1, 0, 0);
    }
    plume::RenderTextureBarrier to_present(target, plume::RenderTextureLayout::PRESENT);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0, &to_present, 1);
    list->end();
    const plume::RenderCommandList* lists[] = {list.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    swapchain->present(index, nullptr, 0);
    queue->waitForCommandFence(fence.get());
    ++frames;

    // Between frames: background compiles of the game's variants for the budget (each one a whole uam
    // compile on plume's compile thread; the pattern frames come first).
    if (!pattern && !compile_done) {
      const uint64_t slice_end = NowNs() + 30'000'000ull;
      while (NowNs() < slice_end && shader_index < package.shaders.size()) {
        const Package::Shader& s = package.shaders[shader_index];
        const std::string text = VariantText(s.text, s.values[value_index]);
        std::vector<uint8_t> dksh;
        std::string log;
        const uint64_t t0 = NowNs();
        const bool ok = dk->compile(s.stage, text, dksh, log);
        cs.ms.push_back(uint32_t((NowNs() - t0) / 1'000'000));
        if (!ok) {
          if (++cs.failed <= 8) Logf("deko3d: background uam failed: %s", log.substr(0, 300).c_str());
        } else {
          ++cs.compiled;
          auto it = package.pc_dksh.find(Fnv1a64(text));
          if (it == package.pc_dksh.end()) {
            ++cs.missing_pc;
          } else if (it->second.second == dksh.size() && size_t(it->second.first) + dksh.size() <= package.cache.size() &&
                     std::memcmp(package.cache.data() + it->second.first, dksh.data(), dksh.size()) == 0) {
            ++cs.identical;
          } else if (++cs.different <= 8) {
            Logf("deko3d: background DKSH differs from the PC's (shader %zu, value 0x%X: console %zu bytes, PC %u)",
                 shader_index, s.values[value_index], dksh.size(), it->second.second);
          }
        }
        if (++value_index >= s.values.size()) {
          value_index = 0;
          ++shader_index;
        }
      }
      if (shader_index >= package.shaders.size() ||
          NowNs() - start > uint64_t(g_options.compile_seconds + g_options.test_pattern_seconds) * 1'000'000'000ull) {
        compile_done = true;
        LogCompileStats("done", cs, package.variants);
      }
    } else if (!pattern) {
      svcSleepThread(100'000'000ll);  // a dark frame ten times a second is enough
    }
    if (NowNs() >= next_report) {
      next_report += 10'000'000'000ull;
      Report(frames, cs, have_package ? package.variants : 0);
    }
  }
  Report(frames, cs, have_package ? package.variants : 0);
  queue->waitForCommandFence(fence.get());
}

}  // namespace

bool Start(LogFn log, RejectedDrawsFn rejected, const Options& options) {
  g_log = log;
  g_rejected = rejected;
  g_options = options;
  g_stop.store(false);
  g_thread = std::thread(RenderThread);
  return true;
}

void Stop() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace theft4::nx_deko3d
