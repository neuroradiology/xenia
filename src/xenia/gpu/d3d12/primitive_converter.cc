/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/primitive_converter.h"

#include <algorithm>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DEFINE_bool(d3d12_convert_quads_to_triangles, false,
            "Convert quad lists to triangle lists on the CPU instead of using "
            "a geometry shader. Not recommended for playing, for debugging "
            "primarily (because PIX fails to display vertices when a geometry "
            "shader is used), and this way quads can't be discarded correctly "
            "when the game uses vertex kill functionality.",
            "D3D12");

namespace xe {
namespace gpu {
namespace d3d12 {

PrimitiveConverter::PrimitiveConverter(D3D12CommandProcessor& command_processor,
                                       const RegisterFile& register_file,
                                       Memory& memory,
                                       TraceWriter& trace_writer)
    : command_processor_(command_processor),
      register_file_(register_file),
      memory_(memory),
      trace_writer_(trace_writer) {
  system_page_size_ = uint32_t(memory::page_size());
}

PrimitiveConverter::~PrimitiveConverter() { Shutdown(); }

bool PrimitiveConverter::Initialize() {
  auto& provider = command_processor_.GetD3D12Context().GetD3D12Provider();
  auto device = provider.GetDevice();
  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed =
      provider.GetHeapFlagCreateNotZeroed();

  // There can be at most 65535 indices in a Xenos draw call (16 bit index
  // count), but they can be up to 4 bytes large, and conversion can add more
  // indices (almost triple the count for triangle strips or fans, for
  // instance).
  buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, std::max(sizeof(uint32_t) * 3 * 65535,
                         ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize));

  // Create the static index buffer for non-indexed drawing.
  D3D12_RESOURCE_DESC static_ib_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      static_ib_desc, kStaticIBTotalCount * sizeof(uint16_t),
      D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, heap_flag_create_not_zeroed,
          &static_ib_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&static_ib_upload_)))) {
    XELOGE(
        "Failed to create the upload buffer for the primitive conversion "
        "static index buffer");
    Shutdown();
    return false;
  }
  D3D12_RANGE static_ib_read_range;
  static_ib_read_range.Begin = 0;
  static_ib_read_range.End = 0;
  void* static_ib_mapping;
  if (FAILED(static_ib_upload_->Map(0, &static_ib_read_range,
                                    &static_ib_mapping))) {
    XELOGE(
        "Failed to map the upload buffer for the primitive conversion "
        "static index buffer");
    Shutdown();
    return false;
  }
  uint16_t* static_ib_data = reinterpret_cast<uint16_t*>(static_ib_mapping);
  // Triangle fans as triangle lists.
  // https://docs.microsoft.com/en-us/windows/desktop/direct3d9/triangle-fans
  // Ordered as (v1, v2, v0), (v2, v3, v0).
  uint16_t* static_ib_data_pointer =
      &static_ib_data[kStaticIBTriangleFanOffset];
  for (uint32_t i = 2; i < kMaxNonIndexedVertices; ++i) {
    *(static_ib_data_pointer++) = i - 1;
    *(static_ib_data_pointer++) = i;
    *(static_ib_data_pointer++) = 0;
  }
  static_ib_data_pointer = &static_ib_data[kStaticIBQuadOffset];
  for (uint32_t i = 0; i < (kMaxNonIndexedVertices >> 2); ++i) {
    uint32_t quad_index = i << 2;
    *(static_ib_data_pointer++) = quad_index;
    *(static_ib_data_pointer++) = quad_index + 1;
    *(static_ib_data_pointer++) = quad_index + 2;
    *(static_ib_data_pointer++) = quad_index;
    *(static_ib_data_pointer++) = quad_index + 2;
    *(static_ib_data_pointer++) = quad_index + 3;
  }
  static_ib_upload_->Unmap(0, nullptr);
  // Not uploaded yet.
  static_ib_upload_submission_ = UINT64_MAX;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, heap_flag_create_not_zeroed,
          &static_ib_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&static_ib_)))) {
    XELOGE("Failed to create the primitive conversion static index buffer");
    Shutdown();
    return false;
  }
  static_ib_gpu_address_ = static_ib_->GetGPUVirtualAddress();

  memory_regions_invalidated_.store(0ull, std::memory_order_relaxed);
  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(
          MemoryInvalidationCallbackThunk, this);

  return true;
}

void PrimitiveConverter::Shutdown() {
  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(
        memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }
  ui::d3d12::util::ReleaseAndNull(static_ib_);
  ui::d3d12::util::ReleaseAndNull(static_ib_upload_);
  buffer_pool_.reset();
}

void PrimitiveConverter::ClearCache() { buffer_pool_->ClearCache(); }

void PrimitiveConverter::CompletedSubmissionUpdated() {
  if (static_ib_upload_ && command_processor_.GetCompletedSubmission() >=
                               static_ib_upload_submission_) {
    // Completely uploaded - release the upload buffer.
    static_ib_upload_->Release();
    static_ib_upload_ = nullptr;
  }
}

void PrimitiveConverter::BeginSubmission() {
  // Got a command list now - upload and transition the static index buffer if
  // needed.
  if (static_ib_upload_ && static_ib_upload_submission_ == UINT64_MAX) {
    command_processor_.GetDeferredCommandList().D3DCopyResource(
        static_ib_, static_ib_upload_);
    command_processor_.PushTransitionBarrier(static_ib_,
                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_INDEX_BUFFER);
    static_ib_upload_submission_ = command_processor_.GetCurrentSubmission();
  }
}

void PrimitiveConverter::BeginFrame() {
  buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
  converted_indices_cache_.clear();
  memory_regions_used_ = 0;
}

xenos::PrimitiveType PrimitiveConverter::GetReplacementPrimitiveType(
    xenos::PrimitiveType type) {
  switch (type) {
    case xenos::PrimitiveType::kTriangleFan:
      return xenos::PrimitiveType::kTriangleList;
    case xenos::PrimitiveType::kLineLoop:
      return xenos::PrimitiveType::kLineStrip;
    case xenos::PrimitiveType::kQuadList:
      if (cvars::d3d12_convert_quads_to_triangles) {
        return xenos::PrimitiveType::kTriangleList;
      }
      break;
    default:
      break;
  }
  return type;
}

PrimitiveConverter::ConversionResult PrimitiveConverter::ConvertPrimitives(
    xenos::PrimitiveType source_type, uint32_t address, uint32_t index_count,
    xenos::IndexFormat index_format, xenos::Endian index_endianness,
    D3D12_GPU_VIRTUAL_ADDRESS& gpu_address_out, uint32_t& index_count_out) {
  bool index_32bit = index_format == xenos::IndexFormat::kInt32;
  const auto& regs = register_file_;
  bool reset = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;
  // Swap the reset index because we will be comparing unswapped values to it.
  uint32_t reset_index = xenos::GpuSwap(
      regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX].u32, index_endianness);
  // If the specified reset index is the same as the one used by Direct3D 12
  // (0xFFFF or 0xFFFFFFFF - in the pipeline cache, we use the former for
  // 16-bit and the latter for 32-bit indices), we can use the buffer directly.
  uint32_t reset_index_host = index_32bit ? 0xFFFFFFFFu : 0xFFFFu;

  // Degenerate line loops are just lines.
  if (source_type == xenos::PrimitiveType::kLineLoop && index_count <= 2) {
    source_type = xenos::PrimitiveType::kLineStrip;
  }

  // Check if need to convert at all.
  if (source_type == xenos::PrimitiveType::kTriangleStrip ||
      source_type == xenos::PrimitiveType::kLineStrip) {
    if (!reset || reset_index == reset_index_host) {
      return ConversionResult::kConversionNotNeeded;
    }
  } else if (source_type == xenos::PrimitiveType::kQuadList) {
    if (!cvars::d3d12_convert_quads_to_triangles) {
      return ConversionResult::kConversionNotNeeded;
    }
  } else if (source_type != xenos::PrimitiveType::kTriangleFan &&
             source_type != xenos::PrimitiveType::kLineLoop) {
    return ConversionResult::kConversionNotNeeded;
  }

#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  // Exit early for clearly empty draws, without even reading the memory.
  uint32_t index_count_min;
  if (source_type == xenos::PrimitiveType::kLineStrip ||
      source_type == xenos::PrimitiveType::kLineLoop) {
    index_count_min = 2;
  } else if (source_type == xenos::PrimitiveType::kQuadList) {
    index_count_min = 4;
  } else {
    index_count_min = 3;
  }
  if (index_count < index_count_min) {
    return ConversionResult::kPrimitiveEmpty;
  }

  // Invalidate the cache if data behind any entry was modified.
  if (memory_regions_invalidated_.exchange(0ull, std::memory_order_acquire) &
      memory_regions_used_) {
    converted_indices_cache_.clear();
    memory_regions_used_ = 0;
  }

  address &= index_32bit ? 0x1FFFFFFC : 0x1FFFFFFE;
  uint32_t index_size = index_32bit ? sizeof(uint32_t) : sizeof(uint16_t);
  uint32_t index_buffer_size = index_size * index_count;
  uint32_t address_last = address + index_size * (index_count - 1);

  // Create the cache entry, currently only for the key.
  ConvertedIndices converted_indices;
  converted_indices.key.address = address;
  converted_indices.key.source_type = source_type;
  converted_indices.key.format = index_format;
  converted_indices.key.count = index_count;
  converted_indices.key.reset = reset ? 1 : 0;
  converted_indices.reset_index = reset_index;

  // Try to find the previously converted index buffer.
  auto found_range =
      converted_indices_cache_.equal_range(converted_indices.key.value);
  for (auto iter = found_range.first; iter != found_range.second; ++iter) {
    const ConvertedIndices& found_converted = iter->second;
    if (reset && found_converted.reset_index != reset_index) {
      continue;
    }
    if (found_converted.converted_index_count == 0) {
      return ConversionResult::kPrimitiveEmpty;
    }
    if (!found_converted.gpu_address) {
      return ConversionResult::kConversionNotNeeded;
    }
    gpu_address_out = found_converted.gpu_address;
    index_count_out = found_converted.converted_index_count;
    return ConversionResult::kConverted;
  }

  // Get the memory usage mask for cache invalidation.
  // 1 bit = (512 / 64) MB = 8 MB.
  uint64_t memory_regions_used_bits = ~((1ull << (address >> 23)) - 1);
  if (address_last < (63 << 23)) {
    memory_regions_used_bits = (1ull << ((address_last >> 23) + 1)) - 1;
  }

  union {
    const void* source;
    const uint8_t* source_8;
    const uint16_t* source_16;
    const uint32_t* source_32;
    uintptr_t source_uintptr;
  };
  source = memory_.TranslatePhysical(address);

  // Calculate the new index count, and also check if there's nothing to convert
  // in the buffer (for instance, if not using actually primitive reset).
  uint32_t converted_index_count = 0;
  bool conversion_needed = false;
  bool simd = false;
  // Optimization specific to primitive types - if reset index not found in the
  // source index buffer, can set this to false and use a faster way of copying.
  bool reset_actually_used = reset;
  if (source_type == xenos::PrimitiveType::kTriangleFan) {
    // Triangle fans are not supported by Direct3D 12 at all.
    conversion_needed = true;
    trace_writer_.WriteMemoryRead(address, index_buffer_size);
    if (reset) {
      uint32_t current_fan_index_count = 0;
      for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t index = index_format == xenos::IndexFormat::kInt32
                             ? source_32[i]
                             : source_16[i];
        if (index == reset_index) {
          current_fan_index_count = 0;
          continue;
        }
        if (++current_fan_index_count >= 3) {
          converted_index_count += 3;
        }
      }
    } else {
      converted_index_count = 3 * (index_count - 2);
    }
  } else if (source_type == xenos::PrimitiveType::kTriangleStrip ||
             source_type == xenos::PrimitiveType::kLineStrip) {
    converted_index_count = index_count;
    // Check if the restart index is used at all in this buffer because reading
    // vertices from a default heap is faster than from an upload heap.
    conversion_needed = false;
    trace_writer_.WriteMemoryRead(address, index_buffer_size);
#if XE_ARCH_AMD64
    // Will use SIMD to copy 16-byte blocks using _mm_or_si128.
    simd = true;
    union {
      const void* check_source;
      const uint32_t* check_source_16;
      const uint32_t* check_source_32;
      const __m128i* check_source_128;
      uintptr_t check_source_uintptr;
    };
    check_source = source;
    uint32_t check_indices_remaining = index_count;
    alignas(16) uint64_t check_result[2];
    if (index_format == xenos::IndexFormat::kInt32) {
      while (check_indices_remaining != 0 && (check_source_uintptr & 15)) {
        --check_indices_remaining;
        if (*(check_source_32++) == reset_index) {
          conversion_needed = true;
          check_indices_remaining = 0;
        }
      }
      __m128i check_reset_index_vector = _mm_set1_epi32(reset_index);
      while (check_indices_remaining >= 4) {
        check_indices_remaining -= 4;
        _mm_store_si128(reinterpret_cast<__m128i*>(&check_result),
                        _mm_cmpeq_epi32(_mm_load_si128(check_source_128++),
                                        check_reset_index_vector));
        if (check_result[0] || check_result[1]) {
          conversion_needed = true;
          check_indices_remaining = 0;
        }
      }
      while (check_indices_remaining != 0) {
        --check_indices_remaining;
        if (*(check_source_32++) == reset_index) {
          conversion_needed = true;
          check_indices_remaining = 0;
        }
      }
    } else {
      while (check_indices_remaining != 0 && (check_source_uintptr & 15)) {
        --check_indices_remaining;
        if (*(check_source_16++) == reset_index) {
          conversion_needed = true;
          check_indices_remaining = 0;
        }
      }
      __m128i check_reset_index_vector = _mm_set1_epi16(reset_index);
      while (check_indices_remaining >= 8) {
        check_indices_remaining -= 8;
        _mm_store_si128(reinterpret_cast<__m128i*>(&check_result),
                        _mm_cmpeq_epi16(_mm_load_si128(check_source_128++),
                                        check_reset_index_vector));
        if (check_result[0] || check_result[1]) {
          conversion_needed = true;
          check_indices_remaining = 0;
        }
      }
      while (check_indices_remaining != 0) {
        --check_indices_remaining;
        if (*(check_source_16++) == reset_index) {
          conversion_needed = true;
          check_indices_remaining = 0;
        }
      }
    }
#else
    if (index_format == xenos::IndexFormat::kInt32) {
      for (uint32_t i = 0; i < index_count; ++i) {
        if (source_32[i] == reset_index) {
          conversion_needed = true;
          break;
        }
      }
    } else {
      for (uint32_t i = 0; i < index_count; ++i) {
        if (source_16[i] == reset_index) {
          conversion_needed = true;
          break;
        }
      }
    }
#endif  // XE_ARCH_AMD64
  } else if (source_type == xenos::PrimitiveType::kLineLoop) {
    conversion_needed = true;
    trace_writer_.WriteMemoryRead(address, index_buffer_size);
    if (reset) {
      reset_actually_used = false;
      uint32_t current_strip_index_count = 0;
      for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t index = index_format == xenos::IndexFormat::kInt32
                             ? source_32[i]
                             : source_16[i];
        if (index == reset_index) {
          reset_actually_used = true;
          // Loop strips with more than 2 vertices.
          if (current_strip_index_count > 2) {
            ++converted_index_count;
          }
          current_strip_index_count = 0;
          continue;
        }
        // Start a new strip if 2 vertices, add one vertex if more.
        if (++current_strip_index_count >= 2) {
          converted_index_count += current_strip_index_count == 2 ? 2 : 1;
        }
      }
    } else {
      converted_index_count = index_count + 1;
    }
  } else if (source_type == xenos::PrimitiveType::kQuadList) {
    conversion_needed = true;
    trace_writer_.WriteMemoryRead(address, index_buffer_size);
    converted_index_count = (index_count >> 2) * 6;
  }
  converted_indices.converted_index_count = converted_index_count;

  // If nothing to convert, store this result so the check won't be happening
  // again and again and exit.
  if (!conversion_needed || converted_index_count == 0) {
    converted_indices.gpu_address = 0;
    converted_indices_cache_.emplace(converted_indices.key.value,
                                     converted_indices);
    memory_regions_used_ |= memory_regions_used_bits;
    return converted_index_count == 0 ? ConversionResult::kPrimitiveEmpty
                                      : ConversionResult::kConversionNotNeeded;
  }

  // Convert.

  D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
  void* target = AllocateIndices(index_format, converted_index_count,
                                 simd ? address & 15 : 0, gpu_address);
  if (target == nullptr) {
    return ConversionResult::kFailed;
  }

  if (source_type == xenos::PrimitiveType::kTriangleFan) {
    // https://docs.microsoft.com/en-us/windows/desktop/direct3d9/triangle-fans
    // Ordered as (v1, v2, v0), (v2, v3, v0).
    if (reset) {
      uint32_t current_fan_index_count = 0;
      uint32_t current_fan_first_index = 0;
      if (index_format == xenos::IndexFormat::kInt32) {
        uint32_t* target_32 = reinterpret_cast<uint32_t*>(target);
        for (uint32_t i = 0; i < index_count; ++i) {
          uint32_t index = source_32[i];
          if (index == reset_index) {
            current_fan_index_count = 0;
            continue;
          }
          if (current_fan_index_count == 0) {
            current_fan_first_index = index;
          }
          if (++current_fan_index_count >= 3) {
            *(target_32++) = source_32[i - 1];
            *(target_32++) = index;
            *(target_32++) = current_fan_first_index;
          }
        }
      } else {
        uint16_t* target_16 = reinterpret_cast<uint16_t*>(target);
        for (uint32_t i = 0; i < index_count; ++i) {
          uint16_t index = source_16[i];
          if (index == reset_index) {
            current_fan_index_count = 0;
            continue;
          }
          if (current_fan_index_count == 0) {
            current_fan_first_index = index;
          }
          if (++current_fan_index_count >= 3) {
            *(target_16++) = source_16[i - 1];
            *(target_16++) = index;
            *(target_16++) = uint16_t(current_fan_first_index);
          }
        }
      }
    } else {
      if (index_format == xenos::IndexFormat::kInt32) {
        uint32_t* target_32 = reinterpret_cast<uint32_t*>(target);
        for (uint32_t i = 2; i < index_count; ++i) {
          *(target_32++) = source_32[i - 1];
          *(target_32++) = source_32[i];
          *(target_32++) = source_32[0];
        }
      } else {
        uint16_t* target_16 = reinterpret_cast<uint16_t*>(target);
        for (uint32_t i = 2; i < index_count; ++i) {
          *(target_16++) = source_16[i - 1];
          *(target_16++) = source_16[i];
          *(target_16++) = source_16[0];
        }
      }
    }
  } else if (source_type == xenos::PrimitiveType::kTriangleStrip ||
             source_type == xenos::PrimitiveType::kLineStrip) {
#if XE_ARCH_AMD64
    // Replace the reset index with the maximum representable value - vector OR
    // gives 0 or 0xFFFF/0xFFFFFFFF, which is exactly what is needed.
    // Allocations in the target index buffer are aligned with 16-byte
    // granularity, and within 16-byte vectors, both the source and the target
    // start at the same offset.
    union {
      const __m128i* source_aligned_128;
      uintptr_t source_aligned_uintptr;
    };
    source_aligned_uintptr = source_uintptr & ~(uintptr_t(15));
    union {
      __m128i* target_aligned_128;
      uintptr_t target_aligned_uintptr;
    };
    target_aligned_uintptr =
        reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(15));
    uint32_t vector_count = (address_last >> 4) - (address >> 4) + 1;
    if (index_format == xenos::IndexFormat::kInt32) {
      __m128i reset_index_vector = _mm_set1_epi32(reset_index);
      for (uint32_t i = 0; i < vector_count; ++i) {
        __m128i indices_vector = _mm_load_si128(source_aligned_128++);
        __m128i indices_are_reset_vector =
            _mm_cmpeq_epi32(indices_vector, reset_index_vector);
        _mm_store_si128(target_aligned_128++,
                        _mm_or_si128(indices_vector, indices_are_reset_vector));
      }
    } else {
      __m128i reset_index_vector = _mm_set1_epi16(reset_index);
      for (uint32_t i = 0; i < vector_count; ++i) {
        __m128i indices_vector = _mm_load_si128(source_aligned_128++);
        __m128i indices_are_reset_vector =
            _mm_cmpeq_epi16(indices_vector, reset_index_vector);
        _mm_store_si128(target_aligned_128++,
                        _mm_or_si128(indices_vector, indices_are_reset_vector));
      }
    }
#else
    if (index_format == xenos::IndexFormat::kInt32) {
      for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t index = source_32[i];
        reinterpret_cast<uint32_t*>(target)[i] =
            index == reset_index ? 0xFFFFFFFFu : index;
      }
    } else {
      for (uint32_t i = 0; i < index_count; ++i) {
        uint16_t index = source_16[i];
        reinterpret_cast<uint16_t*>(target)[i] =
            index == reset_index ? 0xFFFFu : index;
      }
    }
#endif  // XE_ARCH_AMD64
  } else if (source_type == xenos::PrimitiveType::kLineLoop) {
    if (reset_actually_used) {
      uint32_t current_strip_index_count = 0;
      uint32_t current_strip_first_index = 0;
      if (index_format == xenos::IndexFormat::kInt32) {
        uint32_t* target_32 = reinterpret_cast<uint32_t*>(target);
        for (uint32_t i = 0; i < index_count; ++i) {
          uint32_t index = source_32[i];
          if (index == reset_index) {
            if (current_strip_index_count > 2) {
              *(target_32++) = current_strip_first_index;
            }
            current_strip_index_count = 0;
            continue;
          }
          if (current_strip_index_count == 0) {
            current_strip_first_index = index;
          }
          ++current_strip_index_count;
          if (current_strip_index_count >= 2) {
            if (current_strip_index_count == 2) {
              *(target_32++) = current_strip_first_index;
            }
            *(target_32++) = index;
          }
        }
      } else {
        uint16_t* target_16 = reinterpret_cast<uint16_t*>(target);
        for (uint32_t i = 0; i < index_count; ++i) {
          uint16_t index = source_16[i];
          if (index == reset_index) {
            if (current_strip_index_count > 2) {
              *(target_16++) = uint16_t(current_strip_first_index);
            }
            current_strip_index_count = 0;
            continue;
          }
          if (current_strip_index_count == 0) {
            current_strip_first_index = index;
          }
          ++current_strip_index_count;
          if (current_strip_index_count >= 2) {
            if (current_strip_index_count == 2) {
              *(target_16++) = uint16_t(current_strip_first_index);
            }
            *(target_16++) = index;
          }
        }
      }
    } else {
      std::memcpy(target, source, index_count * index_size);
      if (converted_index_count > index_count) {
        if (index_format == xenos::IndexFormat::kInt32) {
          reinterpret_cast<uint32_t*>(target)[index_count] = source_32[0];
        } else {
          reinterpret_cast<uint16_t*>(target)[index_count] = source_16[0];
        }
      }
    }
  } else if (source_type == xenos::PrimitiveType::kQuadList) {
    uint32_t quad_count = index_count >> 4;
    if (index_format == xenos::IndexFormat::kInt32) {
      uint32_t* target_32 = reinterpret_cast<uint32_t*>(target);
      for (uint32_t i = 0; i < quad_count; ++i) {
        uint32_t quad_index = i << 2;
        *(target_32++) = source_32[quad_index];
        *(target_32++) = source_32[quad_index + 1];
        *(target_32++) = source_32[quad_index + 2];
        *(target_32++) = source_32[quad_index];
        *(target_32++) = source_32[quad_index + 2];
        *(target_32++) = source_32[quad_index + 3];
      }
    } else {
      uint16_t* target_16 = reinterpret_cast<uint16_t*>(target);
      for (uint32_t i = 0; i < quad_count; ++i) {
        uint32_t quad_index = i << 2;
        *(target_16++) = source_16[quad_index];
        *(target_16++) = source_16[quad_index + 1];
        *(target_16++) = source_16[quad_index + 2];
        *(target_16++) = source_16[quad_index];
        *(target_16++) = source_16[quad_index + 2];
        *(target_16++) = source_16[quad_index + 3];
      }
    }
  }

  // Cache and return the indices.
  converted_indices.gpu_address = gpu_address;
  converted_indices_cache_.emplace(converted_indices.key.value,
                                   converted_indices);
  memory_regions_used_ |= memory_regions_used_bits;
  gpu_address_out = gpu_address;
  index_count_out = converted_index_count;
  return ConversionResult::kConverted;
}

void* PrimitiveConverter::AllocateIndices(
    xenos::IndexFormat format, uint32_t count, uint32_t simd_offset,
    D3D12_GPU_VIRTUAL_ADDRESS& gpu_address_out) {
  if (count == 0) {
    return nullptr;
  }
  uint32_t size =
      count * (format == xenos::IndexFormat::kInt32 ? sizeof(uint32_t)
                                                    : sizeof(uint16_t));
  // 16-align all index data because SIMD is used to replace the reset index
  // (without that, 4-alignment would be required anyway to mix 16-bit and
  // 32-bit indices in one buffer page).
  size = xe::align(size, uint32_t(16));
  // Add some space to align SIMD register components the same way in the source
  // and the buffer.
  simd_offset &= 15;
  if (simd_offset != 0) {
    size += 16;
  }
  D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
  uint8_t* mapping =
      buffer_pool_->Request(command_processor_.GetCurrentFrame(), size, 16,
                            nullptr, nullptr, &gpu_address);
  if (mapping == nullptr) {
    XELOGE("Failed to allocate space for {} converted {}-bit vertex indices",
           count, format == xenos::IndexFormat::kInt32 ? 32 : 16);
    return nullptr;
  }
  gpu_address_out = gpu_address + simd_offset;
  return mapping + simd_offset;
}

std::pair<uint32_t, uint32_t> PrimitiveConverter::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  // 1 bit = (512 / 64) MB = 8 MB. Invalidate a region of this size.
  uint32_t bit_index_first = physical_address_start >> 23;
  uint32_t bit_index_last = (physical_address_start + length - 1) >> 23;
  uint64_t bits = ~((1ull << bit_index_first) - 1);
  if (bit_index_last < 63) {
    bits &= (1ull << (bit_index_last + 1)) - 1;
  }
  memory_regions_invalidated_ |= bits;
  return std::make_pair<uint32_t, uint32_t>(0, UINT32_MAX);
}

std::pair<uint32_t, uint32_t>
PrimitiveConverter::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length,
    bool exact_range) {
  return reinterpret_cast<PrimitiveConverter*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

D3D12_GPU_VIRTUAL_ADDRESS PrimitiveConverter::GetStaticIndexBuffer(
    xenos::PrimitiveType source_type, uint32_t index_count,
    uint32_t& index_count_out) const {
  if (index_count > kMaxNonIndexedVertices) {
    assert_always();
    return D3D12_GPU_VIRTUAL_ADDRESS(0);
  }
  if (source_type == xenos::PrimitiveType::kTriangleFan) {
    index_count_out = (std::max(index_count, uint32_t(2)) - 2) * 3;
    return static_ib_gpu_address_ +
           kStaticIBTriangleFanOffset * sizeof(uint16_t);
  }
  if (source_type == xenos::PrimitiveType::kQuadList &&
      cvars::d3d12_convert_quads_to_triangles) {
    index_count_out = (index_count >> 2) * 6;
    return static_ib_gpu_address_ + kStaticIBQuadOffset * sizeof(uint16_t);
  }
  return D3D12_GPU_VIRTUAL_ADDRESS(0);
}

void PrimitiveConverter::InitializeTrace() {
  // WriteMemoryRead must not be skipped.
  converted_indices_cache_.clear();
  memory_regions_used_ = 0;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
