/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_TEXTURE_CACHE_H_
#define XENIA_GPU_D3D12_TEXTURE_CACHE_H_

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "xenia/base/mutex.h"
#include "xenia/gpu/d3d12/d3d12_shader.h"
#include "xenia/gpu/d3d12/d3d12_shared_memory.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12CommandProcessor;

// Manages host copies of guest textures, performing untiling, format and endian
// conversion of textures stored in the shared memory, and also handling
// invalidation.
//
// Mipmaps are treated the following way, according to the GPU hang message
// found in game executables explaining the valid usage of BaseAddress when
// streaming the largest LOD (it says games should not use 0 as the base address
// when the largest LOD isn't loaded, but rather, either allocate a valid
// address for it or make it the same as MipAddress):
// - If the texture has a base address, but no mip address, it's not mipmapped -
//   the host texture has only the largest level too.
// - If the texture has different non-zero base address and mip address, a host
//   texture with mip_max_level+1 mipmaps is created - mip_min_level is ignored
//   and treated purely as sampler state because there are tfetch instructions
//   working directly with LOD values - including fetching with an explicit LOD.
//   However, the max level is not ignored because any mip count can be
//   specified when creating a texture, and another texture may be placed after
//   the last one.
// - If the texture has a mip address, but the base address is 0 or the same as
//   the mip address, a mipmapped texture is created, but min/max LOD is clamped
//   to the lower bound of 1 - the game is expected to do that anyway until the
//   largest LOD is loaded.
//   TODO(Triang3l): Check if there are any games with BaseAddress==MipAddress
//   but min or max LOD being 0, especially check Modern Warfare 2/3.
//   TODO(Triang3l): Attach the largest LOD to existing textures with a valid
//   MipAddress but no BaseAddress to save memory because textures are streamed
//   this way anyway.
class TextureCache {
  union TextureKey {
    struct {
      // Physical 4 KB page with the base mip level, disregarding A/C/E address
      // range prefix.
      uint32_t base_page : 17;             // 17 total
      xenos::DataDimension dimension : 2;  // 19
      uint32_t width : 13;                 // 32

      uint32_t height : 13;      // 45
      uint32_t tiled : 1;        // 46
      uint32_t packed_mips : 1;  // 47
      // Physical 4 KB page with mip 1 and smaller.
      uint32_t mip_page : 17;  // 64

      // Layers for stacked and 3D, 6 for cube, 1 for other dimensions.
      uint32_t depth : 10;              // 74
      uint32_t mip_max_level : 4;       // 78
      xenos::TextureFormat format : 6;  // 84
      xenos::Endian endianness : 2;     // 86
      // Whether this texture is signed and has a different host representation
      // than an unsigned view of the same guest texture.
      uint32_t signed_separate : 1;  // 87
      // Whether this texture is a 2x-scaled resolve target.
      uint32_t scaled_resolve : 1;  // 88
    };
    struct {
      // The key used for unordered_multimap lookup. Single uint32_t instead of
      // a uint64_t so XXH hash can be calculated in a stable way due to no
      // padding.
      uint32_t map_key[2];
      // The key used to identify one texture within unordered_multimap buckets.
      uint32_t bucket_key;
    };
    TextureKey() { MakeInvalid(); }
    TextureKey(const TextureKey& key) {
      SetMapKey(key.GetMapKey());
      bucket_key = key.bucket_key;
    }
    TextureKey& operator=(const TextureKey& key) {
      SetMapKey(key.GetMapKey());
      bucket_key = key.bucket_key;
      return *this;
    }
    bool operator==(const TextureKey& key) const {
      return GetMapKey() == key.GetMapKey() && bucket_key == key.bucket_key;
    }
    bool operator!=(const TextureKey& key) const {
      return GetMapKey() != key.GetMapKey() || bucket_key != key.bucket_key;
    }
    uint64_t GetMapKey() const {
      return uint64_t(map_key[0]) | (uint64_t(map_key[1]) << 32);
    }
    void SetMapKey(uint64_t key) {
      map_key[0] = uint32_t(key);
      map_key[1] = uint32_t(key >> 32);
    }
    bool IsInvalid() const {
      // Zero base and zero width is enough for a binding to be invalid.
      return map_key[0] == 0;
    }
    void MakeInvalid() {
      // Reset all for a stable hash.
      SetMapKey(0);
      bucket_key = 0;
    }
  };

 public:
  // Keys that can be stored for checking validity whether descriptors for host
  // shader bindings are up to date.
  struct TextureSRVKey {
    TextureKey key;
    uint32_t host_swizzle;
    uint8_t swizzled_signs;
  };

  // Sampler parameters that can be directly converted to a host sampler or used
  // for binding checking validity whether samplers are up to date.
  union SamplerParameters {
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      // For anisotropic, these are true.
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      // Maximum mip level is in the texture resource itself.
    };
    uint32_t value;

    // Clearing the unused bits.
    SamplerParameters() : value(0) {}
    SamplerParameters(const SamplerParameters& parameters)
        : value(parameters.value) {}
    SamplerParameters& operator=(const SamplerParameters& parameters) {
      value = parameters.value;
      return *this;
    }
    bool operator==(const SamplerParameters& parameters) const {
      return value == parameters.value;
    }
    bool operator!=(const SamplerParameters& parameters) const {
      return value != parameters.value;
    }
  };

  TextureCache(D3D12CommandProcessor& command_processor,
               const RegisterFile& register_file, bool bindless_resources_used,
               D3D12SharedMemory& shared_memory);
  ~TextureCache();

  bool Initialize(bool edram_rov_used);
  void Shutdown();
  void ClearCache();

  void TextureFetchConstantWritten(uint32_t index);

  void BeginFrame();
  void EndFrame();

  // Must be called within a frame - creates and untiles textures needed by
  // shaders and puts them in the SRV state. This may bind compute pipelines
  // (notifying the command processor about that), so this must be called before
  // binding the actual drawing pipeline.
  void RequestTextures(uint32_t used_texture_mask);

  // "ActiveTexture" means as of the latest RequestTextures call.

  // Returns whether texture SRV keys stored externally are still valid for the
  // current bindings and host shader binding layout. Both keys and
  // host_shader_bindings must have host_shader_binding_count elements
  // (otherwise they are incompatible - like if this function returned false).
  bool AreActiveTextureSRVKeysUpToDate(
      const TextureSRVKey* keys,
      const D3D12Shader::TextureBinding* host_shader_bindings,
      size_t host_shader_binding_count) const;
  // Exports the current binding data to texture SRV keys so they can be stored
  // for checking whether subsequent draw calls can keep using the same
  // bindings. Write host_shader_binding_count keys.
  void WriteActiveTextureSRVKeys(
      TextureSRVKey* keys,
      const D3D12Shader::TextureBinding* host_shader_bindings,
      size_t host_shader_binding_count) const;
  // Returns the post-swizzle signedness of a currently bound texture (must be
  // called after RequestTextures).
  uint8_t GetActiveTextureSwizzledSigns(uint32_t index) const {
    return texture_bindings_[index].swizzled_signs;
  }
  void WriteActiveTextureBindfulSRV(
      const D3D12Shader::TextureBinding& host_shader_binding,
      D3D12_CPU_DESCRIPTOR_HANDLE handle);
  uint32_t GetActiveTextureBindlessSRVIndex(
      const D3D12Shader::TextureBinding& host_shader_binding);

  SamplerParameters GetSamplerParameters(
      const D3D12Shader::SamplerBinding& binding) const;
  void WriteSampler(SamplerParameters parameters,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  void MarkRangeAsResolved(uint32_t start_unscaled, uint32_t length_unscaled);

  bool IsResolutionScale2X() const { return scaled_resolve_buffer_ != nullptr; }
  ID3D12Resource* GetScaledResolveBuffer() const {
    return scaled_resolve_buffer_;
  }
  // Ensures the buffer tiles backing the range are resident.
  bool EnsureScaledResolveBufferResident(uint32_t start_unscaled,
                                         uint32_t length_unscaled);
  void UseScaledResolveBufferForReading();
  void UseScaledResolveBufferForWriting();
  void MarkScaledResolveBufferUAVWritesCommitNeeded() {
    if (scaled_resolve_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      scaled_resolve_buffer_uav_writes_commit_needed_ = true;
    }
  }
  // Can't address more than 512 MB on Nvidia, so an offset is required.
  void CreateScaledResolveBufferUintPow2UAV(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                            uint32_t guest_address_bytes,
                                            uint32_t guest_length_bytes,
                                            uint32_t element_size_bytes_pow2);

  // Returns the ID3D12Resource of the front buffer texture (in
  // PIXEL_SHADER_RESOURCE state), or nullptr in case of failure, and writes the
  // description of its SRV. May call LoadTextureData, so the same restrictions
  // (such as about descriptor heap change possibility) apply.
  ID3D12Resource* RequestSwapTexture(
      D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out,
      xenos::TextureFormat& format_out);

 private:
  enum class LoadMode {
    k8bpb,
    k16bpb,
    k32bpb,
    k64bpb,
    k128bpb,
    kR5G5B5A1ToB5G5R5A1,
    kR5G6B5ToB5G6R5,
    kR5G5B6ToB5G6R5WithRBGASwizzle,
    kR4G4B4A4ToB4G4R4A4,
    kR10G11B11ToRGBA16,
    kR10G11B11ToRGBA16SNorm,
    kR11G11B10ToRGBA16,
    kR11G11B10ToRGBA16SNorm,
    kDXT1ToRGBA8,
    kDXT3ToRGBA8,
    kDXT5ToRGBA8,
    kDXNToRG8,
    kDXT3A,
    kDXT3AAs1111,
    kDXT5AToR8,
    kCTX1,
    kDepthUnorm,
    kDepthFloat,

    kCount,

    kUnknown = kCount
  };

  struct LoadModeInfo {
    const void* shader;
    size_t shader_size;
    // Log2 of the sizes, in bytes, of the source (guest) SRV and the
    // destination (host) UAV accessed by the copying shader, since the shader
    // may copy multiple blocks per one invocation.
    uint32_t srv_bpe_log2;
    uint32_t uav_bpe_log2;
    // Optional shader for loading 2x-scaled resolve targets.
    const void* shader_2x;
    size_t shader_2x_size;
    uint32_t srv_bpe_log2_2x;
    uint32_t uav_bpe_log2_2x;
  };

  struct HostFormat {
    // Format info for the regular case.
    // DXGI format (typeless when different signedness or number representation
    // is used) for the texture resource.
    DXGI_FORMAT dxgi_format_resource;
    // DXGI format for unsigned normalized or unsigned/signed float SRV.
    DXGI_FORMAT dxgi_format_unorm;
    // The regular load mode, used when special modes (like signed-specific or
    // decompressing) aren't needed.
    LoadMode load_mode;
    // DXGI format for signed normalized or unsigned/signed float SRV.
    DXGI_FORMAT dxgi_format_snorm;
    // If the signed version needs a different bit representation on the host,
    // this is the load mode for the signed version. Otherwise the regular
    // load_mode will be used for the signed version, and a single copy will be
    // created if both unsigned and signed are used.
    LoadMode load_mode_snorm;

    // Do NOT add integer DXGI formats to this - they are not filterable, can
    // only be read with Load, not Sample! If any game is seen using num_format
    // 1 for fixed-point formats (for floating-point, it's normally set to 1
    // though), add a constant buffer containing multipliers for the
    // textures and multiplication to the tfetch implementation.

    // Whether the DXGI format, if not uncompressing the texture, consists of
    // blocks, thus copy regions must be aligned to block size.
    bool dxgi_format_block_aligned;
    // Uncompression info for when the regular host format for this texture is
    // block-compressed, but the size is not block-aligned, and thus such
    // texture cannot be created in Direct3D on PC and needs decompression,
    // however, such textures are common, for instance, in Halo 3. This only
    // supports unsigned normalized formats - let's hope GPUSIGN_SIGNED was not
    // used for DXN and DXT5A.
    DXGI_FORMAT dxgi_format_uncompressed;
    LoadMode decompress_mode;

    // Mapping of Xenos swizzle components to DXGI format components.
    uint8_t swizzle[4];
  };

  struct Texture {
    TextureKey key;
    ID3D12Resource* resource;
    uint64_t resource_size;
    D3D12_RESOURCE_STATES state;

    uint64_t last_usage_frame;
    uint64_t last_usage_time;
    Texture* used_previous;
    Texture* used_next;

    // Byte size of the top guest mip level.
    uint32_t base_size;
    // Byte size of mips between 1 and key.mip_max_level, containing all array
    // slices.
    uint32_t mip_size;
    // Offsets of all the array slices on a mip level relative to mips_address
    // (0 for mip 0, it's relative to base_address then, and for mip 1).
    uint32_t mip_offsets[14];
    // Byte sizes of an array slice on each mip level.
    uint32_t slice_sizes[14];
    // Row pitches on each mip level (for linear layout mainly).
    uint32_t pitches[14];

    // For bindful - indices in the non-shader-visible descriptor cache for
    // copying to the shader-visible heap (much faster than recreating, which,
    // according to profiling, was often a bottleneck in many games).
    // For bindless - indices in the global shader-visible descriptor heap.
    std::unordered_map<uint32_t, uint32_t> srv_descriptors;

    // These are to be accessed within the global critical region to synchronize
    // with shared memory.
    // Watch handles for the memory ranges.
    SharedMemory::WatchHandle base_watch_handle;
    SharedMemory::WatchHandle mip_watch_handle;
    // Whether the recent base level data has been loaded from the memory.
    bool base_in_sync;
    // Whether the recent mip data has been loaded from the memory.
    bool mips_in_sync;
  };

  struct SRVDescriptorCachePage {
    static constexpr uint32_t kHeapSize = 65536;
    ID3D12DescriptorHeap* heap;
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start;
  };

  struct LoadConstants {
    // vec4 0.
    // Base offset in bytes.
    uint32_t guest_base;
    // For linear textures - row byte pitch.
    uint32_t guest_pitch;
    // In blocks - and for mipmaps, it's also power-of-two-aligned.
    uint32_t guest_storage_width_height[2];

    // vec4 1.
    uint32_t size_blocks[3];
    uint32_t is_3d_endian;

    // vec4 2.
    // Base offset in bytes.
    uint32_t host_base;
    uint32_t host_pitch;
    uint32_t height_texels;

    static constexpr uint32_t kGuestPitchTiled = UINT32_MAX;
  };

  struct TextureBinding {
    TextureKey key;
    // Destination swizzle merged with guest->host format swizzle.
    uint32_t host_swizzle;
    // Packed TextureSign values, 2 bit per each component, with guest-side
    // destination swizzle from the fetch constant applied to them.
    uint8_t swizzled_signs;
    // Unsigned version of the texture (or signed if they have the same data).
    Texture* texture;
    // Signed version of the texture if the data in the signed version is
    // different on the host.
    Texture* texture_signed;
    // Descriptor indices of texture and texture_signed returned from
    // FindOrCreateTextureDescriptor.
    uint32_t descriptor_index;
    uint32_t descriptor_index_signed;
    void Clear() {
      std::memset(this, 0, sizeof(*this));
      descriptor_index = descriptor_index_signed = UINT32_MAX;
    }
  };

  // Whether the signed version of the texture has a different representation on
  // the host than its unsigned version (for example, if it's a fixed-point
  // texture emulated with a larger host pixel format).
  static bool IsSignedVersionSeparate(xenos::TextureFormat format) {
    const HostFormat& host_format = host_formats_[uint32_t(format)];
    return host_format.load_mode_snorm != LoadMode::kUnknown &&
           host_format.load_mode_snorm != host_format.load_mode;
  }
  // Whether decompression is needed on the host (Direct3D only allows creation
  // of block-compressed textures with 4x4-aligned dimensions on PC).
  static bool IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width,
                                    uint32_t height);
  static DXGI_FORMAT GetDXGIResourceFormat(xenos::TextureFormat format,
                                           uint32_t width, uint32_t height) {
    const HostFormat& host_format = host_formats_[uint32_t(format)];
    return IsDecompressionNeeded(format, width, height)
               ? host_format.dxgi_format_uncompressed
               : host_format.dxgi_format_resource;
  }
  static DXGI_FORMAT GetDXGIResourceFormat(TextureKey key) {
    return GetDXGIResourceFormat(key.format, key.width, key.height);
  }
  static DXGI_FORMAT GetDXGIUnormFormat(xenos::TextureFormat format,
                                        uint32_t width, uint32_t height) {
    const HostFormat& host_format = host_formats_[uint32_t(format)];
    return IsDecompressionNeeded(format, width, height)
               ? host_format.dxgi_format_uncompressed
               : host_format.dxgi_format_unorm;
  }
  static DXGI_FORMAT GetDXGIUnormFormat(TextureKey key) {
    return GetDXGIUnormFormat(key.format, key.width, key.height);
  }

  static LoadMode GetLoadMode(TextureKey key);

  // Converts a texture fetch constant to a texture key, normalizing and
  // validating the values, or creating an invalid key, and also gets the
  // host swizzle and post-guest-swizzle signedness.
  static void BindingInfoFromFetchConstant(
      const xenos::xe_gpu_texture_fetch_t& fetch, TextureKey& key_out,
      uint32_t* host_swizzle_out, uint8_t* swizzled_signs_out);

  static constexpr bool AreDimensionsCompatible(
      xenos::FetchOpDimension binding_dimension,
      xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  static void LogTextureKeyAction(TextureKey key, const char* action);
  static void LogTextureAction(const Texture* texture, const char* action);

  // Returns nullptr if the key is not supported, but also if couldn't create
  // the texture - if it's nullptr, occasionally a recreation attempt should be
  // made.
  Texture* FindOrCreateTexture(TextureKey key);

  // Writes data from the shared memory to the texture. This binds pipelines,
  // allocates descriptors and copies!
  bool LoadTextureData(Texture* texture);

  // Returns the index of an existing of a newly created non-shader-visible
  // cached (for bindful) or a shader-visible global (for bindless) descriptor,
  // or UINT32_MAX if failed to create.
  uint32_t FindOrCreateTextureDescriptor(Texture& texture, bool is_signed,
                                         uint32_t host_swizzle);
  D3D12_CPU_DESCRIPTOR_HANDLE GetTextureDescriptorCPUHandle(
      uint32_t descriptor_index) const;

  // For LRU caching - updates the last usage frame and moves the texture to
  // the end of the usage queue. Must be called any time the texture is
  // referenced by any command list to make sure it's not destroyed while still
  // in use.
  void MarkTextureUsed(Texture* texture);

  // Shared memory callback for texture data invalidation.
  static void WatchCallbackThunk(void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu);
  void WatchCallback(Texture* texture, bool is_mip);

  // Makes all bindings invalid. Also requesting textures after calling this
  // will cause another attempt to create a texture or to untile it if there was
  // an error.
  void ClearBindings();

  // Checks if there are any pages that contain scaled resolve data within the
  // range.
  bool IsRangeScaledResolved(uint32_t start_unscaled, uint32_t length_unscaled);
  // Global shared memory invalidation callback for invalidating scaled resolved
  // texture data.
  static void ScaledResolveGlobalWatchCallbackThunk(void* context,
                                                    uint32_t address_first,
                                                    uint32_t address_last,
                                                    bool invalidated_by_gpu);
  void ScaledResolveGlobalWatchCallback(uint32_t address_first,
                                        uint32_t address_last,
                                        bool invalidated_by_gpu);

  static const HostFormat host_formats_[64];

  static const char* const dimension_names_[4];

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  bool bindless_resources_used_;
  D3D12SharedMemory& shared_memory_;

  static const LoadModeInfo load_mode_info_[];
  ID3D12RootSignature* load_root_signature_ = nullptr;
  ID3D12PipelineState* load_pipelines_[size_t(LoadMode::kCount)] = {};
  // Load pipelines for 2x-scaled resolved targets.
  ID3D12PipelineState* load_pipelines_2x_[size_t(LoadMode::kCount)] = {};

  std::unordered_multimap<uint64_t, Texture*> textures_;
  uint64_t textures_total_size_ = 0;
  Texture* texture_used_first_ = nullptr;
  Texture* texture_used_last_ = nullptr;
  uint64_t texture_current_usage_time_;

  std::vector<SRVDescriptorCachePage> srv_descriptor_cache_;
  uint32_t srv_descriptor_cache_allocated_;
  // Indices of cached descriptors used by deleted textures, for reuse.
  std::vector<uint32_t> srv_descriptor_cache_free_;

  enum class NullSRVDescriptorIndex {
    k2DArray,
    k3D,
    kCube,

    kCount,
  };
  // Contains null SRV descriptors of dimensions from NullSRVDescriptorIndex.
  // For copying, not shader-visible.
  ID3D12DescriptorHeap* null_srv_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE null_srv_descriptor_heap_start_;

  TextureBinding texture_bindings_[32] = {};
  // Bit vector with bits reset on fetch constant writes to avoid parsing fetch
  // constants again and again.
  uint32_t texture_bindings_in_sync_ = 0;

  // Whether a texture has been invalidated (a watch has been triggered), so
  // need to try to reload textures, disregarding whether fetch constants have
  // been changed.
  std::atomic<bool> texture_invalidated_ = false;

  // Unsupported texture formats used during this frame (for research and
  // testing).
  enum : uint8_t {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  uint8_t unsupported_format_features_used_[64];

  // The 2 GB tiled buffer for resolved data with 2x resolution scale.
  static constexpr uint32_t kScaledResolveBufferSizeLog2 = 31;
  static constexpr uint32_t kScaledResolveBufferSize =
      1u << kScaledResolveBufferSizeLog2;
  ID3D12Resource* scaled_resolve_buffer_ = nullptr;
  D3D12_RESOURCE_STATES scaled_resolve_buffer_state_ =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool scaled_resolve_buffer_uav_writes_commit_needed_ = false;
  // Not very big heaps (16 MB) because they are needed pretty sparsely. One
  // scaled 1280x720x32bpp texture is slighly bigger than 14 MB.
  static constexpr uint32_t kScaledResolveHeapSizeLog2 = 24;
  static constexpr uint32_t kScaledResolveHeapSize =
      1 << kScaledResolveHeapSizeLog2;
  static_assert(
      (kScaledResolveHeapSize % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) == 0,
      "Scaled resolve heap size must be a multiple of Direct3D tile size");
  // Resident portions of the tiled buffer.
  ID3D12Heap* scaled_resolve_heaps_[kScaledResolveBufferSize >>
                                    kScaledResolveHeapSizeLog2] = {};
  // Number of currently resident portions of the tiled buffer, for profiling.
  uint32_t scaled_resolve_heap_count_ = 0;
  // Global watch for scaled resolve data invalidation.
  SharedMemory::GlobalWatchHandle scaled_resolve_global_watch_handle_ = nullptr;

  xe::global_critical_region global_critical_region_;
  // Bit vector storing whether each 4 KB physical memory page contains scaled
  // resolve data. uint32_t rather than uint64_t because parts of it are sent to
  // shaders.
  uint32_t* scaled_resolve_pages_ = nullptr;
  // Second level of the bit vector for faster rejection of non-scaled textures.
  uint64_t scaled_resolve_pages_l2_[(512 << 20) >> (12 + 5 + 6)];
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_TEXTURE_CACHE_H_
