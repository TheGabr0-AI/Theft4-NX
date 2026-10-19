// Native renderer: the thin layer between the renderer's logic (what the
// game's PM4 stream asks for) and the graphics API. One implementation,
// Vulkan on NVK (vulkan_render_backend.cpp, step N2); the renderer never
// calls Vulkan directly, so another API (deko3d, docs/switch-port/
// 13-deko3d-spike.md) would be one more implementation of this interface.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

enum class ImageFormat : uint32_t {
  kRGBA8,
  kBGRA8,
  kRGBA16F,
  kDepth24Stencil8,
  kDepth32F,
};

struct ImageDesc {
  uint32_t width = 0, height = 0;
  ImageFormat format = ImageFormat::kRGBA8;
  bool render_target = false;
  bool sampled = false;
};

using ImageHandle = uint32_t;
constexpr ImageHandle kInvalidImage = 0;

using PipelineHandle = uint32_t;
constexpr PipelineHandle kInvalidPipeline = 0;

struct PipelineDesc {
  const uint32_t* vertex_spirv = nullptr;
  size_t vertex_spirv_bytes = 0;
  const uint32_t* pixel_spirv = nullptr;
  size_t pixel_spirv_bytes = 0;
  ImageFormat color_format = ImageFormat::kRGBA8;
  bool blend = false;
};

class RenderBackend {
 public:
  virtual ~RenderBackend() = default;
  virtual const char* name() const = 0;

  virtual ImageHandle CreateImage(const ImageDesc& desc) = 0;
  virtual void DestroyImage(ImageHandle image) = 0;
  // Tightly packed rows of the image's format.
  virtual bool UploadImage(ImageHandle image, const void* data, size_t bytes) = 0;
  virtual PipelineHandle CreatePipeline(const PipelineDesc& desc) = 0;

  // One frame of recorded work, submitted at EndFrame.
  virtual bool BeginFrame() = 0;
  virtual void BeginPass(ImageHandle color_target, const float* clear_rgba) = 0;
  virtual void EndPass() = 0;
  virtual void BindPipeline(PipelineHandle pipeline) = 0;
  virtual void BindTexture(uint32_t slot, ImageHandle image) = 0;
  // Bytes for the uniform block of the next draws (copied at the call).
  virtual void SetUniforms(const void* data, size_t bytes) = 0;
  virtual void Draw(uint32_t vertex_count) = 0;
  virtual void CopyImage(ImageHandle source, ImageHandle destination) = 0;
  virtual bool EndFrame() = 0;  // submits and waits

  // The image's pixels as RGBA8, rows packed (for the comparison PNGs).
  virtual bool ReadbackImage(ImageHandle image, std::vector<uint8_t>* rgba) = 0;
};

// The only implementation: Vulkan on the device the presenter already uses
// (NVK on Switch, any Vulkan driver on the PC).
std::unique_ptr<RenderBackend> CreateVulkanRenderBackend(const ui::vulkan::VulkanDevice* device);

}  // namespace rex::graphics::native
