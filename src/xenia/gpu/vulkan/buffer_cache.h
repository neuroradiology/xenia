/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_BUFFER_CACHE_H_
#define XENIA_GPU_VULKAN_BUFFER_CACHE_H_

#include "xenia/base/xxhash.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/circular_buffer.h"
#include "xenia/ui/vulkan/fenced_pools.h"
#include "xenia/ui/vulkan/vulkan.h"
#include "xenia/ui/vulkan/vulkan_device.h"

#include "third_party/vulkan/vk_mem_alloc.h"

#include <map>
#include <unordered_map>

namespace xe {
namespace gpu {
namespace vulkan {

// Efficiently manages buffers of various kinds.
// Used primarily for uploading index and vertex data from guest memory and
// transient data like shader constants.
class BufferCache {
 public:
  BufferCache(RegisterFile* register_file, Memory* memory,
              ui::vulkan::VulkanDevice* device, size_t capacity);
  ~BufferCache();

  VkResult Initialize();
  void Shutdown();

  // Descriptor set containing the dynamic uniform buffer used for constant
  // uploads. Used in conjunction with a dynamic offset returned by
  // UploadConstantRegisters.
  // The set contains two bindings:
  //   binding = 0: for use in vertex shaders
  //   binding = 1: for use in fragment shaders
  VkDescriptorSet constant_descriptor_set() const {
    return constant_descriptor_set_;
  }
  VkDescriptorSetLayout constant_descriptor_set_layout() const {
    return constant_descriptor_set_layout_;
  }

  // Descriptor set containing vertex buffers stored in storage buffers.
  // This set contains one binding with an array of 32 storage buffers.
  VkDescriptorSetLayout vertex_descriptor_set_layout() const {
    return vertex_descriptor_set_layout_;
  }

  // Uploads the constants specified in the register maps to the transient
  // uniform storage buffer.
  // The registers are tightly packed in order as [floats, ints, bools].
  // Returns an offset that can be used with the transient_descriptor_set or
  // VK_WHOLE_SIZE if the constants could not be uploaded (OOM).
  // The returned offsets may alias.
  std::pair<VkDeviceSize, VkDeviceSize> UploadConstantRegisters(
      VkCommandBuffer command_buffer,
      const Shader::ConstantRegisterMap& vertex_constant_register_map,
      const Shader::ConstantRegisterMap& pixel_constant_register_map,
      VkFence fence);

  // Uploads index buffer data from guest memory, possibly eliding with
  // recently uploaded data or cached copies.
  // Returns a buffer and offset that can be used with vkCmdBindIndexBuffer.
  // Size will be VK_WHOLE_SIZE if the data could not be uploaded (OOM).
  std::pair<VkBuffer, VkDeviceSize> UploadIndexBuffer(
      VkCommandBuffer command_buffer, uint32_t source_addr,
      uint32_t source_length, xenos::IndexFormat format, VkFence fence);

  // Uploads vertex buffer data from guest memory, possibly eliding with
  // recently uploaded data or cached copies.
  // Returns a buffer and offset that can be used with vkCmdBindVertexBuffers.
  // Size will be VK_WHOLE_SIZE if the data could not be uploaded (OOM).
  std::pair<VkBuffer, VkDeviceSize> UploadVertexBuffer(
      VkCommandBuffer command_buffer, uint32_t source_addr,
      uint32_t source_length, xenos::Endian endian, VkFence fence);

  // Prepares and returns a vertex descriptor set.
  VkDescriptorSet PrepareVertexSet(
      VkCommandBuffer setup_buffer, VkFence fence,
      const std::vector<Shader::VertexBinding>& vertex_bindings);

  // Flushes all pending data to the GPU.
  // Until this is called the GPU is not guaranteed to see any data.
  // The given command buffer will be used to queue up events so that the
  // cache can determine when data has been consumed.
  void Flush(VkCommandBuffer command_buffer);

  // Marks the cache as potentially invalid.
  // This is not as strong as ClearCache and is a hint that any and all data
  // should be verified before being reused.
  void InvalidateCache();

  // Clears all cached content and prevents future elision with pending data.
  void ClearCache();

  // Wipes all data no longer needed.
  void Scavenge();

 private:
  // This represents an uploaded vertex buffer.
  struct VertexBuffer {
    uint32_t guest_address;
    uint32_t size;

    VmaAllocation alloc;
    VmaAllocationInfo alloc_info;
  };

  VkResult CreateVertexDescriptorPool();
  void FreeVertexDescriptorPool();

  VkResult CreateConstantDescriptorSet();
  void FreeConstantDescriptorSet();

  void HashVertexBindings(
      XXH3_state_t* hash_state,
      const std::vector<Shader::VertexBinding>& vertex_bindings);

  // Allocates a block of memory in the transient buffer.
  // When memory is not available fences are checked and space is reclaimed.
  // Returns VK_WHOLE_SIZE if requested amount of memory is not available.
  VkDeviceSize AllocateTransientData(VkDeviceSize length, VkFence fence);
  // Tries to allocate a block of memory in the transient buffer.
  // Returns VK_WHOLE_SIZE if requested amount of memory is not available.
  VkDeviceSize TryAllocateTransientData(VkDeviceSize length, VkFence fence);
  // Finds a block of data in the transient buffer sourced from the specified
  // guest address and length.
  VkDeviceSize FindCachedTransientData(uint32_t guest_address,
                                       uint32_t guest_length);
  // Adds a block of data to the frame cache.
  void CacheTransientData(uint32_t guest_address, uint32_t guest_length,
                          VkDeviceSize offset);

  RegisterFile* register_file_ = nullptr;
  Memory* memory_ = nullptr;
  ui::vulkan::VulkanDevice* device_ = nullptr;

  VkDeviceMemory gpu_memory_pool_ = nullptr;
  VmaAllocator mem_allocator_ = nullptr;

  // Staging ringbuffer we cycle through fast. Used for data we don't
  // plan on keeping past the current frame.
  std::unique_ptr<ui::vulkan::CircularBuffer> transient_buffer_ = nullptr;
  std::map<uint32_t, std::pair<uint32_t, VkDeviceSize>> transient_cache_;

  // Vertex buffer descriptors
  std::unique_ptr<ui::vulkan::DescriptorPool> vertex_descriptor_pool_ = nullptr;
  VkDescriptorSetLayout vertex_descriptor_set_layout_ = nullptr;

  // Current frame vertex sets.
  std::unordered_map<uint64_t, VkDescriptorSet> vertex_sets_;

  // Descriptor set used to hold vertex/pixel shader float constants
  VkDescriptorPool constant_descriptor_pool_ = nullptr;
  VkDescriptorSetLayout constant_descriptor_set_layout_ = nullptr;
  VkDescriptorSet constant_descriptor_set_ = nullptr;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BUFFER_CACHE_H_
