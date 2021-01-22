/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstring>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/gpu/d3d12/d3d12_shader.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DEFINE_bool(d3d12_bindless, true,
            "Use bindless resources where available - may improve performance, "
            "but may make debugging more complicated.",
            "D3D12");
DEFINE_bool(d3d12_edram_rov, true,
            "Use rasterizer-ordered views for render target emulation where "
            "available.",
            "D3D12");
DEFINE_bool(d3d12_readback_memexport, false,
            "Read data written by memory export in shaders on the CPU. This "
            "may be needed in some games (but many only access exported data "
            "on the GPU, and this flag isn't needed to handle such behavior), "
            "but causes mid-frame synchronization, so it has a huge "
            "performance impact.",
            "D3D12");
DEFINE_bool(d3d12_readback_resolve, false,
            "Read render-to-texture results on the CPU. This may be needed in "
            "some games, for instance, for screenshots in saved games, but "
            "causes mid-frame synchronization, so it has a huge performance "
            "impact.",
            "D3D12");
DEFINE_bool(d3d12_ssaa_custom_sample_positions, false,
            "Enable custom SSAA sample positions for the RTV/DSV rendering "
            "path where available instead of centers (experimental, not very "
            "high-quality).",
            "D3D12");
DEFINE_bool(d3d12_submit_on_primary_buffer_end, true,
            "Submit the command list when a PM4 primary buffer ends if it's "
            "possible to submit immediately to try to reduce frame latency.",
            "D3D12");

namespace xe {
namespace gpu {
namespace d3d12 {

D3D12CommandProcessor::D3D12CommandProcessor(
    D3D12GraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      deferred_command_list_(*this) {}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

void D3D12CommandProcessor::RequestFrameTrace(
    const std::filesystem::path& root_path) {
  // Capture with PIX if attached.
  if (GetD3D12Context().GetD3D12Provider().GetGraphicsAnalysis() != nullptr) {
    pix_capture_requested_.store(true, std::memory_order_relaxed);
    return;
  }
  CommandProcessor::RequestFrameTrace(root_path);
}

void D3D12CommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_converter_->MemoryInvalidationCallback(base_ptr, length, true);
}

void D3D12CommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Starting a new frame because descriptors may be needed.
  BeginSubmission(true);
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

uint32_t D3D12CommandProcessor::GetCurrentColorMask(
    uint32_t shader_writes_color_targets) const {
  auto& regs = *register_file_;
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode !=
      xenos::ModeControl::kColorDepth) {
    return 0;
  }
  uint32_t color_mask = regs[XE_GPU_REG_RB_COLOR_MASK].u32 & 0xFFFF;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(shader_writes_color_targets & (1 << i))) {
      color_mask &= ~(0xF << (i * 4));
    }
  }
  return color_mask;
}

void D3D12CommandProcessor::PushTransitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES old_state,
    D3D12_RESOURCE_STATES new_state, UINT subresource) {
  if (old_state == new_state) {
    return;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_
                       : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN
                  : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader
          ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size())
          : 0;
  uint32_t sampler_count_pixel =
      pixel_shader
          ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size())
          : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN)
           << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges =
        shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType =
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType =
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (edram_rov_used_) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType =
          D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature = ui::d3d12::util::CreateRootSignature(
      GetD3D12Context().GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    XELOGE(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex,
        sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader &&
      !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader &&
      !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex =
        RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex =
        RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update,
    uint32_t count_for_full_update, D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update,
      count_for_full_update, descriptor_index);
  if (current_heap_index ==
      ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  auto& provider = GetD3D12Context().GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(
    uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCPUGPUHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  auto& provider = GetD3D12Context().GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count > kViewBindlessHeapSize - view_bindless_heap_allocated_ +
                    view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] =
          std::make_pair(provider.OffsetViewDescriptor(
                             view_bindless_heap_cpu_start_, descriptor_index),
                         provider.OffsetViewDescriptor(
                             view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(
            ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count, count,
            cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] =
          std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                         provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCPUGPUHandlePair
D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  auto& provider = GetD3D12Context().GetD3D12Provider();
  return std::make_pair(provider.OffsetViewDescriptor(
                            view_bindless_heap_cpu_start_, uint32_t(view)),
                        provider.OffsetViewDescriptor(
                            view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCPUGPUHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCPUGPUHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCPUGPUHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update,
    uint32_t count_for_full_update, D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update,
      count_for_full_update, descriptor_index);
  if (current_heap_index ==
      ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  auto& provider = GetD3D12Context().GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(),
      descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(),
      descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(
    uint32_t size, D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = xe::align(size, kScratchBufferSizeIncrement);

  auto& provider = GetD3D12Context().GetD3D12Provider();
  auto device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault,
          provider.GetHeapFlagCreateNotZeroed(), &buffer_desc, state, nullptr,
          IID_PPV_ARGS(&buffer)))) {
    XELOGE("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    buffers_for_deletion_.push_back(
        std::make_pair(scratch_buffer_, submission_current_));
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(
    ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetSamplePositions(
    xenos::MsaaSamples sample_positions) {
  if (current_sample_positions_ == sample_positions) {
    return;
  }
  // Evaluating attributes by sample index - which is done for per-sample
  // depth - is undefined with programmable sample positions, so can't use them
  // for ROV output. There's hardly any difference between 2,6 (of 0 and 3 with
  // 4x MSAA) and 4,4 anyway.
  // https://docs.microsoft.com/en-us/windows/desktop/api/d3d12/nf-d3d12-id3d12graphicscommandlist1-setsamplepositions
  if (cvars::d3d12_ssaa_custom_sample_positions && !edram_rov_used_ &&
      command_list_1_) {
    auto& provider = GetD3D12Context().GetD3D12Provider();
    auto tier = provider.GetProgrammableSamplePositionsTier();
    if (tier >= D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_2) {
      // Depth buffer transitions are affected by sample positions.
      SubmitBarriers();
      // Standard sample positions in Direct3D 10.1, but adjusted to take the
      // fact that SSAA samples are already shifted by 1/4 of a pixel.
      // TODO(Triang3l): Find what sample positions are used by Xenos, though
      // they are not necessarily better. The purpose is just to make 2x SSAA
      // work a little bit better for tall stairs.
      // FIXME(Triang3l): This is currently even uglier than without custom
      // sample positions.
      if (sample_positions >= xenos::MsaaSamples::k2X) {
        // Sample 1 is lower-left on Xenos, but upper-right in Direct3D 12.
        D3D12_SAMPLE_POSITION d3d_sample_positions[4];
        if (sample_positions >= xenos::MsaaSamples::k4X) {
          // Upper-left.
          d3d_sample_positions[0].X = -2 + 4;
          d3d_sample_positions[0].Y = -6 + 4;
          // Upper-right.
          d3d_sample_positions[1].X = 6 - 4;
          d3d_sample_positions[1].Y = -2 + 4;
          // Lower-left.
          d3d_sample_positions[2].X = -6 + 4;
          d3d_sample_positions[2].Y = 2 - 4;
          // Lower-right.
          d3d_sample_positions[3].X = 2 - 4;
          d3d_sample_positions[3].Y = 6 - 4;
        } else {
          // Upper.
          d3d_sample_positions[0].X = -4;
          d3d_sample_positions[0].Y = -4 + 4;
          d3d_sample_positions[1].X = -4;
          d3d_sample_positions[1].Y = -4 + 4;
          // Lower.
          d3d_sample_positions[2].X = 4;
          d3d_sample_positions[2].Y = 4 - 4;
          d3d_sample_positions[3].X = 4;
          d3d_sample_positions[3].Y = 4 - 4;
        }
        deferred_command_list_.D3DSetSamplePositions(1, 4,
                                                     d3d_sample_positions);
      } else {
        deferred_command_list_.D3DSetSamplePositions(0, 0, nullptr);
      }
    }
  }
  current_sample_positions_ = sample_positions;
}

void D3D12CommandProcessor::SetComputePipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    deferred_command_list_.D3DSetPipelineState(pipeline);
    current_external_pipeline_ = pipeline;
    current_cached_pipeline_ = nullptr;
  }
}

void D3D12CommandProcessor::NotifyShaderBindingsLayoutUIDsInvalidated() {
  if (bindless_resources_used_) {
    cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
  } else {
    bindful_textures_written_vertex_ = false;
    bindful_textures_written_pixel_ = false;
    bindful_samplers_written_vertex_ = false;
    bindful_samplers_written_pixel_ = false;
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  if (render_target_cache_) {
    if (!edram_rov_used_) {
      return "Direct3D 12 - no ROV, inaccurate";
    }
    // Currently scaling is only supported with ROV.
    if (texture_cache_ != nullptr && texture_cache_->IsResolutionScale2X()) {
      return "Direct3D 12 - ROV 2x";
    }
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    return "Direct3D 12 - ROV";
  }
  return "Direct3D 12";
}

std::unique_ptr<xe::ui::RawImage> D3D12CommandProcessor::Capture() {
  ID3D12Resource* readback_buffer =
      RequestReadbackBuffer(uint32_t(swap_texture_copy_size_));
  if (!readback_buffer) {
    return nullptr;
  }
  BeginSubmission(false);
  PushTransitionBarrier(swap_texture_,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
  SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION location_source, location_dest;
  location_source.pResource = swap_texture_;
  location_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  location_source.SubresourceIndex = 0;
  location_dest.pResource = readback_buffer;
  location_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  location_dest.PlacedFootprint = swap_texture_copy_footprint_;
  deferred_command_list_.CopyTexture(location_dest, location_source);
  PushTransitionBarrier(swap_texture_, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (!AwaitAllQueueOperationsCompletion()) {
    return nullptr;
  }
  D3D12_RANGE readback_range;
  readback_range.Begin = swap_texture_copy_footprint_.Offset;
  readback_range.End = swap_texture_copy_size_;
  void* readback_mapping;
  if (FAILED(readback_buffer->Map(0, &readback_range, &readback_mapping))) {
    return nullptr;
  }
  std::unique_ptr<xe::ui::RawImage> raw_image(new xe::ui::RawImage());
  auto swap_texture_size = GetSwapTextureSize();
  raw_image->width = swap_texture_size.first;
  raw_image->height = swap_texture_size.second;
  raw_image->stride = swap_texture_size.first * 4;
  raw_image->data.resize(raw_image->stride * swap_texture_size.second);
  const uint8_t* readback_source_data =
      reinterpret_cast<const uint8_t*>(readback_mapping) +
      swap_texture_copy_footprint_.Offset;
  static_assert(
      ui::d3d12::D3D12Context::kSwapChainFormat == DXGI_FORMAT_B8G8R8A8_UNORM,
      "D3D12CommandProcessor::Capture assumes swap_texture_ to be in "
      "DXGI_FORMAT_B8G8R8A8_UNORM because it swaps red and blue");
  for (uint32_t i = 0; i < swap_texture_size.second; ++i) {
    uint8_t* pixel_dest = raw_image->data.data() + i * raw_image->stride;
    const uint8_t* pixel_source =
        readback_source_data +
        i * swap_texture_copy_footprint_.Footprint.RowPitch;
    for (uint32_t j = 0; j < swap_texture_size.first; ++j) {
      pixel_dest[0] = pixel_source[2];
      pixel_dest[1] = pixel_source[1];
      pixel_dest[2] = pixel_source[0];
      pixel_dest[3] = pixel_source[3];
      pixel_dest += 4;
      pixel_source += 4;
    }
  }
  return raw_image;
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  auto& provider = GetD3D12Context().GetD3D12Provider();
  auto device = provider.GetDevice();
  auto direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, false, false, nullptr);
  if (fence_completion_event_ == nullptr) {
    XELOGE("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&submission_fence_)))) {
    XELOGE("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(
          0, D3D12_FENCE_FLAG_NONE,
          IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    XELOGE(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)))) {
    XELOGE("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       command_allocator, nullptr,
                                       IID_PPV_ARGS(&command_list_)))) {
    XELOGE("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));

  bindless_resources_used_ =
      cvars::d3d12_bindless &&
      provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;
  edram_rov_used_ =
      cvars::d3d12_edram_rov && provider.AreRasterizerOrderedViewsSupported();

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider,
      std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
               sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(
            &view_bindless_heap_desc, IID_PPV_ARGS(&view_bindless_heap_)))) {
      XELOGE("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ =
        view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ =
        view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags =
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(
            &sampler_bindless_heap_desc,
            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      XELOGE("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ =
        std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
            device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ =
        std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
            device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter =
          root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless
          [kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter =
          root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless
          [kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless
          [kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter =
          root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter =
          root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter =
          root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges =
          &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType =
          D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[6];
    {
      auto& parameter =
          root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Shared memory SRV.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    xe::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable
                                                    .NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister =
            UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart =
            UINT(SystemBindlessView::kSharedMemoryRawSRV);
      }
      // Shared memory UAV.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    xe::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable
                                                    .NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister =
            UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart =
            UINT(SystemBindlessView::kSharedMemoryRawUAV);
      }
      // EDRAM.
      if (edram_rov_used_) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    xe::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable
                                                    .NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister =
            UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart =
            UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    xe::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable
                                                    .NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace =
            UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart =
            UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    xe::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable
                                                    .NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace =
            UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart =
            UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    xe::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable
                                                    .NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace =
            UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart =
            UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ = ui::d3d12::util::CreateRootSignature(
        provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      XELOGE(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex]
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex]
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ = ui::d3d12::util::CreateRootSignature(
        provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      XELOGE(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }
  }

  shared_memory_ =
      std::make_unique<D3D12SharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  texture_cache_ = std::make_unique<TextureCache>(
      *this, *register_file_, bindless_resources_used_, *shared_memory_);
  if (!texture_cache_->Initialize(edram_rov_used_)) {
    XELOGE("Failed to initialize the texture cache");
    return false;
  }

  render_target_cache_ = std::make_unique<RenderTargetCache>(
      *this, *register_file_, trace_writer_, bindless_resources_used_,
      edram_rov_used_);
  if (!render_target_cache_->Initialize(*texture_cache_)) {
    XELOGE("Failed to initialize the render target cache");
    return false;
  }

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, bindless_resources_used_, edram_rov_used_,
      render_target_cache_->depth_float24_conversion(),
      texture_cache_->IsResolutionScale2X() ? 2 : 1);
  if (!pipeline_cache_->Initialize()) {
    XELOGE("Failed to initialize the graphics pipeline cache");
    return false;
  }

  primitive_converter_ = std::make_unique<PrimitiveConverter>(
      *this, *register_file_, *memory_, trace_writer_);
  if (!primitive_converter_->Initialize()) {
    XELOGE("Failed to initialize the geometric primitive converter");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed =
      provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources. The PWL gamma ramp is 16-bit, but 6 bits are
  // hardwired to zero, so DXGI_FORMAT_R10G10B10A2_UNORM can be used for it too.
  // https://www.x.org/docs/AMD/old/42590_m76_rrg_1.01o.pdf
  dirty_gamma_ramp_normal_ = true;
  dirty_gamma_ramp_pwl_ = true;
  D3D12_RESOURCE_DESC gamma_ramp_desc;
  gamma_ramp_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
  gamma_ramp_desc.Alignment = 0;
  gamma_ramp_desc.Width = 256;
  gamma_ramp_desc.Height = 1;
  gamma_ramp_desc.DepthOrArraySize = 1;
  // Normal gamma is 256x1, PWL gamma is 128x1.
  gamma_ramp_desc.MipLevels = 2;
  gamma_ramp_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  gamma_ramp_desc.SampleDesc.Count = 1;
  gamma_ramp_desc.SampleDesc.Quality = 0;
  gamma_ramp_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  gamma_ramp_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  // The first action will be uploading.
  gamma_ramp_texture_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, heap_flag_create_not_zeroed,
          &gamma_ramp_desc, gamma_ramp_texture_state_, nullptr,
          IID_PPV_ARGS(&gamma_ramp_texture_)))) {
    XELOGE("Failed to create the gamma ramp texture");
    return false;
  }
  // Get the layout for the upload buffer.
  gamma_ramp_desc.DepthOrArraySize = kQueueFrames;
  UINT64 gamma_ramp_upload_size;
  device->GetCopyableFootprints(&gamma_ramp_desc, 0, kQueueFrames * 2, 0,
                                gamma_ramp_footprints_, nullptr, nullptr,
                                &gamma_ramp_upload_size);
  // Create the upload buffer for the gamma ramp.
  ui::d3d12::util::FillBufferResourceDesc(
      gamma_ramp_desc, gamma_ramp_upload_size, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, heap_flag_create_not_zeroed,
          &gamma_ramp_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&gamma_ramp_upload_)))) {
    XELOGE("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_mapping_)))) {
    XELOGE("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_mapping_ = nullptr;
    return false;
  }

  D3D12_RESOURCE_DESC swap_texture_desc;
  swap_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  swap_texture_desc.Alignment = 0;
  auto swap_texture_size = GetSwapTextureSize();
  swap_texture_desc.Width = swap_texture_size.first;
  swap_texture_desc.Height = swap_texture_size.second;
  swap_texture_desc.DepthOrArraySize = 1;
  swap_texture_desc.MipLevels = 1;
  swap_texture_desc.Format = ui::d3d12::D3D12Context::kSwapChainFormat;
  swap_texture_desc.SampleDesc.Count = 1;
  swap_texture_desc.SampleDesc.Quality = 0;
  swap_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  swap_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  // Can be sampled at any time, switch to render target when needed, then back.
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, heap_flag_create_not_zeroed,
          &swap_texture_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
          nullptr, IID_PPV_ARGS(&swap_texture_)))) {
    XELOGE("Failed to create the command processor front buffer");
    return false;
  }
  device->GetCopyableFootprints(&swap_texture_desc, 0, 1, 0,
                                &swap_texture_copy_footprint_, nullptr, nullptr,
                                &swap_texture_copy_size_);
  D3D12_DESCRIPTOR_HEAP_DESC swap_descriptor_heap_desc;
  swap_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  swap_descriptor_heap_desc.NumDescriptors = 1;
  swap_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  swap_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(
          &swap_descriptor_heap_desc,
          IID_PPV_ARGS(&swap_texture_rtv_descriptor_heap_)))) {
    XELOGE("Failed to create the command processor front buffer RTV heap");
    return false;
  }
  swap_texture_rtv_ =
      swap_texture_rtv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC swap_rtv_desc;
  swap_rtv_desc.Format = ui::d3d12::D3D12Context::kSwapChainFormat;
  swap_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  swap_rtv_desc.Texture2D.MipSlice = 0;
  swap_rtv_desc.Texture2D.PlaneSlice = 0;
  device->CreateRenderTargetView(swap_texture_, &swap_rtv_desc,
                                 swap_texture_rtv_);
  swap_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  swap_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(
          &swap_descriptor_heap_desc,
          IID_PPV_ARGS(&swap_texture_srv_descriptor_heap_)))) {
    XELOGE("Failed to create the command processor front buffer SRV heap");
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_srv_desc;
  swap_srv_desc.Format = ui::d3d12::D3D12Context::kSwapChainFormat;
  swap_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  swap_srv_desc.Shader4ComponentMapping =
      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  swap_srv_desc.Texture2D.MostDetailedMip = 0;
  swap_srv_desc.Texture2D.MipLevels = 1;
  swap_srv_desc.Texture2D.PlaneSlice = 0;
  swap_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      swap_texture_, &swap_srv_desc,
      swap_texture_srv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart());

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping =
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampNormalSRV.
    WriteGammaRampSRV(false,
                      provider.OffsetViewDescriptor(
                          view_bindless_heap_cpu_start_,
                          uint32_t(SystemBindlessView::kGammaRampNormalSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(true,
                      provider.OffsetViewDescriptor(
                          view_bindless_heap_cpu_start_,
                          uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  pix_capture_requested_.store(false, std::memory_order_relaxed);
  pix_capturing_ = false;

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();

  ui::d3d12::util::ReleaseAndNull(readback_buffer_);
  readback_buffer_size_ = 0;

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;

  for (auto& buffer_for_deletion : buffers_for_deletion_) {
    buffer_for_deletion.first->Release();
  }
  buffers_for_deletion_.clear();

  if (swap_texture_srv_descriptor_heap_ != nullptr) {
    {
      std::lock_guard<std::mutex> lock(swap_state_.mutex);
      swap_state_.pending = false;
      swap_state_.front_buffer_texture = 0;
    }
    // TODO(Triang3l): Ensure this is synchronized. The display context may not
    // exist at this point, so awaiting its fence doesn't always work.
    swap_texture_srv_descriptor_heap_->Release();
    swap_texture_srv_descriptor_heap_ = nullptr;
  }
  ui::d3d12::util::ReleaseAndNull(swap_texture_rtv_descriptor_heap_);
  ui::d3d12::util::ReleaseAndNull(swap_texture_);

  // Don't need the data anymore, so zero range.
  if (gamma_ramp_upload_mapping_ != nullptr) {
    D3D12_RANGE gamma_ramp_written_range;
    gamma_ramp_written_range.Begin = 0;
    gamma_ramp_written_range.End = 0;
    gamma_ramp_upload_->Unmap(0, &gamma_ramp_written_range);
    gamma_ramp_upload_mapping_ = nullptr;
  }
  ui::d3d12::util::ReleaseAndNull(gamma_ramp_upload_);
  ui::d3d12::util::ReleaseAndNull(gamma_ramp_texture_);

  primitive_converter_.reset();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  texture_cache_.reset();

  shared_memory_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed :
         sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    }
  } else if (index == XE_GPU_REG_DC_LUT_PWL_DATA) {
    UpdateGammaRampValue(GammaRampType::kPWL, value);
  } else if (index == XE_GPU_REG_DC_LUT_30_COLOR) {
    UpdateGammaRampValue(GammaRampType::kNormal, value);
  } else if (index == XE_GPU_REG_DC_LUT_RW_MODE) {
    gamma_ramp_rw_subindex_ = 0;
  }
}

void D3D12CommandProcessor::PerformSwap(uint32_t frontbuffer_ptr,
                                        uint32_t frontbuffer_width,
                                        uint32_t frontbuffer_height) {
  // FIXME(Triang3l): frontbuffer_ptr is currently unreliable, in the trace
  // player it's set to 0, but it's not needed anyway since the fetch constant
  // contains the address.

  SCOPE_profile_cpu_f("gpu");

  // In case the swap command is the only one in the frame.
  BeginSubmission(true);

  auto device = GetD3D12Context().GetD3D12Provider().GetDevice();

  // Upload the new gamma ramps, using the upload buffer for the current frame
  // (will close the frame after this anyway, so can't write multiple times per
  // frame).
  uint32_t gamma_ramp_frame = uint32_t(frame_current_ % kQueueFrames);
  if (dirty_gamma_ramp_normal_) {
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& gamma_ramp_footprint =
        gamma_ramp_footprints_[gamma_ramp_frame * 2];
    volatile uint32_t* mapping = reinterpret_cast<uint32_t*>(
        gamma_ramp_upload_mapping_ + gamma_ramp_footprint.Offset);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = gamma_ramp_.normal[i].value;
      // Swap red and blue (Project Sylpheed has settings allowing separate
      // configuration).
      mapping[i] = ((value & 1023) << 20) | (value & (1023 << 10)) |
                   ((value >> 20) & 1023);
    }
    PushTransitionBarrier(gamma_ramp_texture_, gamma_ramp_texture_state_,
                          D3D12_RESOURCE_STATE_COPY_DEST);
    gamma_ramp_texture_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    SubmitBarriers();
    D3D12_TEXTURE_COPY_LOCATION location_source, location_dest;
    location_source.pResource = gamma_ramp_upload_;
    location_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location_source.PlacedFootprint = gamma_ramp_footprint;
    location_dest.pResource = gamma_ramp_texture_;
    location_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location_dest.SubresourceIndex = 0;
    deferred_command_list_.CopyTexture(location_dest, location_source);
    dirty_gamma_ramp_normal_ = false;
  }
  if (dirty_gamma_ramp_pwl_) {
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& gamma_ramp_footprint =
        gamma_ramp_footprints_[gamma_ramp_frame * 2 + 1];
    volatile uint32_t* mapping = reinterpret_cast<uint32_t*>(
        gamma_ramp_upload_mapping_ + gamma_ramp_footprint.Offset);
    for (uint32_t i = 0; i < 128; ++i) {
      // TODO(Triang3l): Find a game to test if red and blue need to be swapped.
      mapping[i] = (gamma_ramp_.pwl[i].values[0].base >> 6) |
                   (uint32_t(gamma_ramp_.pwl[i].values[1].base >> 6) << 10) |
                   (uint32_t(gamma_ramp_.pwl[i].values[2].base >> 6) << 20);
    }
    PushTransitionBarrier(gamma_ramp_texture_, gamma_ramp_texture_state_,
                          D3D12_RESOURCE_STATE_COPY_DEST);
    gamma_ramp_texture_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    SubmitBarriers();
    D3D12_TEXTURE_COPY_LOCATION location_source, location_dest;
    location_source.pResource = gamma_ramp_upload_;
    location_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location_source.PlacedFootprint = gamma_ramp_footprint;
    location_dest.pResource = gamma_ramp_texture_;
    location_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location_dest.SubresourceIndex = 1;
    deferred_command_list_.CopyTexture(location_dest, location_source);
    dirty_gamma_ramp_pwl_ = false;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  ID3D12Resource* swap_texture_resource = texture_cache_->RequestSwapTexture(
      swap_texture_srv_desc, frontbuffer_format);
  if (swap_texture_resource) {
    render_target_cache_->FlushAndUnbindRenderTargets();

    // This is according to D3D::InitializePresentationParameters from a game
    // executable, which initializes the normal gamma ramp for 8_8_8_8 output
    // and the PWL gamma ramp for 2_10_10_10.
    bool use_pwl_gamma_ramp =
        frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
        frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

    bool descriptors_obtained;
    ui::d3d12::util::DescriptorCPUGPUHandlePair descriptor_swap_texture;
    ui::d3d12::util::DescriptorCPUGPUHandlePair descriptor_gamma_ramp;
    if (bindless_resources_used_) {
      descriptors_obtained =
          RequestOneUseSingleViewDescriptors(1, &descriptor_swap_texture);
      descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
          use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                             : SystemBindlessView::kGammaRampNormalSRV);
    } else {
      ui::d3d12::util::DescriptorCPUGPUHandlePair descriptors[2];
      descriptors_obtained = RequestOneUseSingleViewDescriptors(2, descriptors);
      if (descriptors_obtained) {
        descriptor_swap_texture = descriptors[0];
        descriptor_gamma_ramp = descriptors[1];
        WriteGammaRampSRV(use_pwl_gamma_ramp, descriptor_gamma_ramp.first);
      }
    }
    if (descriptors_obtained) {
      // Must not call anything that can change the descriptor heap from now on!

      // Create the swap texture descriptor.
      device->CreateShaderResourceView(swap_texture_resource,
                                       &swap_texture_srv_desc,
                                       descriptor_swap_texture.first);

      // The swap texture is kept as an SRV because the graphics system may draw
      // with it at any time. It's switched to RTV and back when needed.
      PushTransitionBarrier(swap_texture_,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
      PushTransitionBarrier(gamma_ramp_texture_, gamma_ramp_texture_state_,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      gamma_ramp_texture_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      SubmitBarriers();

      auto swap_texture_size = GetSwapTextureSize();

      // Draw the stretching rectangle.
      deferred_command_list_.D3DOMSetRenderTargets(1, &swap_texture_rtv_, TRUE,
                                                   nullptr);
      D3D12_VIEWPORT viewport;
      viewport.TopLeftX = 0.0f;
      viewport.TopLeftY = 0.0f;
      viewport.Width = float(swap_texture_size.first);
      viewport.Height = float(swap_texture_size.second);
      viewport.MinDepth = 0.0f;
      viewport.MaxDepth = 0.0f;
      deferred_command_list_.RSSetViewport(viewport);
      D3D12_RECT scissor;
      scissor.left = 0;
      scissor.top = 0;
      scissor.right = swap_texture_size.first;
      scissor.bottom = swap_texture_size.second;
      deferred_command_list_.RSSetScissorRect(scissor);
      D3D12GraphicsSystem* graphics_system =
          static_cast<D3D12GraphicsSystem*>(graphics_system_);
      graphics_system->StretchTextureToFrontBuffer(
          descriptor_swap_texture.second, &descriptor_gamma_ramp.second,
          use_pwl_gamma_ramp ? (1.0f / 128.0f) : (1.0f / 256.0f),
          deferred_command_list_);
      // Ending the current frame anyway, so no need to reset the current render
      // targets when using ROV.

      PushTransitionBarrier(swap_texture_, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      // Don't care about graphics state because the frame is ending anyway.
      {
        std::lock_guard<std::mutex> lock(swap_state_.mutex);
        swap_state_.width = swap_texture_size.first;
        swap_state_.height = swap_texture_size.second;
        swap_state_.front_buffer_texture =
            reinterpret_cast<uintptr_t>(swap_texture_srv_descriptor_heap_);
      }
    }
  }

  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (cvars::d3d12_submit_on_primary_buffer_end && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          uint32_t guest_address,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  auto device = GetD3D12Context().GetD3D12Provider().GetDevice();
  auto& regs = *register_file_;

#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  xenos::ModeControl edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::ModeControl::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw.
    // TODO(Triang3l): Do something so memexport still works in this case maybe?
    // Unlikely that zero would even really be legal though.
    return true;
  }

  // Vertex shader.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex =
      !vertex_shader->memexport_stream_constants().empty();
  DxbcShaderTranslator::Modification vertex_shader_modification;
  pipeline_cache_->GetCurrentShaderModification(*vertex_shader,
                                                vertex_shader_modification);
  bool tessellated = vertex_shader_modification.host_vertex_shader_type !=
                     Shader::HostVertexShaderType::kVertex;
  bool primitive_polygonal =
      xenos::IsPrimitivePolygonal(tessellated, primitive_type);

  // Pixel shader.
  D3D12Shader* pixel_shader = nullptr;
  if (draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    // See xenos::ModeControl for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::ModeControl::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel;
  DxbcShaderTranslator::Modification pixel_shader_modification;
  if (pixel_shader) {
    memexport_used_pixel = !pixel_shader->memexport_stream_constants().empty();
    if (!pipeline_cache_->GetCurrentShaderModification(
            *pixel_shader, pixel_shader_modification)) {
      return false;
    }
  } else {
    memexport_used_pixel = false;
    pixel_shader_modification = DxbcShaderTranslator::Modification(0);
  }

  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  BeginSubmission(true);

  // Set up the render targets - this may bind pipelines.
  uint32_t pixel_shader_writes_color_targets =
      pixel_shader ? pixel_shader->writes_color_targets() : 0;
  if (!render_target_cache_->UpdateRenderTargets(
          pixel_shader_writes_color_targets)) {
    return false;
  }
  const RenderTargetCache::PipelineRenderTarget* pipeline_render_targets =
      render_target_cache_->GetCurrentPipelineRenderTargets();

  // Set up primitive topology.
  bool indexed = index_buffer_info != nullptr && index_buffer_info->guest_base;
  xenos::PrimitiveType primitive_type_converted;
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (tessellated) {
    primitive_type_converted = primitive_type;
    switch (primitive_type_converted) {
      // TODO(Triang3l): Support all kinds of patches if found in games.
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      default:
        return false;
    }
  } else {
    primitive_type_converted =
        PrimitiveConverter::GetReplacementPrimitiveType(primitive_type);
    switch (primitive_type_converted) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        return false;
    }
  }
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
  uint32_t line_loop_closing_index;
  if (primitive_type == xenos::PrimitiveType::kLineLoop && !indexed &&
      index_count >= 3) {
    // Add a vertex to close the loop, and make the vertex shader replace its
    // index (before adding the offset) with 0 to fetch the first vertex again.
    // For indexed line loops, the primitive converter will add the vertex.
    line_loop_closing_index = index_count;
    ++index_count;
  } else {
    // Replace index 0 with 0 (do nothing) otherwise.
    line_loop_closing_index = 0;
  }

  // Translate the shaders and create the pipeline if needed.
  D3D12Shader::D3D12Translation* vertex_shader_translation =
      static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetOrCreateTranslation(
              vertex_shader_modification.value));
  D3D12Shader::D3D12Translation* pixel_shader_translation =
      pixel_shader ? static_cast<D3D12Shader::D3D12Translation*>(
                         pixel_shader->GetOrCreateTranslation(
                             pixel_shader_modification.value))
                   : nullptr;
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation,
          primitive_type_converted,
          indexed ? index_buffer_info->format : xenos::IndexFormat::kInt16,
          pipeline_render_targets, &pipeline_handle, &root_signature)) {
    return false;
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr
           ? pixel_shader->GetUsedTextureMaskAfterTranslation()
           : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  if (current_cached_pipeline_ != pipeline_handle) {
    deferred_command_list_.SetPipelineStateHandle(
        reinterpret_cast<void*>(pipeline_handle));
    current_cached_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state.
  // Supersampling replacing multisampling due to difficulties of emulating
  // EDRAM with multisampling with RTV/DSV (with ROV, there's MSAA), and also
  // resolution scale.
  uint32_t pixel_size_x, pixel_size_y;
  if (edram_rov_used_) {
    pixel_size_x = 1;
    pixel_size_y = 1;
  } else {
    xenos::MsaaSamples msaa_samples =
        regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
    pixel_size_x = msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1;
    pixel_size_y = msaa_samples >= xenos::MsaaSamples::k2X ? 2 : 1;
  }
  if (texture_cache_->IsResolutionScale2X()) {
    pixel_size_x *= 2;
    pixel_size_y *= 2;
  }
  flags::DepthFloat24Conversion depth_float24_conversion =
      render_target_cache_->depth_float24_conversion();
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(
      regs, float(pixel_size_x), float(pixel_size_y), true,
      float(D3D12_VIEWPORT_BOUNDS_MAX), float(D3D12_VIEWPORT_BOUNDS_MAX), false,
      !edram_rov_used_ &&
          (depth_float24_conversion ==
               flags::DepthFloat24Conversion::kOnOutputTruncating ||
           depth_float24_conversion ==
               flags::DepthFloat24Conversion::kOnOutputRounding),
      viewport_info);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.left *= pixel_size_x;
  scissor.top *= pixel_size_y;
  scissor.width *= pixel_size_x;
  scissor.height *= pixel_size_y;

  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal);

  // Update system constants before uploading them.
  UpdateSystemConstantValues(
      memexport_used, primitive_polygonal, line_loop_closing_index,
      indexed ? index_buffer_info->endianness : xenos::Endian::kNone,
      viewport_info, pixel_size_x, pixel_size_y, used_texture_mask,
      pixel_shader ? GetCurrentColorMask(pixel_shader->writes_color_targets())
                   : 0,
      pipeline_render_targets);

  // Update constant buffers, descriptors and root parameters.
  if (!UpdateBindings(vertex_shader, pixel_shader, root_signature)) {
    return false;
  }
  // Must not call anything that can change the descriptor heap from now on!

  // Ensure vertex buffers are resident.
  // TODO(Triang3l): Cache residency for ranges in a way similar to how texture
  // validity is tracked.
  uint64_t vertex_buffers_resident[2] = {};
  for (const Shader::VertexBinding& vertex_binding :
       vertex_shader->vertex_bindings()) {
    uint32_t vfetch_index = vertex_binding.fetch_constant;
    if (vertex_buffers_resident[vfetch_index >> 6] &
        (uint64_t(1) << (vfetch_index & 63))) {
      continue;
    }
    const auto& vfetch_constant = regs.Get<xenos::xe_gpu_vertex_fetch_t>(
        XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + vfetch_index * 2);
    switch (vfetch_constant.type) {
      case xenos::FetchConstantType::kVertex:
        break;
      case xenos::FetchConstantType::kInvalidVertex:
        if (cvars::gpu_allow_invalid_fetch_constants) {
          break;
        }
        XELOGW(
            "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
            "This "
            "is incorrect behavior, but you can try bypassing this by "
            "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
            vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
        return false;
      default:
        XELOGW(
            "Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
            vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
        return false;
    }
    if (!shared_memory_->RequestRange(vfetch_constant.address << 2,
                                      vfetch_constant.size << 2)) {
      XELOGE(
          "Failed to request vertex buffer at 0x{:08X} (size {}) in the shared "
          "memory",
          vfetch_constant.address << 2, vfetch_constant.size << 2);
      return false;
    }
    vertex_buffers_resident[vfetch_index >> 6] |= uint64_t(1)
                                                  << (vfetch_index & 63);
  }

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  struct MemExportRange {
    uint32_t base_address_dwords;
    uint32_t size_dwords;
  };
  MemExportRange memexport_ranges[512];
  uint32_t memexport_range_count = 0;
  if (memexport_used_vertex) {
    for (uint32_t constant_index :
         vertex_shader->memexport_stream_constants()) {
      const auto& memexport_stream = regs.Get<xenos::xe_gpu_memexport_stream_t>(
          XE_GPU_REG_SHADER_CONSTANT_000_X + constant_index * 4);
      if (memexport_stream.index_count == 0) {
        continue;
      }
      uint32_t memexport_format_size =
          GetSupportedMemExportFormatSize(memexport_stream.format);
      if (memexport_format_size == 0) {
        XELOGE("Unsupported memexport format {}",
               FormatInfo::Get(
                   xenos::TextureFormat(uint32_t(memexport_stream.format)))
                   ->name);
        return false;
      }
      uint32_t memexport_size_dwords =
          memexport_stream.index_count * memexport_format_size;
      // Try to reduce the number of shared memory operations when writing
      // different elements into the same buffer through different exports
      // (happens in Halo 3).
      bool memexport_range_reused = false;
      for (uint32_t i = 0; i < memexport_range_count; ++i) {
        MemExportRange& memexport_range = memexport_ranges[i];
        if (memexport_range.base_address_dwords ==
            memexport_stream.base_address) {
          memexport_range.size_dwords =
              std::max(memexport_range.size_dwords, memexport_size_dwords);
          memexport_range_reused = true;
          break;
        }
      }
      // Add a new range if haven't expanded an existing one.
      if (!memexport_range_reused) {
        MemExportRange& memexport_range =
            memexport_ranges[memexport_range_count++];
        memexport_range.base_address_dwords = memexport_stream.base_address;
        memexport_range.size_dwords = memexport_size_dwords;
      }
    }
  }
  if (memexport_used_pixel) {
    for (uint32_t constant_index : pixel_shader->memexport_stream_constants()) {
      const auto& memexport_stream = regs.Get<xenos::xe_gpu_memexport_stream_t>(
          XE_GPU_REG_SHADER_CONSTANT_256_X + constant_index * 4);
      if (memexport_stream.index_count == 0) {
        continue;
      }
      uint32_t memexport_format_size =
          GetSupportedMemExportFormatSize(memexport_stream.format);
      if (memexport_format_size == 0) {
        XELOGE("Unsupported memexport format {}",
               FormatInfo::Get(
                   xenos::TextureFormat(uint32_t(memexport_stream.format)))
                   ->name);
        return false;
      }
      uint32_t memexport_size_dwords =
          memexport_stream.index_count * memexport_format_size;
      bool memexport_range_reused = false;
      for (uint32_t i = 0; i < memexport_range_count; ++i) {
        MemExportRange& memexport_range = memexport_ranges[i];
        if (memexport_range.base_address_dwords ==
            memexport_stream.base_address) {
          memexport_range.size_dwords =
              std::max(memexport_range.size_dwords, memexport_size_dwords);
          memexport_range_reused = true;
          break;
        }
      }
      if (!memexport_range_reused) {
        MemExportRange& memexport_range =
            memexport_ranges[memexport_range_count++];
        memexport_range.base_address_dwords = memexport_stream.base_address;
        memexport_range.size_dwords = memexport_size_dwords;
      }
    }
  }
  for (uint32_t i = 0; i < memexport_range_count; ++i) {
    const MemExportRange& memexport_range = memexport_ranges[i];
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_dwords << 2)) {
      XELOGE(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range.base_address_dwords << 2,
          memexport_range.size_dwords << 2);
      return false;
    }
  }

  // Actually draw.
  if (indexed) {
    uint32_t index_size =
        index_buffer_info->format == xenos::IndexFormat::kInt32
            ? sizeof(uint32_t)
            : sizeof(uint16_t);
    assert_false(index_buffer_info->guest_base & (index_size - 1));
    uint32_t index_base =
        index_buffer_info->guest_base & 0x1FFFFFFF & ~(index_size - 1);
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.Format =
        index_buffer_info->format == xenos::IndexFormat::kInt32
            ? DXGI_FORMAT_R32_UINT
            : DXGI_FORMAT_R16_UINT;
    PrimitiveConverter::ConversionResult conversion_result;
    uint32_t converted_index_count;
    if (tessellated) {
      conversion_result =
          PrimitiveConverter::ConversionResult::kConversionNotNeeded;
    } else {
      conversion_result = primitive_converter_->ConvertPrimitives(
          primitive_type, index_buffer_info->guest_base, index_count,
          index_buffer_info->format, index_buffer_info->endianness,
          index_buffer_view.BufferLocation, converted_index_count);
      if (conversion_result == PrimitiveConverter::ConversionResult::kFailed) {
        return false;
      }
      if (conversion_result ==
          PrimitiveConverter::ConversionResult::kPrimitiveEmpty) {
        return true;
      }
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    if (conversion_result == PrimitiveConverter::ConversionResult::kConverted) {
      index_buffer_view.SizeInBytes = converted_index_count * index_size;
      index_count = converted_index_count;
    } else {
      uint32_t index_buffer_size = index_buffer_info->count * index_size;
      if (!shared_memory_->RequestRange(index_base, index_buffer_size)) {
        XELOGE(
            "Failed to request index buffer at 0x{:08X} (size {}) in the "
            "shared memory",
            index_base, index_buffer_size);
        return false;
      }
      if (memexport_used) {
        // If the shared memory is a UAV, it can't be used as an index buffer
        // (UAV is a read/write state, index buffer is a read-only state). Need
        // to copy the indices to a buffer in the index buffer state.
        scratch_index_buffer = RequestScratchGPUBuffer(
            index_buffer_size, D3D12_RESOURCE_STATE_COPY_DEST);
        if (scratch_index_buffer == nullptr) {
          return false;
        }
        shared_memory_->UseAsCopySource();
        SubmitBarriers();
        deferred_command_list_.D3DCopyBufferRegion(
            scratch_index_buffer, 0, shared_memory_->GetBuffer(), index_base,
            index_buffer_size);
        PushTransitionBarrier(scratch_index_buffer,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_INDEX_BUFFER);
        index_buffer_view.BufferLocation =
            scratch_index_buffer->GetGPUVirtualAddress();
      } else {
        index_buffer_view.BufferLocation =
            shared_memory_->GetGPUAddress() + index_base;
      }
      index_buffer_view.SizeInBytes = index_buffer_size;
    }
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    deferred_command_list_.D3DDrawIndexedInstanced(index_count, 1, 0, 0, 0);
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer,
                              D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  } else {
    // Check if need to draw using a conversion index buffer.
    uint32_t converted_index_count = 0;
    D3D12_GPU_VIRTUAL_ADDRESS conversion_gpu_address =
        tessellated ? 0
                    : primitive_converter_->GetStaticIndexBuffer(
                          primitive_type, index_count, converted_index_count);
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    if (conversion_gpu_address) {
      D3D12_INDEX_BUFFER_VIEW index_buffer_view;
      index_buffer_view.BufferLocation = conversion_gpu_address;
      index_buffer_view.SizeInBytes = converted_index_count * sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
      deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
      deferred_command_list_.D3DDrawIndexedInstanced(converted_index_count, 1,
                                                     0, 0, 0);
    } else {
      deferred_command_list_.D3DDrawInstanced(index_count, 1, 0, 0);
    }
  }

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    for (uint32_t i = 0; i < memexport_range_count; ++i) {
      const MemExportRange& memexport_range = memexport_ranges[i];
      shared_memory_->RangeWrittenByGpu(
          memexport_range.base_address_dwords << 2,
          memexport_range.size_dwords << 2);
    }
    if (cvars::d3d12_readback_memexport) {
      // Read the exported data on the CPU.
      uint32_t memexport_total_size = 0;
      for (uint32_t i = 0; i < memexport_range_count; ++i) {
        memexport_total_size += memexport_ranges[i].size_dwords << 2;
      }
      if (memexport_total_size != 0) {
        ID3D12Resource* readback_buffer =
            RequestReadbackBuffer(memexport_total_size);
        if (readback_buffer != nullptr) {
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
          uint32_t readback_buffer_offset = 0;
          for (uint32_t i = 0; i < memexport_range_count; ++i) {
            const MemExportRange& memexport_range = memexport_ranges[i];
            uint32_t memexport_range_size = memexport_range.size_dwords << 2;
            deferred_command_list_.D3DCopyBufferRegion(
                readback_buffer, readback_buffer_offset, shared_memory_buffer,
                memexport_range.base_address_dwords << 2, memexport_range_size);
            readback_buffer_offset += memexport_range_size;
          }
          if (AwaitAllQueueOperationsCompletion()) {
            D3D12_RANGE readback_range;
            readback_range.Begin = 0;
            readback_range.End = memexport_total_size;
            void* readback_mapping;
            if (SUCCEEDED(readback_buffer->Map(0, &readback_range,
                                               &readback_mapping))) {
              const uint32_t* readback_dwords =
                  reinterpret_cast<const uint32_t*>(readback_mapping);
              for (uint32_t i = 0; i < memexport_range_count; ++i) {
                const MemExportRange& memexport_range = memexport_ranges[i];
                std::memcpy(memory_->TranslatePhysical(
                                memexport_range.base_address_dwords << 2),
                            readback_dwords, memexport_range.size_dwords << 2);
                readback_dwords += memexport_range.size_dwords;
              }
              D3D12_RANGE readback_write_range = {};
              readback_buffer->Unmap(0, &readback_write_range);
            }
          }
        }
      }
    }
  }

  return true;
}

void D3D12CommandProcessor::InitializeTrace() {
  BeginSubmission(false);
  bool render_target_cache_submitted =
      render_target_cache_->InitializeTraceSubmitDownloads();
  bool shared_memory_submitted =
      shared_memory_->InitializeTraceSubmitDownloads();
  if (!render_target_cache_submitted && !shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (render_target_cache_submitted) {
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  BeginSubmission(true);
  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                     written_address, written_length)) {
    return false;
  }
  if (cvars::d3d12_readback_resolve && !texture_cache_->IsResolutionScale2X() &&
      written_length) {
    // Read the resolved data on the CPU.
    ID3D12Resource* readback_buffer = RequestReadbackBuffer(written_length);
    if (readback_buffer != nullptr) {
      shared_memory_->UseAsCopySource();
      SubmitBarriers();
      ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
      deferred_command_list_.D3DCopyBufferRegion(
          readback_buffer, 0, shared_memory_buffer, written_address,
          written_length);
      if (AwaitAllQueueOperationsCompletion()) {
        D3D12_RANGE readback_range;
        readback_range.Begin = 0;
        readback_range.End = written_length;
        void* readback_mapping;
        if (SUCCEEDED(
                readback_buffer->Map(0, &readback_range, &readback_mapping))) {
          std::memcpy(memory_->TranslatePhysical(written_address),
                      readback_mapping, written_length);
          D3D12_RANGE readback_write_range = {};
          readback_buffer->Unmap(0, &readback_write_range);
        }
      }
    }
  }
  return true;
}

void D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission) {
  if (await_submission >= submission_current_) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      ID3D12CommandQueue* direct_queue =
          GetD3D12Context().GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(
              direct_queue->Signal(queue_operations_since_submission_fence_,
                                   fence_value) &&
              SUCCEEDED(queue_operations_since_submission_fence_
                            ->SetEventOnCompletion(fence_value,
                                                   fence_completion_event_)))) {
        WaitForSingleObject(fence_completion_event_, INFINITE);
        queue_operations_done_since_submission_signal_ = false;
      } else {
        XELOGE(
            "Failed to await an out-of-submission queue operation completion "
            "Direct3D 12 fence");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = submission_current_ - 1;
  }

  uint64_t submission_completed_before = submission_completed_;
  submission_completed_ = submission_fence_->GetCompletedValue();
  if (submission_completed_ < await_submission) {
    if (SUCCEEDED(submission_fence_->SetEventOnCompletion(
            await_submission, fence_completion_event_))) {
      WaitForSingleObject(fence_completion_event_, INFINITE);
      submission_completed_ = submission_fence_->GetCompletedValue();
    }
  }
  if (submission_completed_ < await_submission) {
    XELOGE("Failed to await a submission completion Direct3D 12 fence");
  }
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return;
  }

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission >
        submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next =
          command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ =
        command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second >
        submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(
        view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient buffers marked for deletion.
  while (!buffers_for_deletion_.empty()) {
    if (buffers_for_deletion_.front().second > submission_completed_) {
      break;
    }
    buffers_for_deletion_.front().first->Release();
    buffers_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_converter_->CompletedSubmissionUpdated();
}

void D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionFence(
      is_opening_frame
          ? closed_frame_submissions_[frame_current_ % kQueueFrames]
          : 0);
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ =
        std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] >
          submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    current_sample_positions_ = xenos::MsaaSamples::k1X;
    current_cached_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_converter_->BeginSubmission();
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0,
                sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ =
          ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ =
          ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }

    pix_capturing_ =
        pix_capture_requested_.exchange(false, std::memory_order_relaxed);
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis =
          GetD3D12Context().GetD3D12Provider().GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->BeginCapture();
      }
    }

    texture_cache_->BeginFrame();

    primitive_converter_->BeginFrame();
  }
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  auto& provider = GetD3D12Context().GetD3D12Provider();

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&command_allocator)))) {
      XELOGE("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    render_target_cache_->EndFrame();

    texture_cache_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    pipeline_cache_->EndSubmission();

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

    auto direct_queue = provider.GetDirectQueue();

    // Submit the command list.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    command_allocator->Reset();
    command_list_->Reset(command_allocator, nullptr);
    deferred_command_list_.Execute(command_list_, command_list_1_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    command_allocator_writable_first_->last_usage_submission =
        submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next =
          command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    direct_queue->Signal(submission_fence_, submission_current_++);

    submission_open_ = false;

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
    // Close the capture after submitting.
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis = provider.GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->EndCapture();
      }
      pix_capturing_ = false;
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] =
        submission_current_ - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed :
             sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      primitive_converter_->ClearCache();

      pipeline_cache_->ClearCache();

      render_target_cache_->ClearCache();

      texture_cache_->ClearCache();

      for (auto it : root_signatures_bindful_) {
        it.second->Release();
      }
      root_signatures_bindful_.clear();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info,
    const draw_util::Scissor& scissor, bool primitive_polygonal) {
#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = viewport_info.left;
  viewport.TopLeftY = viewport_info.top;
  viewport.Width = viewport_info.width;
  viewport.Height = viewport_info.height;
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(viewport);
    ff_viewport_update_needed_ = false;
  }

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.left);
  scissor_rect.top = LONG(scissor.top);
  scissor_rect.right = LONG(scissor.left + scissor.width);
  scissor_rect.bottom = LONG(scissor.top + scissor.height);
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(scissor_rect);
    ff_scissor_update_needed_ = false;
  }

  if (!edram_rov_used_) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    ff_blend_factor_update_needed_ |=
        ff_blend_factor_[0] != regs[XE_GPU_REG_RB_BLEND_RED].f32;
    ff_blend_factor_update_needed_ |=
        ff_blend_factor_[1] != regs[XE_GPU_REG_RB_BLEND_GREEN].f32;
    ff_blend_factor_update_needed_ |=
        ff_blend_factor_[2] != regs[XE_GPU_REG_RB_BLEND_BLUE].f32;
    ff_blend_factor_update_needed_ |=
        ff_blend_factor_[3] != regs[XE_GPU_REG_RB_BLEND_ALPHA].f32;
    if (ff_blend_factor_update_needed_) {
      ff_blend_factor_[0] = regs[XE_GPU_REG_RB_BLEND_RED].f32;
      ff_blend_factor_[1] = regs[XE_GPU_REG_RB_BLEND_GREEN].f32;
      ff_blend_factor_[2] = regs[XE_GPU_REG_RB_BLEND_BLUE].f32;
      ff_blend_factor_[3] = regs[XE_GPU_REG_RB_BLEND_ALPHA].f32;
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal &&
        draw_util::GetDepthControlForCurrentEdramMode(regs).backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref =
        regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal,
    uint32_t line_loop_closing_index, xenos::Endian index_endian,
    const draw_util::ViewportInfo& viewport_info, uint32_t pixel_size_x,
    uint32_t pixel_size_y, uint32_t used_texture_mask, uint32_t color_mask,
    const RenderTargetCache::PipelineRenderTarget render_targets[4]) {
#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
  auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  float rb_alpha_ref = regs[XE_GPU_REG_RB_ALPHA_REF].f32;
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_depthcontrol = draw_util::GetDepthControlForCurrentEdramMode(regs);
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  int32_t vgt_indx_offset = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET].u32);

  // Get the color info register values for each render target, and also put
  // some safety measures for the ROV path - disable fully aliased render
  // targets. Also, for ROV, exclude components that don't exist in the format
  // from the write mask.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;

    if (edram_rov_used_) {
      // Get the mask for keeping previous color's components unmodified,
      // or two UINT32_MAX if no colors actually existing in the RT are written.
      DxbcShaderTranslator::ROV_GetColorFormatSystemConstants(
          color_info.color_format, (color_mask >> (i * 4)) & 0b1111,
          rt_clamp[i][0], rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3],
          rt_keep_masks[i][0], rt_keep_masks[i][1]);

      // Disable the render target if it has the same EDRAM base as another one
      // (with a smaller index - assume it's more important).
      if (rt_keep_masks[i][0] == UINT32_MAX &&
          rt_keep_masks[i][1] == UINT32_MAX) {
        for (uint32_t j = 0; j < i; ++j) {
          if (color_info.color_base == color_infos[j].color_base &&
              (rt_keep_masks[j][0] != UINT32_MAX ||
               rt_keep_masks[j][1] != UINT32_MAX)) {
            rt_keep_masks[i][0] = UINT32_MAX;
            rt_keep_masks[i][1] = UINT32_MAX;
            break;
          }
        }
      }
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in Banjo-Kazooie, though depth writing is
  // already disabled there).
  bool depth_stencil_enabled =
      rb_depthcontrol.stencil_enable || rb_depthcontrol.z_enable;
  if (edram_rov_used_ && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX ||
           rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  if (!pa_cl_clip_cntl.clip_disable) {
    flags |= (pa_cl_clip_cntl.value & 0b111111)
             << DxbcShaderTranslator::kSysFlag_UserClipPlane0_Shift;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive killing condition.
  if (pa_cl_clip_cntl.vtx_kill_or) {
    flags |= DxbcShaderTranslator::kSysFlag_KillIfAnyVertexKilled;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  for (uint32_t i = 0; i < 4; ++i) {
    if (color_infos[i].color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= DxbcShaderTranslator::kSysFlag_Color0Gamma << i;
    }
  }
  if (edram_rov_used_ && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthFloat24;
    }
    if (rb_depthcontrol.z_enable) {
      flags |= uint32_t(rb_depthcontrol.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (rb_depthcontrol.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (rb_depthcontrol.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min =
      regs[XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL].f32 + 1.0f;
  float tessellation_factor_max =
      regs[XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL].f32 + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min !=
           tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max !=
           tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  if (!pa_cl_clip_cntl.clip_disable) {
    for (uint32_t i = 0; i < 6; ++i) {
      if (!(pa_cl_clip_cntl.value & (1 << i))) {
        continue;
      }
      const float* ucp = &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4].f32;
      if (std::memcmp(system_constants_.user_clip_planes[i], ucp,
                      4 * sizeof(float))) {
        dirty = true;
        std::memcpy(system_constants_.user_clip_planes[i], ucp,
                    4 * sizeof(float));
      }
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  float point_size_x = float(pa_su_point_size.width) * 0.125f;
  float point_size_y = float(pa_su_point_size.height) * 0.125f;
  float point_size_min = float(pa_su_point_minmax.min_size) * 0.125f;
  float point_size_max = float(pa_su_point_minmax.max_size) * 0.125f;
  dirty |= system_constants_.point_size[0] != point_size_x;
  dirty |= system_constants_.point_size[1] != point_size_y;
  dirty |= system_constants_.point_size_min_max[0] != point_size_min;
  dirty |= system_constants_.point_size_min_max[1] != point_size_max;
  system_constants_.point_size[0] = point_size_x;
  system_constants_.point_size[1] = point_size_y;
  system_constants_.point_size_min_max[0] = point_size_min;
  system_constants_.point_size_min_max[1] = point_size_max;
  float point_screen_to_ndc_x =
      (0.5f * 2.0f * pixel_size_x) / viewport_info.width;
  float point_screen_to_ndc_y =
      (0.5f * 2.0f * pixel_size_y) / viewport_info.height;
  dirty |= system_constants_.point_screen_to_ndc[0] != point_screen_to_ndc_x;
  dirty |= system_constants_.point_screen_to_ndc[1] != point_screen_to_ndc_y;
  system_constants_.point_screen_to_ndc[0] = point_screen_to_ndc_x;
  system_constants_.point_screen_to_ndc[1] = point_screen_to_ndc_y;

  // Pixel parameter register.
  uint32_t ps_param_gen =
      sq_program_cntl.param_gen ? sq_context_misc.param_gen_pos : UINT_MAX;
  dirty |= system_constants_.ps_param_gen != ps_param_gen;
  system_constants_.ps_param_gen = ps_param_gen;

  // Texture signedness.
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint =
        system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint32_t texture_signs_shifted =
        uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(texture_index))
        << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint =
        (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
  }

  // Log2 of sample count, for scaling VPOS with SSAA (without ROV) and for
  // EDRAM address calculation with MSAA (with ROV).
  uint32_t sample_count_log2_x =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y =
      rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                               ? (rb_colorcontrol.value >> 24) | (1 << 8)
                               : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  // EDRAM pitch for ROV writing.
  if (edram_rov_used_) {
    uint32_t edram_pitch_tiles =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         79) /
        80;
    dirty |= system_constants_.edram_pitch_tiles != edram_pitch_tiles;
    system_constants_.edram_pitch_tiles = edram_pitch_tiles;
  }

  // Color exponent bias and output index mapping or ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format ==
            xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      // On the Xbox 360, k_16_16_EDRAM and k_16_16_16_16_EDRAM internally have
      // -32...32 range and expect shaders to give -32...32 values, but they're
      // emulated using normalized RG16/RGBA16 when not using the ROV, so the
      // value returned from the shader needs to be divided by 32 (blending will
      // be incorrect in this case, but there's no other way without using ROV,
      // though there's an option to limit the range to -1...1).
      // http://www.students.science.uu.nl/~3220516/advancedgraphics/papers/inferred_lighting.pdf
      if (!edram_rov_used_ && cvars::d3d12_16bit_rtv_full_range) {
        color_exp_bias -= 5;
      }
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        0x3F800000 + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_rov_used_) {
      dirty |=
          system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |=
          system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX ||
          rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * 1280;
        if (texture_cache_->IsResolutionScale2X()) {
          rt_base_dwords_scaled <<= 2;
        }
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] !=
                 rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] =
            rt_base_dwords_scaled;
        uint32_t format_flags = DxbcShaderTranslator::ROV_AddColorFormatFlags(
            color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |= std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i],
                             4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i],
                    4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]].u32 & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] !=
                 blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    } else {
      dirty |= system_constants_.color_output_map[i] !=
               render_targets[i].guest_render_target;
      system_constants_.color_output_map[i] =
          render_targets[i].guest_render_target;
    }
  }

  // Interpolator sampling pattern, resolution scale, depth/stencil testing and
  // blend constant for ROV.
  if (edram_rov_used_) {
    // Not needed without ROV because without ROV, MSAA is faked with SSAA, and
    // everything is interpolated at samples, without the possibility of
    // extrapolation.
    uint32_t interpolator_sampling_pattern =
        xenos::GetInterpolatorSamplingPattern(
            rb_surface_info.msaa_samples, sq_context_misc.sc_sample_cntl,
            regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    dirty |= system_constants_.interpolator_sampling_pattern !=
             interpolator_sampling_pattern;
    system_constants_.interpolator_sampling_pattern =
        interpolator_sampling_pattern;

    uint32_t resolution_square_scale =
        texture_cache_->IsResolutionScale2X() ? 4 : 1;
    dirty |= system_constants_.edram_resolution_square_scale !=
             resolution_square_scale;
    system_constants_.edram_resolution_square_scale = resolution_square_scale;

    uint32_t depth_base_dwords = rb_depth_info.depth_base * 1280;
    dirty |= system_constants_.edram_depth_base_dwords != depth_base_dwords;
    system_constants_.edram_depth_base_dwords = depth_base_dwords;

    float depth_range_scale = viewport_info.z_max - viewport_info.z_min;
    dirty |= system_constants_.edram_depth_range_scale != depth_range_scale;
    system_constants_.edram_depth_range_scale = depth_range_scale;
    dirty |= system_constants_.edram_depth_range_offset != viewport_info.z_min;
    system_constants_.edram_depth_range_offset = viewport_info.z_min;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale =
            regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE].f32;
        poly_offset_front_offset =
            regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET].f32;
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale =
            regs[XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE].f32;
        poly_offset_back_offset =
            regs[XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET].f32;
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale =
            regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE].f32;
        poly_offset_front_offset =
            regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET].f32;
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // "slope computed in subpixels (1/12 or 1/16)" - R5xx Acceleration. Also:
    // https://github.com/mesa3d/mesa/blob/54ad9b444c8e73da498211870e785239ad3ff1aa/src/gallium/drivers/radeonsi/si_state.c#L943
    poly_offset_front_scale *= 1.0f / 16.0f;
    poly_offset_back_scale *= 1.0f / 16.0f;
    if (texture_cache_->IsResolutionScale2X()) {
      poly_offset_front_scale *= 2.f;
      poly_offset_back_scale *= 2.f;
    }
    dirty |= system_constants_.edram_poly_offset_front_scale !=
             poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset !=
             poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale !=
             poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset !=
             poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && rb_depthcontrol.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference !=
               rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference =
          rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask !=
               rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask =
          rb_stencilrefmask.stencilmask;
      dirty |= system_constants_.edram_stencil_front_write_mask !=
               rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask =
          rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops =
          (rb_depthcontrol.value >> 8) & ((1 << 12) - 1);
      dirty |=
          system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && rb_depthcontrol.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference !=
                 rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference =
            rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask !=
                 rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask =
            rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask =
            rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf =
            (rb_depthcontrol.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops !=
                 stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front,
                             4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back,
                    system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] !=
             regs[XE_GPU_REG_RB_BLEND_RED].f32;
    system_constants_.edram_blend_constant[0] =
        regs[XE_GPU_REG_RB_BLEND_RED].f32;
    dirty |= system_constants_.edram_blend_constant[1] !=
             regs[XE_GPU_REG_RB_BLEND_GREEN].f32;
    system_constants_.edram_blend_constant[1] =
        regs[XE_GPU_REG_RB_BLEND_GREEN].f32;
    dirty |= system_constants_.edram_blend_constant[2] !=
             regs[XE_GPU_REG_RB_BLEND_BLUE].f32;
    system_constants_.edram_blend_constant[2] =
        regs[XE_GPU_REG_RB_BLEND_BLUE].f32;
    dirty |= system_constants_.edram_blend_constant[3] !=
             regs[XE_GPU_REG_RB_BLEND_ALPHA].f32;
    system_constants_.edram_blend_constant[3] =
        regs[XE_GPU_REG_RB_BLEND_ALPHA].f32;
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

bool D3D12CommandProcessor::UpdateBindings(
    const D3D12Shader* vertex_shader, const D3D12Shader* pixel_shader,
    ID3D12RootSignature* root_signature) {
  auto& provider = GetD3D12Context().GetD3D12Provider();
  auto device = provider.GetDevice();
  auto& regs = *register_file_;

#if XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_UI_D3D12_FINE_GRAINED_DRAW_SCOPES

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(
          vertex_shader, pixel_shader, current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants =
      bindless_resources_used_ ? kRootParameter_Bindless_FetchConstants
                               : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsPixel
                               : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants =
      bindless_resources_used_ ? kRootParameter_Bindless_SystemConstants
                               : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants =
      bindless_resources_used_ ? kRootParameter_Bindless_BoolLoopConstants
                               : kRootParameter_Bindful_BoolLoopConstants;

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST].u32 == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST].u32 == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST].u32 == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST].u32 == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_,
                sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_,
        sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry =
          float_constant_map_vertex.float_bitmap[i];
      uint32_t float_constant_index;
      while (xe::bit_scan_forward(float_constant_map_entry,
                                  &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(float_constants,
                    &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) +
                          (float_constant_index << 2)]
                         .f32,
                    4 * sizeof(float));
        float_constants += 4 * sizeof(float);
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_,
        sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry =
            float_constant_map_pixel.float_bitmap[i];
        uint32_t float_constant_index;
        while (xe::bit_scan_forward(float_constant_map_entry,
                                    &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(float_constants,
                      &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) +
                            (float_constant_index << 2)]
                           .f32,
                      4 * sizeof(float));
          float_constants += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants,
                &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031].u32,
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants,
                &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0].u32,
                kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex =
      vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex =
      vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      current_samplers_pixel_.resize(std::max(current_samplers_pixel_.size(),
                                              size_t(sampler_count_pixel)));
      for (uint32_t i = 0; i < sampler_count_pixel; ++i) {
        TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex &&
        cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_vertex_.data(), textures_vertex.data(),
             texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel &&
        cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(),
             texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex &&
         !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel &&
         !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <=
                  submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags =
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(
                    &sampler_heap_new_desc, IID_PPV_ARGS(&sampler_heap_new)))) {
              XELOGE(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(std::make_pair(
              sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_
                  ->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_
                  ->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(
              view_bindless_heap_, sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &=
              ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex &&
            !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(
              std::max(current_sampler_bindless_indices_vertex_.size(),
                       size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            TextureCache::SamplerParameters sampler_parameters =
                current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(
                sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(
                  sampler_parameters,
                  provider.OffsetSamplerDescriptor(
                      sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(
                  sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel &&
            !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(
              std::max(current_sampler_bindless_indices_pixel_.size(),
                       size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            TextureCache::SamplerParameters sampler_parameters =
                current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(
                sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(
                  sampler_parameters,
                  provider.OffsetSamplerDescriptor(
                      sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(
                  sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices =
          reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
              frame_current_,
              std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) *
                  sizeof(uint32_t),
              D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
              &cbuffer_binding_descriptor_indices_vertex_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(),
                     size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(
            current_texture_srv_keys_vertex_.data(), textures_vertex.data(),
            texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices =
          reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
              frame_current_,
              std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) *
                  sizeof(uint32_t),
              D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
              &cbuffer_binding_descriptor_indices_pixel_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(),
                     size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(
            current_texture_srv_keys_pixel_.data(), textures_pixel->data(),
            texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex &&
        (!bindful_textures_written_vertex_ ||
         current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_vertex_.data(), textures_vertex.data(),
             texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(),
             texture_count_pixel));
    bool write_samplers_vertex =
        sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel =
        sampler_count_pixel && !bindful_samplers_written_pixel_;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // All the constants + shared memory SRV and UAV + textures.
    size_t view_count_full_update =
        2 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used_) {
      // + EDRAM UAV.
      ++view_count_full_update;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      XELOGE("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index =
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_,
          uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel),
          sampler_cpu_handle, sampler_gpu_handle);
      if (sampler_heap_index ==
          ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        XELOGE("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      gpu_handle_shared_memory_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used_) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle,
                                                              2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &=
          ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index !=
            ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i],
                                                     view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(),
                   size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(
          current_texture_srv_keys_vertex_.data(), textures_vertex.data(),
          texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i],
                                                     view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(std::max(
          current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(
          current_texture_srv_keys_pixel_.data(), textures_pixel->data(),
          texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i],
                                     sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i],
                                     sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index !=
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_fetch_constants, cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex,
        cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |=
        1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel,
        cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |=
        1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_system_constants, cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_bool_loop_constants, cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u
                                         << root_parameter_bool_loop_constants;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesPixel,
          cbuffer_binding_descriptor_indices_pixel_.address);
      current_graphics_root_up_to_date_ |=
          1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesVertex,
          cbuffer_binding_descriptor_indices_vertex_.address);
      current_graphics_root_up_to_date_ |=
          1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          kRootParameter_Bindless_SamplerHeap,
          sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |=
          1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          kRootParameter_Bindless_ViewHeap, view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u
                                           << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindful_SharedMemoryAndEdram))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          kRootParameter_Bindful_SharedMemoryAndEdram,
          gpu_handle_shared_memory_and_edram_);
      current_graphics_root_up_to_date_ |=
          1u << kRootParameter_Bindful_SharedMemoryAndEdram;
    }
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          extra_index, gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          extra_index, gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          extra_index, gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
          extra_index, gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

uint32_t D3D12CommandProcessor::GetSupportedMemExportFormatSize(
    xenos::ColorFormat format) {
  switch (format) {
    case xenos::ColorFormat::k_8_8_8_8:
    case xenos::ColorFormat::k_2_10_10_10:
    // TODO(Triang3l): Investigate how k_8_8_8_8_A works - not supported in the
    // texture cache currently.
    // case xenos::ColorFormat::k_8_8_8_8_A:
    case xenos::ColorFormat::k_10_11_11:
    case xenos::ColorFormat::k_11_11_10:
    case xenos::ColorFormat::k_16_16:
    case xenos::ColorFormat::k_16_16_FLOAT:
    case xenos::ColorFormat::k_32_FLOAT:
    case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16:
    case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16:
    case xenos::ColorFormat::k_10_11_11_AS_16_16_16_16:
    case xenos::ColorFormat::k_11_11_10_AS_16_16_16_16:
      return 1;
    case xenos::ColorFormat::k_16_16_16_16:
    case xenos::ColorFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorFormat::k_32_32_FLOAT:
      return 2;
    case xenos::ColorFormat::k_32_32_32_32_FLOAT:
      return 4;
    default:
      break;
  }
  return 0;
}

ID3D12Resource* D3D12CommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return nullptr;
  }
  size = xe::align(size, kReadbackBufferSizeIncrement);
  if (size > readback_buffer_size_) {
    auto& provider = GetD3D12Context().GetD3D12Provider();
    auto device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                            D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback,
            provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      XELOGE("Failed to create a {} MB readback buffer", size >> 20);
      return nullptr;
    }
    if (readback_buffer_ != nullptr) {
      readback_buffer_->Release();
    }
    readback_buffer_ = buffer;
  }
  return readback_buffer_;
}

void D3D12CommandProcessor::WriteGammaRampSRV(
    bool is_pwl, D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  auto device = GetD3D12Context().GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  // 256-entry for normal, 128-entry for PWL.
  desc.Texture1D.MostDetailedMip = is_pwl ? 1 : 0;
  desc.Texture1D.MipLevels = 1;
  desc.Texture1D.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(gamma_ramp_texture_, &desc, handle);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
