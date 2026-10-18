// switch-smoke step 8: the same GPU workload through Mesa's NVK (Vulkan,
// libvulkan.a of mesa-switch) and through deko3d (devkitPro's libdeko3d), in
// the same process, at the same clocks, offscreen (docs/switch-port/
// 13-deko3d-spike.md, D.2). One API at a time: its device is destroyed before
// the other one starts. Two rounds (NVK, deko3d, NVK, deko3d) show how much a
// number moves between two measurements of the same thing.
//
// Parts, the same for both APIs (shaders from the same GLSL in
// gpu_bench_shaders/, glslang for NVK and uam for deko3d):
//   1. draws: 20 frames of 5000 small textured quads into a 1280x720 RGBA8
//      target; per draw a new 256-byte uniform slice and a scissor, every 4
//      draws another of 16 textures, every 8 draws another of 4 pipelines
//      (2 pixel shaders x 2 blend states). CPU time to record and to submit,
//      GPU time (timestamps at the start and end of the frame), wall time.
//   2. submit and fence: 200 empty submissions, each awaited.
//   3. texture upload: a 2048x2048 RGBA8 image (16 MiB) from a host buffer,
//      5 times; the memcpy into the host buffer apart.
//   4. fill rate: 20 full-screen passes of a heavy pixel shader (32 rounds of
//      sin/cos) at 1280x720; GPU time per pass.
//   5. render target changes: 400 changes among 4 targets of 1280x720, one
//      small quad each; CPU per change and GPU total.
// The medians skip the first frame or repetition (warm-up). GPU timestamps
// are converted with 625/384 ns per tick on both sides (dkTimestampToNs; NVK
// reports timestampPeriod 1 but its counter ticks every 1.627 ns, 06 §12).

#include "gpu_bench.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <switch.h>

#include <deko3d.h>
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

#include "gpu_bench_shaders.h"
#include "nx_clocks.h"
#include "probes.h"

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
constexpr uint32_t kDraws = 5000, kFrames = 20;
constexpr uint32_t kTextures = 16, kTextureSize = 64, kTextureEvery = 4;
constexpr uint32_t kPipelines = 4, kPipelineEvery = 8;
constexpr uint32_t kUboStride = 256;
constexpr uint32_t kFenceRounds = 200;
constexpr uint32_t kUploadSize = 2048, kUploads = 5;
constexpr uint64_t kUploadBytes = uint64_t(kUploadSize) * kUploadSize * 4;
constexpr uint32_t kFillPasses = 20;
constexpr uint32_t kTargets = 4, kTargetChanges = 400;

uint64_t NowNs() { return armTicksToNs(armGetSystemTick()); }
uint64_t TicksToNs(uint64_t ticks) { return ticks * 625 / 384; }

double Median(std::vector<double> v, size_t skip = 1) {
  if (v.size() > skip) v.erase(v.begin(), v.begin() + skip);
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double Percentile(std::vector<double> v, double p, size_t skip = 1) {
  if (v.size() > skip) v.erase(v.begin(), v.begin() + skip);
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

// Per-draw uniform slices: a quad on a 100 x 50 grid, a colour; the same for both APIs.
void FillDrawData(uint8_t* base) {
  for (uint32_t i = 0; i < kDraws; ++i) {
    float* f = reinterpret_cast<float*>(base + size_t(i) * kUboStride);
    f[0] = -1.0f + float(i % 100) * 0.02f;
    f[1] = -1.0f + float((i / 100) % 50) * 0.04f;
    f[2] = 0.015f;
    f[3] = 0.03f;
    f[4] = float(i % 7) / 7.0f;
    f[5] = float(i % 11) / 11.0f;
    f[6] = float(i % 13) / 13.0f;
    f[7] = 0.75f;
  }
}

void FillTexels(uint32_t* texels, uint64_t count, uint32_t seed) {
  uint32_t x = seed * 2654435761u + 1;
  for (uint64_t i = 0; i < count; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    texels[i] = x | 0xFF000000u;
  }
}

DkScissor ScissorOf(uint32_t i) {
  // A slightly different scissor per draw, always containing the quad.
  return DkScissor{i % 8, i % 5, kWidth - (i % 8), kHeight - (i % 5)};
}

struct BenchResult {
  bool ok = false;
  std::string failure;
  double record_us_per_draw = 0, submit_ms = 0, frame_gpu_ms = 0, frame_wall_ms = 0;
  double fence_us_median = 0, fence_us_p95 = 0;
  double upload_memcpy_ms = 0, upload_wall_ms = 0, upload_gpu_ms = 0;
  double fill_gpu_ms_per_pass = 0;
  double rt_record_us_per_change = 0, rt_gpu_ms = 0;
};

void LogClocks(const char* when) {
  SMOKE_INFO("gpu bench: clocks %s: %s", when,
             theft4::nx_clocks::Describe(theft4::nx_clocks::Read()).c_str());
}

void LogResult(const char* api, int round, const BenchResult& r) {
  if (!r.ok) {
    SMOKE_INFO("gpu bench: %s round %d FAILED at %s", api, round, r.failure.c_str());
    return;
  }
  SMOKE_INFO("gpu bench: %s round %d draws: %.2f us CPU per draw to record, submit %.3f ms, "
             "GPU %.2f ms per frame of %u draws (%.2f us per draw), wall %.2f ms",
             api, round, r.record_us_per_draw, r.submit_ms, r.frame_gpu_ms, kDraws,
             r.frame_gpu_ms * 1000.0 / kDraws, r.frame_wall_ms);
  SMOKE_INFO("gpu bench: %s round %d submit+fence: median %.1f us, p95 %.1f us (%u rounds)", api,
             round, r.fence_us_median, r.fence_us_p95, kFenceRounds);
  SMOKE_INFO("gpu bench: %s round %d upload 16 MiB: memcpy %.2f ms, copy wall %.2f ms, GPU %.2f "
             "ms = %.0f MB/s",
             api, round, r.upload_memcpy_ms, r.upload_wall_ms, r.upload_gpu_ms,
             r.upload_gpu_ms > 0 ? double(kUploadBytes) / 1e6 / (r.upload_gpu_ms / 1000.0) : 0.0);
  SMOKE_INFO("gpu bench: %s round %d fill 1280x720 heavy shader: %.3f ms GPU per pass (%.0f "
             "Mpixel/s)",
             api, round, r.fill_gpu_ms_per_pass,
             r.fill_gpu_ms_per_pass > 0
                 ? double(kWidth) * kHeight / 1e6 / (r.fill_gpu_ms_per_pass / 1000.0)
                 : 0.0);
  SMOKE_INFO("gpu bench: %s round %d render targets: %.2f us CPU per change, GPU %.2f ms for %u "
             "changes",
             api, round, r.rt_record_us_per_change, r.rt_gpu_ms, kTargetChanges);
}

// ── NVK ─────────────────────────────────────────────────────────────────────

class VulkanBench {
 public:
  BenchResult Run() {
    if (Setup()) {
      r_.ok = Draws() && Fences() && Upload() && Fill() && Targets();
    }
    Teardown();
    return r_;
  }

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool Check(const char* what, VkResult result) {
    if (result == VK_SUCCESS) return true;
    r_.failure = std::string(what) + " -> VkResult " + std::to_string(int(result));
    return false;
  }

  uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
  }

  bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (!Check("vkCreateBuffer", vkCreateBuffer(device_, &bi, nullptr, &out->buffer))) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, out->buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return Check("host-coherent memory type", VK_ERROR_UNKNOWN);
    if (!Check("vkAllocateMemory", vkAllocateMemory(device_, &ai, nullptr, &out->memory)) ||
        !Check("vkBindBufferMemory", vkBindBufferMemory(device_, out->buffer, out->memory, 0)) ||
        !Check("vkMapMemory", vkMapMemory(device_, out->memory, 0, VK_WHOLE_SIZE, 0, &out->mapped))) {
      return false;
    }
    buffers_.push_back(out);
    return true;
  }

  bool MakeImage(uint32_t w, uint32_t h, VkImageUsageFlags usage, Image* out) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check("vkCreateImage", vkCreateImage(device_, &ii, nullptr, &out->image))) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, out->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!Check("vkAllocateMemory (image)", vkAllocateMemory(device_, &ai, nullptr, &out->memory)) ||
        !Check("vkBindImageMemory", vkBindImageMemory(device_, out->image, out->memory, 0))) {
      return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!Check("vkCreateImageView", vkCreateImageView(device_, &vi, nullptr, &out->view))) {
      return false;
    }
    images_.push_back(out);
    return true;
  }

  static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                      VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  }

  bool Begin() {
    return Check("vkResetCommandBuffer", vkResetCommandBuffer(cmd_, 0)) && [&] {
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      return Check("vkBeginCommandBuffer", vkBeginCommandBuffer(cmd_, &bi));
    }();
  }

  // Ends, submits with the fence and waits; submit_ns gets the vkQueueSubmit time.
  bool SubmitAndWait(uint64_t* submit_ns = nullptr) {
    if (!Check("vkEndCommandBuffer", vkEndCommandBuffer(cmd_))) return false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    const uint64_t t0 = NowNs();
    if (!Check("vkQueueSubmit", vkQueueSubmit(queue_, 1, &si, fence_))) return false;
    if (submit_ns) *submit_ns = NowNs() - t0;
    return Check("vkWaitForFences", vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX)) &&
           Check("vkResetFences", vkResetFences(device_, 1, &fence_));
  }

  double GpuMs(uint32_t first) {
    uint64_t ts[2] = {};
    if (vkGetQueryPoolResults(device_, queries_, first, 2, sizeof(ts), ts, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
      return -1.0;
    }
    return double(TicksToNs(ts[1] - ts[0])) / 1e6;
  }

  VkShaderModule Module(const uint32_t* words, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    if (!Check("vkCreateShaderModule", vkCreateShaderModule(device_, &ci, nullptr, &m))) {
      return VK_NULL_HANDLE;
    }
    modules_.push_back(m);
    return m;
  }

  bool MakePipeline(VkShaderModule vs, VkShaderModule fs, bool blend, VkPipelineLayout layout,
                    VkPipeline* out) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    if (blend) {
      att.blendEnable = VK_TRUE;
      att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      att.colorBlendOp = VK_BLEND_OP_ADD;
      att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dynamic;
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rendering;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vertex;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    if (!Check("vkCreateGraphicsPipelines",
               vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, out))) {
      return false;
    }
    pipelines_.push_back(*out);
    return true;
  }

  void BeginRendering(const Image& target, bool clear) {
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kWidth, kHeight}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd_, &ri);
    const VkViewport viewport{0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
  }

  bool Setup() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "theft4-gpu-bench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (!Check("vkCreateInstance", vkCreateInstance(&ici, nullptr, &instance_))) return false;
    uint32_t n = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkResult er = vkEnumeratePhysicalDevices(instance_, &n, &physical);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !physical) {
      return Check("vkEnumeratePhysicalDevices", er == VK_SUCCESS ? VK_ERROR_UNKNOWN : er);
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = 0;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    if (!Check("vkCreateDevice", vkCreateDevice(physical, &dci, nullptr, &device_))) return false;
    vkGetDeviceQueue(device_, 0, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!Check("vkCreateCommandPool", vkCreateCommandPool(device_, &pci, nullptr, &pool_))) {
      return false;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    if (!Check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device_, &cai, &cmd_)) ||
        !Check("vkCreateFence", vkCreateFence(device_, &fci, nullptr, &fence_)) ||
        !Check("vkCreateQueryPool", vkCreateQueryPool(device_, &qpi, nullptr, &queries_))) {
      return false;
    }

    // Buffers: per-draw uniforms, staging for the small textures and the upload.
    if (!MakeBuffer(VkDeviceSize(kDraws) * kUboStride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo_) ||
        !MakeBuffer(kUploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_)) {
      return false;
    }
    FillDrawData(static_cast<uint8_t*>(ubo_.mapped));
    // Images: 4 targets (the first is the draws' and fill target), 16 textures, the upload image.
    const VkImageUsageFlags rt_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    for (Image& t : targets_) {
      if (!MakeImage(kWidth, kHeight, rt_usage, &t)) return false;
    }
    for (Image& t : textures_) {
      if (!MakeImage(kTextureSize, kTextureSize,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &t)) {
        return false;
      }
    }
    if (!MakeImage(kUploadSize, kUploadSize, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &upload_)) {
      return false;
    }
    // Texture contents through the staging buffer, then every layout as the parts need it.
    const uint64_t texture_texels = uint64_t(kTextureSize) * kTextureSize;
    for (uint32_t i = 0; i < kTextures; ++i) {
      FillTexels(static_cast<uint32_t*>(staging_.mapped) + i * texture_texels, texture_texels, i);
    }
    if (!Begin()) return false;
    for (uint32_t i = 0; i < kTextures; ++i) {
      Barrier(cmd_, textures_[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy region{};
      region.bufferOffset = VkDeviceSize(i) * texture_texels * 4;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {kTextureSize, kTextureSize, 1};
      vkCmdCopyBufferToImage(cmd_, staging_.buffer, textures_[i].image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      Barrier(cmd_, textures_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT);
    }
    for (Image& t : targets_) {
      Barrier(cmd_, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    Barrier(cmd_, upload_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!SubmitAndWait()) return false;

    // Descriptors: one set per texture, the uniform slice through a dynamic offset.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.0f;
    if (!Check("vkCreateSampler", vkCreateSampler(device_, &sci, nullptr, &sampler_))) return false;
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = bindings;
    if (!Check("vkCreateDescriptorSetLayout",
               vkCreateDescriptorSetLayout(device_, &dli, nullptr, &set_layout_))) {
      return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kTextures},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTextures}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kTextures;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    if (!Check("vkCreateDescriptorPool",
               vkCreateDescriptorPool(device_, &dpi, nullptr, &descriptor_pool_))) {
      return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(kTextures, set_layout_);
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = descriptor_pool_;
    dsa.descriptorSetCount = kTextures;
    dsa.pSetLayouts = layouts.data();
    if (!Check("vkAllocateDescriptorSets", vkAllocateDescriptorSets(device_, &dsa, sets_))) {
      return false;
    }
    for (uint32_t i = 0; i < kTextures; ++i) {
      VkDescriptorBufferInfo buffer_info{ubo_.buffer, 0, kUboStride};
      VkDescriptorImageInfo image_info{sampler_, textures_[i].view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
      writes[0].dstSet = sets_[i];
      writes[0].dstBinding = 0;
      writes[0].descriptorCount = 1;
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[0].pBufferInfo = &buffer_info;
      writes[1].dstSet = sets_[i];
      writes[1].dstBinding = 1;
      writes[1].descriptorCount = 1;
      writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[1].pImageInfo = &image_info;
      vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    if (!Check("vkCreatePipelineLayout",
               vkCreatePipelineLayout(device_, &pli, nullptr, &layout_))) {
      return false;
    }
    using namespace gpu_bench_shaders;
    VkShaderModule quad = Module(kSpv_quad_vert, sizeof(kSpv_quad_vert));
    VkShaderModule textured = Module(kSpv_textured_frag, sizeof(kSpv_textured_frag));
    VkShaderModule gray = Module(kSpv_gray_frag, sizeof(kSpv_gray_frag));
    VkShaderModule full = Module(kSpv_fullscreen_vert, sizeof(kSpv_fullscreen_vert));
    VkShaderModule heavy = Module(kSpv_heavy_frag, sizeof(kSpv_heavy_frag));
    if (!quad || !textured || !gray || !full || !heavy) return false;
    const uint64_t t0 = NowNs();
    for (uint32_t p = 0; p < kPipelines; ++p) {
      if (!MakePipeline(quad, (p & 1) ? gray : textured, (p & 2) != 0, layout_, &variants_[p])) {
        return false;
      }
    }
    if (!MakePipeline(full, heavy, false, layout_, &fill_)) return false;
    SMOKE_INFO("gpu bench: NVK 5 pipelines created in %.1f ms", double(NowNs() - t0) / 1e6);
    return true;
  }

  bool Draws() {
    std::vector<double> record, submit, gpu, wall;
    for (uint32_t f = 0; f < kFrames; ++f) {
      const uint64_t t0 = NowNs();
      if (!Begin()) return false;
      vkCmdResetQueryPool(cmd_, queries_, 0, 2);
      vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
      BeginRendering(targets_[0], true);
      for (uint32_t i = 0; i < kDraws; ++i) {
        if (i % kPipelineEvery == 0) {
          vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            variants_[(i / kPipelineEvery) % kPipelines]);
        }
        const uint32_t offset = i * kUboStride;
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                &sets_[(i / kTextureEvery) % kTextures], 1, &offset);
        const DkScissor s = ScissorOf(i);
        const VkRect2D scissor{{int32_t(s.x), int32_t(s.y)}, {s.width, s.height}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdDraw(cmd_, 6, 1, 0, 0);
      }
      vkCmdEndRendering(cmd_);
      vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
      const uint64_t t1 = NowNs();
      uint64_t submit_ns = 0;
      if (!SubmitAndWait(&submit_ns)) return false;
      const uint64_t t2 = NowNs();
      record.push_back(double(t1 - t0) / 1000.0 / kDraws);
      submit.push_back(double(submit_ns) / 1e6);
      gpu.push_back(GpuMs(0));
      wall.push_back(double(t2 - t0) / 1e6);
    }
    r_.record_us_per_draw = Median(record);
    r_.submit_ms = Median(submit);
    r_.frame_gpu_ms = Median(gpu);
    r_.frame_wall_ms = Median(wall);
    return true;
  }

  bool Fences() {
    std::vector<double> us;
    for (uint32_t i = 0; i < kFenceRounds; ++i) {
      const uint64_t t0 = NowNs();
      if (!Begin() || !SubmitAndWait()) return false;
      us.push_back(double(NowNs() - t0) / 1000.0);
    }
    r_.fence_us_median = Median(us);
    r_.fence_us_p95 = Percentile(us, 0.95);
    return true;
  }

  bool Upload() {
    std::vector<double> copy_ms, wall, gpu;
    for (uint32_t i = 0; i < kUploads; ++i) {
      const uint64_t t0 = NowNs();
      FillTexels(static_cast<uint32_t*>(staging_.mapped), kUploadBytes / 4, 100 + i);
      const uint64_t t1 = NowNs();
      if (!Begin()) return false;
      vkCmdResetQueryPool(cmd_, queries_, 0, 2);
      vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {kUploadSize, kUploadSize, 1};
      vkCmdCopyBufferToImage(cmd_, staging_.buffer, upload_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
      if (!SubmitAndWait()) return false;
      const uint64_t t2 = NowNs();
      copy_ms.push_back(double(t1 - t0) / 1e6);
      wall.push_back(double(t2 - t1) / 1e6);
      gpu.push_back(GpuMs(0));
    }
    // The memcpy column is the pattern generation plus the write into the mapping.
    r_.upload_memcpy_ms = Median(copy_ms);
    r_.upload_wall_ms = Median(wall);
    r_.upload_gpu_ms = Median(gpu);
    return true;
  }

  bool Fill() {
    if (!Begin()) return false;
    vkCmdResetQueryPool(cmd_, queries_, 0, 2);
    BeginRendering(targets_[0], false);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, fill_);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    for (uint32_t p = 0; p < kFillPasses; ++p) vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    vkCmdEndRendering(cmd_);
    if (!SubmitAndWait()) return false;
    r_.fill_gpu_ms_per_pass = GpuMs(0) / kFillPasses;
    return true;
  }

  bool Targets() {
    const uint64_t t0 = NowNs();
    if (!Begin()) return false;
    vkCmdResetQueryPool(cmd_, queries_, 0, 2);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    for (uint32_t c = 0; c < kTargetChanges; ++c) {
      BeginRendering(targets_[c % kTargets], false);
      vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, variants_[0]);
      const uint32_t offset = (c % kDraws) * kUboStride;
      vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[0], 1,
                              &offset);
      vkCmdDraw(cmd_, 6, 1, 0, 0);
      vkCmdEndRendering(cmd_);
    }
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    const uint64_t t1 = NowNs();
    if (!SubmitAndWait()) return false;
    r_.rt_record_us_per_change = double(t1 - t0) / 1000.0 / kTargetChanges;
    r_.rt_gpu_ms = GpuMs(0);
    return true;
  }

  void Teardown() {
    if (!device_) {
      if (instance_) vkDestroyInstance(instance_, nullptr);
      return;
    }
    vkDeviceWaitIdle(device_);
    for (VkPipeline p : pipelines_) vkDestroyPipeline(device_, p, nullptr);
    for (VkShaderModule m : modules_) vkDestroyShaderModule(device_, m, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    for (Image* i : images_) {
      vkDestroyImageView(device_, i->view, nullptr);
      vkDestroyImage(device_, i->image, nullptr);
      vkFreeMemory(device_, i->memory, nullptr);
    }
    for (Buffer* b : buffers_) {
      vkDestroyBuffer(device_, b->buffer, nullptr);
      vkFreeMemory(device_, b->memory, nullptr);
    }
    if (queries_) vkDestroyQueryPool(device_, queries_, nullptr);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }

  BenchResult r_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkQueryPool queries_ = VK_NULL_HANDLE;
  Buffer ubo_, staging_;
  Image targets_[kTargets], textures_[kTextures], upload_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet sets_[kTextures] = {};
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline variants_[kPipelines] = {}, fill_ = VK_NULL_HANDLE;
  std::vector<VkPipeline> pipelines_;
  std::vector<VkShaderModule> modules_;
  std::vector<Image*> images_;
  std::vector<Buffer*> buffers_;
};

// ── deko3d ──────────────────────────────────────────────────────────────────

class DekoBench {
 public:
  BenchResult Run() {
    if (Setup()) {
      r_.ok = Draws() && Fences() && Upload() && Fill() && Targets();
    }
    Teardown();
    return r_;
  }

 private:
  struct DkshHeader {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
  };

  static void DebugCallback(void* user, const char* context, DkResult result, const char* message) {
    auto* self = static_cast<DekoBench*>(user);
    if (self->r_.failure.empty()) {
      self->r_.failure = std::string("deko3d ") + (context ? context : "?") + ": " +
                         (message ? message : "?") + " (DkResult " + std::to_string(int(result)) +
                         ")";
    }
    SMOKE_INFO("gpu bench: deko3d debug: %s: %s (%d)", context ? context : "?",
               message ? message : "?", int(result));
  }

  // A bump allocator over one memory block.
  struct Pool {
    DkMemBlock block = nullptr;
    uint32_t used = 0, size = 0;
    uint32_t Take(uint32_t bytes, uint32_t align) {
      used = (used + align - 1) & ~(align - 1);
      const uint32_t at = used;
      used += bytes;
      return used <= size ? at : UINT32_MAX;
    }
    uint8_t* Cpu(uint32_t at) const { return static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block)) + at; }
    DkGpuAddr Gpu(uint32_t at) const { return dkMemBlockGetGpuAddr(block) + at; }
  };

  bool MakePool(uint32_t size, uint32_t flags, Pool* out, const char* what) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, device_, (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
    m.flags = flags;
    out->block = dkMemBlockCreate(&m);
    out->size = m.size;
    if (!out->block) {
      r_.failure = std::string("dkMemBlockCreate ") + what;
      return false;
    }
    return true;
  }

  bool LoadShader(const uint8_t* dksh, DkShader* out) {
    DkshHeader h;
    std::memcpy(&h, dksh, sizeof(h));
    const uint32_t at = code_.Take(h.code_sz, DK_SHADER_CODE_ALIGNMENT);
    if (at == UINT32_MAX) {
      r_.failure = "shader code pool full";
      return false;
    }
    std::memcpy(code_.Cpu(at), dksh + h.control_sz, h.code_sz);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, code_.block, at);
    m.control = dksh;
    dkShaderInitialize(out, &m);
    if (!dkShaderIsValid(out)) {
      r_.failure = "dkShaderInitialize";
      return false;
    }
    return true;
  }

  bool MakeImage(uint32_t w, uint32_t h, uint32_t flags, DkImage* out, DkImageView* view) {
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = flags;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const uint32_t at = images_.Take(uint32_t(dkImageLayoutGetSize(&layout)),
                                     dkImageLayoutGetAlignment(&layout));
    if (at == UINT32_MAX) {
      r_.failure = "image pool full";
      return false;
    }
    dkImageInitialize(out, &layout, images_.block, at);
    dkImageViewDefaults(view, out);
    return true;
  }

  // A fresh command list in the command memory (the GPU is idle between lists).
  void BeginList() {
    dkCmdBufClear(cmd_);
    dkCmdBufAddMemory(cmd_, data_.block, cmd_at_, kCmdBytes);
    dkCmdBufBindImageDescriptorSet(cmd_, data_.Gpu(image_descriptors_at_), kTextures);
    dkCmdBufBindSamplerDescriptorSet(cmd_, data_.Gpu(sampler_descriptor_at_), 1);
  }

  // Submits and waits on a fence; submit_ns gets dkQueueSubmitCommands + flush.
  bool SubmitAndWait(uint64_t* submit_ns = nullptr) {
    DkCmdList list = dkCmdBufFinishList(cmd_);
    const uint64_t t0 = NowNs();
    dkQueueSubmitCommands(queue_, list);
    DkFence fence;
    dkQueueSignalFence(queue_, &fence, true);
    if (submit_ns) *submit_ns = NowNs() - t0;
    if (dkFenceWait(&fence, -1) != DkResult_Success || dkQueueIsInErrorState(queue_)) {
      if (r_.failure.empty()) r_.failure = "dkFenceWait / queue in error state";
      return false;
    }
    return r_.failure.empty();
  }

  void Stamp(uint32_t slot) {
    dkCmdBufReportCounter(cmd_, DkCounter_Timestamp, data_.Gpu(stamps_at_ + slot * 16));
  }

  // The report is 16 bytes (value, timestamp); the timestamp is the second word.
  double GpuMs() {
    const auto* s = reinterpret_cast<const volatile uint64_t*>(data_.Cpu(stamps_at_));
    const uint64_t a = s[1] ? s[1] : s[0], b = s[3] ? s[3] : s[2];
    if (!logged_stamps_) {
      logged_stamps_ = true;
      SMOKE_INFO("gpu bench: deko3d timestamp reports (raw words): %016" PRIX64 " %016" PRIX64
                 " / %016" PRIX64 " %016" PRIX64,
                 uint64_t(s[0]), uint64_t(s[1]), uint64_t(s[2]), uint64_t(s[3]));
    }
    return double(dkTimestampToNs(b - a)) / 1e6;
  }

  void BindTarget(uint32_t t) {
    const DkImageView* colors[1] = {&target_views_[t]};
    dkCmdBufBindRenderTargets(cmd_, colors, 1, nullptr);
    const DkViewport viewport{0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd_, 0, &viewport, 1);
    const DkScissor scissor{0, 0, kWidth, kHeight};
    dkCmdBufSetScissors(cmd_, 0, &scissor, 1);
  }

  void BindStaticState() {
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = false;
    ds.depthWriteEnable = false;
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    DkMultisampleState ms;
    dkMultisampleStateDefaults(&ms);
    dkCmdBufBindRasterizerState(cmd_, &rs);
    dkCmdBufBindDepthStencilState(cmd_, &ds);
    dkCmdBufBindColorWriteState(cmd_, &cw);
    dkCmdBufBindMultisampleState(cmd_, &ms);
  }

  // Pipeline variant p: pixel shader p & 1, blend p & 2 (as the NVK pipelines).
  void BindVariant(uint32_t p) {
    const DkShader* shaders[2] = {&quad_, (p & 1) ? &gray_ : &textured_};
    dkCmdBufBindShaders(cmd_, DkStageFlag_GraphicsMask, shaders, 2);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, (p & 2) != 0);
    dkCmdBufBindColorState(cmd_, &cs);
    if (p & 2) {
      DkBlendState bs;
      dkBlendStateDefaults(&bs);
      dkBlendStateSetFactors(&bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
                             DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
      dkCmdBufBindBlendStates(cmd_, 0, &bs, 1);
    }
  }

  bool Setup() {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.userData = this;
    dm.cbDebug = DebugCallback;
    device_ = dkDeviceCreate(&dm);
    if (!device_) {
      r_.failure = "dkDeviceCreate";
      return false;
    }
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device_);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio;
    queue_ = dkQueueCreate(&qm);
    if (!queue_) {
      r_.failure = "dkQueueCreate";
      return false;
    }
    // Memory: shader code; uniforms, descriptors, timestamps and the command
    // memory; the images; the host staging buffer (CPU uncached, as NVK's
    // host-coherent type).
    const uint32_t ubo_bytes = kDraws * kUboStride;
    if (!MakePool(256 * 1024, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                  DkMemBlockFlags_Code, &code_, "code") ||
        !MakePool(ubo_bytes + kCmdBytes + 64 * 1024,
                  DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, &data_, "data") ||
        !MakePool(48u << 20, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, &images_,
                  "images") ||
        !MakePool(uint32_t(kUploadBytes), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                  &staging_, "staging")) {
      return false;
    }
    ubo_at_ = data_.Take(ubo_bytes, DK_UNIFORM_BUF_ALIGNMENT);
    image_descriptors_at_ = data_.Take(kTextures * sizeof(DkImageDescriptor),
                                       DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    sampler_descriptor_at_ = data_.Take(sizeof(DkSamplerDescriptor), DK_SAMPLER_DESCRIPTOR_ALIGNMENT);
    stamps_at_ = data_.Take(64, 16);
    cmd_at_ = data_.Take(kCmdBytes, DK_CMDMEM_ALIGNMENT);
    if (cmd_at_ == UINT32_MAX) {
      r_.failure = "data pool full";
      return false;
    }
    FillDrawData(data_.Cpu(ubo_at_));

    using namespace gpu_bench_shaders;
    if (!LoadShader(kDksh_quad_vert, &quad_) || !LoadShader(kDksh_textured_frag, &textured_) ||
        !LoadShader(kDksh_gray_frag, &gray_) || !LoadShader(kDksh_fullscreen_vert, &full_) ||
        !LoadShader(kDksh_heavy_frag, &heavy_)) {
      return false;
    }
    for (uint32_t t = 0; t < kTargets; ++t) {
      if (!MakeImage(kWidth, kHeight, DkImageFlags_UsageRender, &targets_[t], &target_views_[t])) {
        return false;
      }
    }
    for (uint32_t t = 0; t < kTextures; ++t) {
      if (!MakeImage(kTextureSize, kTextureSize, 0, &textures_[t], &texture_views_[t])) {
        return false;
      }
    }
    if (!MakeImage(kUploadSize, kUploadSize, 0, &upload_, &upload_view_)) return false;

    // Descriptors written by the CPU into uncached memory.
    for (uint32_t t = 0; t < kTextures; ++t) {
      dkImageDescriptorInitialize(
          reinterpret_cast<DkImageDescriptor*>(data_.Cpu(image_descriptors_at_)) + t,
          &texture_views_[t], false, false);
    }
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    sampler.minFilter = DkFilter_Linear;
    sampler.magFilter = DkFilter_Linear;
    dkSamplerDescriptorInitialize(
        reinterpret_cast<DkSamplerDescriptor*>(data_.Cpu(sampler_descriptor_at_)), &sampler);

    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device_);
    cmd_ = dkCmdBufCreate(&cm);
    if (!cmd_) {
      r_.failure = "dkCmdBufCreate";
      return false;
    }
    // Texture contents through the staging buffer.
    const uint64_t texture_texels = uint64_t(kTextureSize) * kTextureSize;
    for (uint32_t i = 0; i < kTextures; ++i) {
      FillTexels(reinterpret_cast<uint32_t*>(staging_.Cpu(0)) + i * texture_texels, texture_texels, i);
    }
    BeginList();
    for (uint32_t i = 0; i < kTextures; ++i) {
      const DkCopyBuf src{staging_.Gpu(0) + i * texture_texels * 4, 0, 0};
      const DkImageRect rect{0, 0, 0, kTextureSize, kTextureSize, 1};
      dkCmdBufCopyBufferToImage(cmd_, &src, &texture_views_[i], &rect, 0);
    }
    dkCmdBufBarrier(cmd_, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    BindStaticState();
    return SubmitAndWait();
  }

  bool Draws() {
    std::vector<double> record, submit, gpu, wall;
    for (uint32_t f = 0; f < kFrames; ++f) {
      const uint64_t t0 = NowNs();
      BeginList();
      Stamp(0);
      BindTarget(0);
      dkCmdBufClearColorFloat(cmd_, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
      for (uint32_t i = 0; i < kDraws; ++i) {
        if (i % kPipelineEvery == 0) BindVariant((i / kPipelineEvery) % kPipelines);
        if (i % kTextureEvery == 0) {
          dkCmdBufBindTexture(cmd_, DkStage_Fragment, 0,
                              dkMakeTextureHandle((i / kTextureEvery) % kTextures, 0));
        }
        dkCmdBufBindUniformBuffer(cmd_, DkStage_Vertex, 0, data_.Gpu(ubo_at_ + i * kUboStride),
                                  kUboStride);
        const DkScissor scissor = ScissorOf(i);
        dkCmdBufSetScissors(cmd_, 0, &scissor, 1);
        dkCmdBufDraw(cmd_, DkPrimitive_Triangles, 6, 1, 0, 0);
      }
      Stamp(1);
      const uint64_t t1 = NowNs();
      uint64_t submit_ns = 0;
      if (!SubmitAndWait(&submit_ns)) return false;
      const uint64_t t2 = NowNs();
      record.push_back(double(t1 - t0) / 1000.0 / kDraws);
      submit.push_back(double(submit_ns) / 1e6);
      gpu.push_back(GpuMs());
      wall.push_back(double(t2 - t0) / 1e6);
    }
    r_.record_us_per_draw = Median(record);
    r_.submit_ms = Median(submit);
    r_.frame_gpu_ms = Median(gpu);
    r_.frame_wall_ms = Median(wall);
    return true;
  }

  bool Fences() {
    std::vector<double> us;
    for (uint32_t i = 0; i < kFenceRounds; ++i) {
      const uint64_t t0 = NowNs();
      dkCmdBufClear(cmd_);
      dkCmdBufAddMemory(cmd_, data_.block, cmd_at_, kCmdBytes);
      if (!SubmitAndWait()) return false;
      us.push_back(double(NowNs() - t0) / 1000.0);
    }
    r_.fence_us_median = Median(us);
    r_.fence_us_p95 = Percentile(us, 0.95);
    return true;
  }

  bool Upload() {
    std::vector<double> copy_ms, wall, gpu;
    for (uint32_t i = 0; i < kUploads; ++i) {
      const uint64_t t0 = NowNs();
      FillTexels(reinterpret_cast<uint32_t*>(staging_.Cpu(0)), kUploadBytes / 4, 100 + i);
      const uint64_t t1 = NowNs();
      BeginList();
      Stamp(0);
      const DkCopyBuf src{staging_.Gpu(0), 0, 0};
      const DkImageRect rect{0, 0, 0, kUploadSize, kUploadSize, 1};
      dkCmdBufCopyBufferToImage(cmd_, &src, &upload_view_, &rect, 0);
      Stamp(1);
      if (!SubmitAndWait()) return false;
      const uint64_t t2 = NowNs();
      copy_ms.push_back(double(t1 - t0) / 1e6);
      wall.push_back(double(t2 - t1) / 1e6);
      gpu.push_back(GpuMs());
    }
    r_.upload_memcpy_ms = Median(copy_ms);
    r_.upload_wall_ms = Median(wall);
    r_.upload_gpu_ms = Median(gpu);
    return true;
  }

  bool Fill() {
    BeginList();
    BindTarget(0);
    const DkShader* shaders[2] = {&full_, &heavy_};
    dkCmdBufBindShaders(cmd_, DkStageFlag_GraphicsMask, shaders, 2);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkCmdBufBindColorState(cmd_, &cs);
    Stamp(0);
    for (uint32_t p = 0; p < kFillPasses; ++p) dkCmdBufDraw(cmd_, DkPrimitive_Triangles, 3, 1, 0, 0);
    Stamp(1);
    if (!SubmitAndWait()) return false;
    r_.fill_gpu_ms_per_pass = GpuMs() / kFillPasses;
    return true;
  }

  bool Targets() {
    const uint64_t t0 = NowNs();
    BeginList();
    BindVariant(0);
    dkCmdBufBindTexture(cmd_, DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
    Stamp(0);
    for (uint32_t c = 0; c < kTargetChanges; ++c) {
      BindTarget(c % kTargets);
      dkCmdBufBindUniformBuffer(cmd_, DkStage_Vertex, 0,
                                data_.Gpu(ubo_at_ + (c % kDraws) * kUboStride), kUboStride);
      dkCmdBufDraw(cmd_, DkPrimitive_Triangles, 6, 1, 0, 0);
    }
    Stamp(1);
    const uint64_t t1 = NowNs();
    if (!SubmitAndWait()) return false;
    r_.rt_record_us_per_change = double(t1 - t0) / 1000.0 / kTargetChanges;
    r_.rt_gpu_ms = GpuMs();
    return true;
  }

  void Teardown() {
    if (queue_) {
      dkQueueWaitIdle(queue_);
      dkQueueDestroy(queue_);
    }
    if (cmd_) dkCmdBufDestroy(cmd_);
    for (Pool* p : {&code_, &data_, &images_, &staging_}) {
      if (p->block) dkMemBlockDestroy(p->block);
    }
    if (device_) dkDeviceDestroy(device_);
  }

  // Command memory for one list: 5000 draws with their binds fit with room to spare.
  static constexpr uint32_t kCmdBytes = 8u << 20;

  BenchResult r_;
  DkDevice device_ = nullptr;
  DkQueue queue_ = nullptr;
  DkCmdBuf cmd_ = nullptr;
  Pool code_, data_, images_, staging_;
  uint32_t ubo_at_ = 0, image_descriptors_at_ = 0, sampler_descriptor_at_ = 0;
  uint32_t stamps_at_ = 0, cmd_at_ = 0;
  DkShader quad_, textured_, gray_, full_, heavy_;
  DkImage targets_[kTargets], textures_[kTextures], upload_;
  DkImageView target_views_[kTargets], texture_views_[kTextures], upload_view_;
  bool logged_stamps_ = false;
};

void ClockLog(bool error, const char* text) { Report(error, "gpu bench: %s", text); }

}  // namespace

bool RunGpuBench() {
  SMOKE_INFO("step 8 GPU benchmark: NVK and deko3d, the same workload, offscreen "
             "(docs/switch-port/13-deko3d-spike.md)");
  // The game's clocks: apm 0x92220008 in handheld mode, nothing raised.
  theft4::nx_clocks::Start(true, ClockLog);
  theft4::nx_clocks::Wanted wanted;
  theft4::nx_clocks::WaitStartup(3'000'000'000ull, &wanted);
  LogClocks("at the start");
  bool all = true;
  BenchResult rounds[2][2];
  for (int round = 1; round <= 2; ++round) {
    SMOKE_INFO("gpu bench: about to run NVK, round %d", round);
    rounds[round - 1][0] = VulkanBench().Run();
    LogResult("NVK", round, rounds[round - 1][0]);
    LogClocks("after NVK");
    SMOKE_INFO("gpu bench: about to run deko3d, round %d", round);
    rounds[round - 1][1] = DekoBench().Run();
    LogResult("deko3d", round, rounds[round - 1][1]);
    LogClocks("after deko3d");
    all = all && rounds[round - 1][0].ok && rounds[round - 1][1].ok;
  }
  // One summary line per part: NVK / deko3d, round 2 (round 1 above).
  const BenchResult& v = rounds[1][0];
  const BenchResult& d = rounds[1][1];
  if (v.ok && d.ok) {
    SMOKE_INFO("gpu bench SUMMARY (round 2, NVK / deko3d): CPU per draw %.2f / %.2f us; GPU per "
               "frame of %u draws %.2f / %.2f ms; submit+fence %.0f / %.0f us",
               v.record_us_per_draw, d.record_us_per_draw, kDraws, v.frame_gpu_ms, d.frame_gpu_ms,
               v.fence_us_median, d.fence_us_median);
    SMOKE_INFO("gpu bench SUMMARY (round 2, NVK / deko3d): upload 16 MiB GPU %.2f / %.2f ms; fill "
               "pass %.3f / %.3f ms; RT change CPU %.2f / %.2f us, GPU %.2f / %.2f ms",
               v.upload_gpu_ms, d.upload_gpu_ms, v.fill_gpu_ms_per_pass, d.fill_gpu_ms_per_pass,
               v.rt_record_us_per_change, d.rt_record_us_per_change, v.rt_gpu_ms, d.rt_gpu_ms);
  }
  SMOKE_INFO("%s", all ? "GPU BENCH OK" : "GPU BENCH FAIL (see the lines above)");
  return all;
}
