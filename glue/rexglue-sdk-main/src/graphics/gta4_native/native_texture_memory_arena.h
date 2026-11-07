#ifndef REX_GRAPHICS_GTA4_NATIVE_NATIVE_TEXTURE_MEMORY_ARENA_H_
#define REX_GRAPHICS_GTA4_NATIVE_NATIVE_TEXTURE_MEMORY_ARENA_H_

// Texture images placed in large shared VkDeviceMemory blocks instead of one
// dedicated allocation each.
//
// Why: on the Switch every vkAllocateMemory is an nvMapCreate plus GPU address
// space and MapBufferEx calls (~1.9 ms of CPU per dedicated texture, measured by
// the Need for Speed: Most Wanted port on the same driver; sd_logs37 showed the
// same calls on the recorder while driving). Sub-allocating from 32 MB blocks
// pays that once per block (the NFSMW port went from 1.9 to 0.75 ms per texture,
// and a burst of 15 new textures from 28.5 to 11.3 ms). requiresDedicatedAllocation
// is never true on NVK, and the only benefit of a dedicated allocation,
// compression, does not apply to SAMPLED | TRANSFER_DST images such as these.
//
// Thread safety: Allocate runs on the recorder, Free also on the texture
// reclaimer thread, so both take the mutex.

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/device.h>

namespace rex::graphics::gta4_native {

class NativeTextureMemoryArena {
 public:
  static constexpr VkDeviceSize kBlockSize = VkDeviceSize(32) << 20;
  // Larger images keep a dedicated allocation (few of them, and they would
  // fragment the blocks).
  static constexpr VkDeviceSize kMaximumSubAllocation = VkDeviceSize(8) << 20;

  struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
  };

  bool Allocate(const ui::vulkan::VulkanDevice* device, uint32_t memory_type, VkDeviceSize size,
                VkDeviceSize alignment, Allocation& out) {
    if (!device || !size || size > kMaximumSubAllocation || memory_type == UINT32_MAX) {
      return false;
    }
    if (!alignment) alignment = 1;
    std::lock_guard lock(mutex_);
    for (auto& block : blocks_) {
      if (block->memory_type == memory_type && Place(*block, size, alignment, out)) {
        return true;
      }
    }
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = kBlockSize;
    info.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (device->functions().vkAllocateMemory(device->device(), &info, nullptr, &memory) !=
        VK_SUCCESS) {
      return false;
    }
    auto block = std::make_unique<Block>();
    block->memory = memory;
    block->memory_type = memory_type;
    block->free_ranges.emplace(0, kBlockSize);
    blocks_.push_back(std::move(block));
    ++blocks_created_;
    return Place(*blocks_.back(), size, alignment, out);
  }

  // Unknown memory (a dedicated allocation, or a block already released at
  // teardown) is ignored.
  void Free(const ui::vulkan::VulkanDevice* device, VkDeviceMemory memory, VkDeviceSize offset) {
    std::lock_guard lock(mutex_);
    auto block_it = std::find_if(blocks_.begin(), blocks_.end(),
                                 [memory](const auto& block) { return block->memory == memory; });
    if (block_it == blocks_.end()) return;
    Block& block = **block_it;
    const auto live = block.live.find(offset);
    if (live == block.live.end()) return;
    VkDeviceSize start = offset;
    VkDeviceSize length = live->second;
    block.used -= length;
    block.live.erase(live);
    auto next = block.free_ranges.lower_bound(start);
    if (next != block.free_ranges.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == start) {
        start = previous->first;
        length += previous->second;
        block.free_ranges.erase(previous);
      }
    }
    if (next != block.free_ranges.end() && start + length == next->first) {
      length += next->second;
      block.free_ranges.erase(next);
    }
    block.free_ranges.emplace(start, length);
    if (block.used == 0 && device) {
      // Keep one empty block per memory type for the next burst.
      const size_t empty_of_type = size_t(std::count_if(
          blocks_.begin(), blocks_.end(), [&block](const auto& other) {
            return other->memory_type == block.memory_type && other->used == 0;
          }));
      if (empty_of_type > 1) {
        device->functions().vkFreeMemory(device->device(), block.memory, nullptr);
        blocks_.erase(block_it);
      }
    }
  }

  void Destroy(const ui::vulkan::VulkanDevice* device) {
    std::lock_guard lock(mutex_);
    if (device) {
      for (const auto& block : blocks_) {
        device->functions().vkFreeMemory(device->device(), block->memory, nullptr);
      }
    }
    blocks_.clear();
  }

  // Bytes reserved in blocks, bytes in use, blocks ever created.
  void Stats(uint64_t& reserved, uint64_t& used, uint64_t& created) {
    std::lock_guard lock(mutex_);
    reserved = uint64_t(blocks_.size()) * kBlockSize;
    used = 0;
    for (const auto& block : blocks_) used += block->used;
    created = blocks_created_;
  }

 private:
  struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t memory_type = UINT32_MAX;
    VkDeviceSize used = 0;
    std::map<VkDeviceSize, VkDeviceSize> free_ranges;   // offset -> length
    std::unordered_map<VkDeviceSize, VkDeviceSize> live;  // offset -> length
  };

  static bool Place(Block& block, VkDeviceSize size, VkDeviceSize alignment, Allocation& out) {
    for (auto it = block.free_ranges.begin(); it != block.free_ranges.end(); ++it) {
      const VkDeviceSize start = it->first;
      const VkDeviceSize length = it->second;
      const VkDeviceSize aligned = (start + alignment - 1) / alignment * alignment;
      const VkDeviceSize padding = aligned - start;
      if (length < padding || length - padding < size) continue;
      block.free_ranges.erase(it);
      if (padding) block.free_ranges.emplace(start, padding);
      const VkDeviceSize remainder = length - padding - size;
      if (remainder) block.free_ranges.emplace(aligned + size, remainder);
      block.live.emplace(aligned, size);
      block.used += size;
      out.memory = block.memory;
      out.offset = aligned;
      return true;
    }
    return false;
  }

  std::mutex mutex_;
  std::vector<std::unique_ptr<Block>> blocks_;
  uint64_t blocks_created_ = 0;
};

}  // namespace rex::graphics::gta4_native

#endif  // REX_GRAPHICS_GTA4_NATIVE_NATIVE_TEXTURE_MEMORY_ARENA_H_
