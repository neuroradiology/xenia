/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/texture_util.h"

#include <algorithm>

#include "xenia/base/assert.h"
#include "xenia/base/math.h"

namespace xe {
namespace gpu {
namespace texture_util {

void GetSubresourcesFromFetchConstant(
    const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t* width_out,
    uint32_t* height_out, uint32_t* depth_or_faces_out, uint32_t* base_page_out,
    uint32_t* mip_page_out, uint32_t* mip_min_level_out,
    uint32_t* mip_max_level_out, xenos::TextureFilter sampler_mip_filter) {
  uint32_t width = 0, height = 0, depth_or_faces = 0;
  switch (fetch.dimension) {
    case xenos::DataDimension::k1D:
      assert_false(fetch.stacked);
      assert_false(fetch.tiled);
      assert_false(fetch.packed_mips);
      width = fetch.size_1d.width;
      break;
    case xenos::DataDimension::k2DOrStacked:
      width = fetch.size_2d.width;
      height = fetch.size_2d.height;
      depth_or_faces = fetch.stacked ? fetch.size_2d.stack_depth : 0;
      break;
    case xenos::DataDimension::k3D:
      assert_false(fetch.stacked);
      width = fetch.size_3d.width;
      height = fetch.size_3d.height;
      depth_or_faces = fetch.size_3d.depth;
      break;
    case xenos::DataDimension::kCube:
      assert_false(fetch.stacked);
      assert_true(fetch.size_2d.stack_depth == 5);
      width = fetch.size_2d.width;
      height = fetch.size_2d.height;
      depth_or_faces = 5;
      break;
  }
  ++width;
  ++height;
  ++depth_or_faces;
  if (width_out) {
    *width_out = width;
  }
  if (height_out) {
    *height_out = height;
  }
  if (depth_or_faces_out) {
    *depth_or_faces_out = depth_or_faces;
  }

  uint32_t longest_axis = std::max(width, height);
  if (fetch.dimension == xenos::DataDimension::k3D) {
    longest_axis = std::max(longest_axis, depth_or_faces);
  }
  uint32_t size_mip_max_level = xe::log2_floor(longest_axis);
  xenos::TextureFilter mip_filter =
      sampler_mip_filter == xenos::TextureFilter::kUseFetchConst
          ? fetch.mip_filter
          : sampler_mip_filter;

  uint32_t base_page = fetch.base_address & 0x1FFFF;
  uint32_t mip_page = fetch.mip_address & 0x1FFFF;

  uint32_t mip_min_level, mip_max_level;
  if (mip_filter == xenos::TextureFilter::kBaseMap || mip_page == 0) {
    mip_min_level = 0;
    mip_max_level = 0;
  } else {
    mip_min_level = std::min(fetch.mip_min_level, size_mip_max_level);
    mip_max_level = std::max(std::min(fetch.mip_max_level, size_mip_max_level),
                             mip_min_level);
  }
  if (mip_max_level != 0) {
    // Special case for streaming. Games such as Banjo-Kazooie: Nuts & Bolts
    // specify the same address for both the base level and the mips and set
    // mip_min_index to 1 until the texture is actually loaded - this is the way
    // recommended by a GPU hang error message found in game executables. In
    // this case we assume that the base level is not loaded yet.
    if (base_page == mip_page) {
      base_page = 0;
    }
    if (base_page == 0) {
      mip_min_level = std::max(mip_min_level, uint32_t(1));
    }
    if (mip_min_level != 0) {
      base_page = 0;
    }
  } else {
    mip_page = 0;
  }

  if (base_page_out) {
    *base_page_out = base_page;
  }
  if (mip_page_out) {
    *mip_page_out = mip_page;
  }
  if (mip_min_level_out) {
    *mip_min_level_out = mip_min_level;
  }
  if (mip_max_level_out) {
    *mip_max_level_out = mip_max_level;
  }
}

void GetGuestMipBlocks(xenos::DataDimension dimension, uint32_t width,
                       uint32_t height, uint32_t depth,
                       xenos::TextureFormat format, uint32_t mip,
                       uint32_t& width_blocks_out, uint32_t& height_blocks_out,
                       uint32_t& depth_blocks_out) {
  // Get mipmap size.
  if (mip != 0) {
    width = std::max(xe::next_pow2(width) >> mip, uint32_t(1));
    if (dimension != xenos::DataDimension::k1D) {
      height = std::max(xe::next_pow2(height) >> mip, uint32_t(1));
      if (dimension == xenos::DataDimension::k3D) {
        depth = std::max(xe::next_pow2(depth) >> mip, uint32_t(1));
      }
    }
  }

  // Get the size in blocks rather than in pixels.
  const FormatInfo* format_info = FormatInfo::Get(format);
  width = xe::align(width, format_info->block_width) / format_info->block_width;
  height =
      xe::align(height, format_info->block_height) / format_info->block_height;

  // Align to tiles.
  width_blocks_out = xe::align(width, xenos::kTextureTileWidthHeight);
  if (dimension != xenos::DataDimension::k1D) {
    height_blocks_out = xe::align(height, xenos::kTextureTileWidthHeight);
  } else {
    height_blocks_out = 1;
  }
  if (dimension == xenos::DataDimension::k3D) {
    depth_blocks_out = xe::align(depth, xenos::kTextureTiledDepthGranularity);
  } else {
    depth_blocks_out = 1;
  }
}

uint32_t GetGuestMipSliceStorageSize(uint32_t width_blocks,
                                     uint32_t height_blocks,
                                     uint32_t depth_blocks, bool is_tiled,
                                     xenos::TextureFormat format,
                                     uint32_t* row_pitch_out, bool align_4kb) {
  const FormatInfo* format_info = FormatInfo::Get(format);
  uint32_t row_pitch = width_blocks * format_info->block_width *
                       format_info->block_height * format_info->bits_per_pixel /
                       8;
  if (!is_tiled) {
    row_pitch = xe::align(row_pitch, xenos::kTextureLinearRowAlignmentBytes);
  }
  if (row_pitch_out != nullptr) {
    *row_pitch_out = row_pitch;
  }
  uint32_t size = row_pitch * height_blocks * depth_blocks;
  if (align_4kb) {
    size = xe::align(size, xenos::kTextureSubresourceAlignmentBytes);
  }
  return size;
}

bool GetPackedMipOffset(uint32_t width, uint32_t height, uint32_t depth,
                        xenos::TextureFormat format, uint32_t mip,
                        uint32_t& x_blocks, uint32_t& y_blocks,
                        uint32_t& z_blocks) {
  // Tile size is 32x32, and once textures go <=16 they are packed into a
  // single tile together. The math here is insane. Most sourced from
  // graph paper, looking at dds dumps and executable reverse engineering.
  //   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // 0         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 1         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 2         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 3         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 4 x               +.....8x8.....+ +............16x16............+
  // 5                 +.....8x8.....+ +............16x16............+
  // 6                 +.....8x8.....+ +............16x16............+
  // 7                 +.....8x8.....+ +............16x16............+
  // 8 2x2                             +............16x16............+
  // 9 2x2                             +............16x16............+
  // 0                                 +............16x16............+
  // ...                                            .....
  //
  // The 2x2 and 1x1 squares are packed in their specific positions because
  // each square is the size of at least one block (which is 4x4 pixels max)
  //
  // if (tile_aligned(w) > tile_aligned(h)) {
  //   // wider than tall, so packed horizontally
  // } else if (tile_aligned(w) < tile_aligned(h)) {
  //   // taller than wide, so packed vertically
  // } else {
  //   square
  // }
  // It's important to use logical sizes here, as the input sizes will be
  // for the entire packed tile set, not the actual texture.
  // The minimum dimension is what matters most: if either width or height
  // is <= 16 this mode kicks in.

  uint32_t log2_width = xe::log2_ceil(width);
  uint32_t log2_height = xe::log2_ceil(height);
  uint32_t log2_size = std::min(log2_width, log2_height);
  if (log2_size > 4 + mip) {
    // The shortest dimension is bigger than 16, not packed.
    x_blocks = 0;
    y_blocks = 0;
    z_blocks = 0;
    return false;
  }
  uint32_t packed_mip_base = (log2_size > 4) ? (log2_size - 4) : 0;
  uint32_t packed_mip = mip - packed_mip_base;

  // Find the block offset of the mip.
  if (packed_mip < 3) {
    if (log2_width > log2_height) {
      // Wider than tall. Laid out vertically.
      x_blocks = 0;
      y_blocks = 16 >> packed_mip;
    } else {
      // Taller than wide. Laid out horizontally.
      x_blocks = 16 >> packed_mip;
      y_blocks = 0;
    }
    z_blocks = 0;
  } else {
    uint32_t offset;
    if (log2_width > log2_height) {
      // Wider than tall. Laid out horizontally.
      offset = (1 << (log2_width - packed_mip_base)) >> (packed_mip - 2);
      x_blocks = offset;
      y_blocks = 0;
    } else {
      // Taller than wide. Laid out vertically.
      x_blocks = 0;
      offset = (1 << (log2_height - packed_mip_base)) >> (packed_mip - 2);
      y_blocks = offset;
    }
    if (offset < 4) {
      // Pack 1x1 Z mipmaps along Z - not reached for 2D.
      uint32_t log2_depth = xe::log2_ceil(depth);
      if (log2_depth > 1 + packed_mip) {
        z_blocks = (log2_depth - packed_mip) * 4;
      } else {
        z_blocks = 4;
      }
    } else {
      z_blocks = 0;
    }
  }

  const FormatInfo* format_info = FormatInfo::Get(format);
  x_blocks /= format_info->block_width;
  y_blocks /= format_info->block_height;
  return true;
}

void GetTextureTotalSize(xenos::DataDimension dimension, uint32_t width,
                         uint32_t height, uint32_t depth,
                         xenos::TextureFormat format, bool is_tiled,
                         bool packed_mips, uint32_t mip_max_level,
                         uint32_t* base_size_out, uint32_t* mip_size_out) {
  bool is_3d = dimension == xenos::DataDimension::k3D;
  uint32_t width_blocks, height_blocks, depth_blocks;
  if (base_size_out) {
    GetGuestMipBlocks(dimension, width, height, depth, format, 0, width_blocks,
                      height_blocks, depth_blocks);
    uint32_t size = GetGuestMipSliceStorageSize(
        width_blocks, height_blocks, depth_blocks, is_tiled, format, nullptr);
    if (!is_3d) {
      size *= depth;
    }
    *base_size_out = size;
  }
  if (mip_size_out) {
    uint32_t size = 0;
    uint32_t longest_axis = std::max(width, height);
    if (is_3d) {
      longest_axis = std::max(longest_axis, depth);
    }
    mip_max_level = std::min(mip_max_level, xe::log2_floor(longest_axis));
    if (mip_max_level) {
      // If the texture is very small, its packed mips may be stored at level 0.
      uint32_t mip_packed =
          packed_mips ? GetPackedMipLevel(width, height) : UINT32_MAX;
      for (uint32_t i = std::min(uint32_t(1), mip_packed);
           i <= std::min(mip_max_level, mip_packed); ++i) {
        GetGuestMipBlocks(dimension, width, height, depth, format, i,
                          width_blocks, height_blocks, depth_blocks);
        uint32_t level_size = GetGuestMipSliceStorageSize(
            width_blocks, height_blocks, depth_blocks, is_tiled, format,
            nullptr);
        if (!is_3d) {
          level_size *= depth;
        }
        size += level_size;
      }
    }
    *mip_size_out = size;
  }
}

int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t width,
                         uint32_t bpb_log2) {
  // https://github.com/gildor2/UModel/blob/de8fbd3bc922427ea056b7340202dcdcc19ccff5/Unreal/UnTexture.cpp#L489
  width = xe::align(width, xenos::kTextureTileWidthHeight);
  // Top bits of coordinates.
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(width >> 5)) << (bpb_log2 + 7);
  // Lower bits of coordinates (result is 6-bit value).
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  // Mix micro/macro + add few remaining x/y bits.
  int32_t offset =
      macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  // Mix bits again.
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t GetTiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t width,
                         uint32_t height, uint32_t bpb_log2) {
  // Reconstructed from disassembly of XGRAPHICS::TileVolume.
  width = xe::align(width, xenos::kTextureTileWidthHeight);
  height = xe::align(height, xenos::kTextureTileWidthHeight);
  int32_t macro_outer =
      ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(width >> 5);
  int32_t macro = ((((x >> 5) + macro_outer) << (bpb_log2 + 6)) & 0xFFFFFFF)
                  << 1;
  int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bpb_log2 + 6)) >> 6;
  int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int32_t offset1 =
      offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                    ((z & 3) << (bpb_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

uint8_t SwizzleSigns(const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint8_t signs = 0;
  bool any_not_signed = false, any_signed = false;
  // 0b00 or 0b01 for each component, whether it's constant 0/1.
  uint8_t constant_mask = 0b00000000;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t swizzle = (fetch.swizzle >> (i * 3)) & 0b111;
    if (swizzle & 0b100) {
      constant_mask |= uint8_t(1) << (i * 2);
    } else {
      xenos::TextureSign sign =
          xenos::TextureSign((fetch.dword_0 >> (2 + swizzle * 2)) & 0b11);
      signs |= uint8_t(sign) << (i * 2);
      if (sign == xenos::TextureSign::kSigned) {
        any_signed = true;
      } else {
        any_not_signed = true;
      }
    }
  }
  xenos::TextureSign constants_sign = xenos::TextureSign::kUnsigned;
  if (constant_mask == 0b01010101) {
    // If only constant components, choose according to the original format
    // (what would more likely be loaded if there were non-constant components).
    // If all components would be signed, use signed.
    if (((fetch.dword_0 >> 2) & 0b11111111) ==
        uint32_t(xenos::TextureSign::kSigned) * 0b01010101) {
      constants_sign = xenos::TextureSign::kSigned;
    }
  } else {
    // If only signed and constant components, reading just from the signed host
    // view is enough.
    if (any_signed && !any_not_signed) {
      constants_sign = xenos::TextureSign::kSigned;
    }
  }
  signs |= uint8_t(constants_sign) * constant_mask;
  return signs;
}

}  // namespace texture_util
}  // namespace gpu
}  // namespace xe
