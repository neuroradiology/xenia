/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_XENOS_H_
#define XENIA_GPU_XENOS_H_

#include <algorithm>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"

namespace xe {
namespace gpu {
namespace xenos {

enum class ShaderType : uint32_t {
  kVertex = 0,
  kPixel = 1,
};

enum class PrimitiveType : uint32_t {
  kNone = 0x00,
  kPointList = 0x01,
  kLineList = 0x02,
  kLineStrip = 0x03,
  kTriangleList = 0x04,
  kTriangleFan = 0x05,
  kTriangleStrip = 0x06,
  kTriangleWithWFlags = 0x07,
  kRectangleList = 0x08,
  kLineLoop = 0x0C,
  kQuadList = 0x0D,
  kQuadStrip = 0x0E,
  kPolygon = 0x0F,

  // Starting with this primitive type, explicit major mode is assumed (in the
  // R6xx/R7xx registers, k2DCopyRectListV0 is 22, and implicit major mode is
  // only used for primitive types 0 through 21) - and tessellation patches use
  // the range that starts from k2DCopyRectListV0.
  // TODO(Triang3l): Verify if this is also true for the Xenos.
  kExplicitMajorModeForceStart = 0x10,

  k2DCopyRectListV0 = 0x10,
  k2DCopyRectListV1 = 0x11,
  k2DCopyRectListV2 = 0x12,
  k2DCopyRectListV3 = 0x13,
  k2DFillRectList = 0x14,
  k2DLineStrip = 0x15,
  k2DTriStrip = 0x16,

  // Tessellation patches when VGT_OUTPUT_PATH_CNTL::path_select is
  // VGTOutputPath::kTessellationEnable. The vertex shader receives patch index
  // rather than control point indices.
  kLinePatch = 0x10,
  kTrianglePatch = 0x11,
  kQuadPatch = 0x12,
};

// Polygonal primitive types (not including points and lines) are rasterized as
// triangles, have front and back faces, and also support face culling and fill
// modes (polymode_front_ptype, polymode_back_ptype). Other primitive types are
// always "front" (but don't support front face and back face culling, according
// to OpenGL and Vulkan specifications - even if glCullFace is
// GL_FRONT_AND_BACK, points and lines are still drawn), and may in some cases
// use the "para" registers instead of "front" or "back" (for "parallelogram" -
// like poly_offset_para_enable).
constexpr bool IsPrimitivePolygonal(bool tessellated, PrimitiveType type) {
  if (tessellated && (type == PrimitiveType::kTrianglePatch ||
                      type == PrimitiveType::kQuadPatch)) {
    return true;
  }
  switch (type) {
    case PrimitiveType::kTriangleList:
    case PrimitiveType::kTriangleFan:
    case PrimitiveType::kTriangleStrip:
    case PrimitiveType::kTriangleWithWFlags:
    case PrimitiveType::kQuadList:
    case PrimitiveType::kQuadStrip:
    case PrimitiveType::kPolygon:
      return true;
    default:
      break;
  }
  // TODO(Triang3l): Investigate how kRectangleList should be treated - possibly
  // actually drawn as two polygons on the console, however, the current
  // geometry shader doesn't care about the winding order - allowing backface
  // culling for rectangles currently breaks Gears of War 2.
  return false;
}

// For the texture fetch constant (not the tfetch instruction), stacked stored
// as 2D.
enum class DataDimension : uint32_t {
  k1D = 0,
  k2DOrStacked = 1,
  k3D = 2,
  kCube = 3,
};

enum class ClampMode : uint32_t {
  kRepeat = 0,
  kMirroredRepeat = 1,
  kClampToEdge = 2,
  kMirrorClampToEdge = 3,
  kClampToHalfway = 4,
  kMirrorClampToHalfway = 5,
  kClampToBorder = 6,
  kMirrorClampToBorder = 7,
};

// TEX_FORMAT_COMP, known as GPUSIGN on the Xbox 360.
enum class TextureSign : uint32_t {
  kUnsigned = 0,
  // Two's complement texture data.
  kSigned = 1,
  // 2*color-1 - https://xboxforums.create.msdn.com/forums/t/107374.aspx
  kUnsignedBiased = 2,
  // Linearized when sampled.
  kGamma = 3,
};

enum class TextureFilter : uint32_t {
  kPoint = 0,
  kLinear = 1,
  kBaseMap = 2,  // Only applicable for mip-filter - always fetch from level 0.
  kUseFetchConst = 3,
};

enum class AnisoFilter : uint32_t {
  kDisabled = 0,
  kMax_1_1 = 1,
  kMax_2_1 = 2,
  kMax_4_1 = 3,
  kMax_8_1 = 4,
  kMax_16_1 = 5,
  kUseFetchConst = 7,
};

enum class BorderColor : uint32_t {
  k_AGBR_Black = 0,
  k_AGBR_White = 1,
  k_ACBYCR_BLACK = 2,
  k_ACBCRY_BLACK = 3,
};

// For the tfetch instruction (not the fetch constant) and related instructions,
// stacked accessed using tfetch3D.
enum class FetchOpDimension : uint32_t {
  k1D = 0,
  k2D = 1,
  k3DOrStacked = 2,
  kCube = 3,
};

inline int GetFetchOpDimensionComponentCount(FetchOpDimension dimension) {
  switch (dimension) {
    case FetchOpDimension::k1D:
      return 1;
    case FetchOpDimension::k2D:
      return 2;
    case FetchOpDimension::k3DOrStacked:
    case FetchOpDimension::kCube:
      return 3;
    default:
      assert_unhandled_case(dimension);
      return 1;
  }
}

enum class SampleLocation : uint32_t {
  kCentroid = 0,
  kCenter = 1,
};

enum class Endian : uint32_t {
  kNone = 0,
  k8in16 = 1,
  k8in32 = 2,
  k16in32 = 3,
};

enum class Endian128 : uint32_t {
  kNone = 0,
  k8in16 = 1,
  k8in32 = 2,
  k16in32 = 3,
  k8in64 = 4,
  k8in128 = 5,
};

enum class IndexFormat : uint32_t {
  kInt16,
  kInt32,
};

// SurfaceNumberX from yamato_enum.h.
enum class SurfaceNumFormat : uint32_t {
  kUnsignedRepeatingFraction = 0,
  // Microsoft-style, scale factor (2^(n-1))-1.
  kSignedRepeatingFraction = 1,
  kUnsignedInteger = 2,
  kSignedInteger = 3,
  kFloat = 7,
};

// The EDRAM is an opaque block of memory accessible by the RB pipeline stage of
// the GPU, which performs output-merger functionality (color render target
// writing and blending, depth and stencil testing) and resolve (copy)
// operations.
//
// Data in the 10 MiB of EDRAM is laid out as 2048 tiles on 80x16 32bpp MSAA
// samples. With 2x MSAA, one pixel consists of 1x2 samples, and with 4x, it
// consists of 2x2 samples. Thus, for a 32bpp render target, one tile contains
// 80x16 pixels without MSAA, samples of 80x8 pixels with 2x MSAA, or samples of
// 40x8 pixels with 4x MSAA. The base is specified in tiles, the pitch is also
// treated as tiles (so a 256x single-sampled surface will be stored in the
// EDRAM as 320x).
//
// XGSurfaceSize code in game executables calculates the size in tiles in the
// following order:
// 1) If MSAA is >=2x, multiply the height by 2.
// 2) If MSAA is 4x, multiply the width by 2.
// 3) 80x16-align width and height in samples.
// 4) Multiply width*height in samples by 4 or 8 depending on the pixel format.
// 5) Divide the byte size by 5120.
// This means that when working with layout of surfaces in the EDRAM, it should
// be assumed that a multisampled surface is the same as a single-sampled
// surface with 2x height and (with 4x MSAA) width - however, format size
// doesn't effect the dimensions, 64bpp surfaces take twice as many tiles as
// 32bpp surfaces.
//
// From this, it follows that the tile row pitch in tiles can be multiplied by
// 64bpp too. In the formula for calculating the tile count:
// (height rounded up to 16) * (width rounded up to 80) * (4 or 8) / 5120
// the fraction can be reduced because the numerator is always divisible by
// 5120 - it changes in 80 * 16 * 4 = 5120 increments - in tile increments -
// resulting in:
// (height in tiles) * (width in tiles) * (1 or 2)
// Here we get only multiplication, which (disregarding the variable size) is
// associative for integers, so:
// ((height in tiles) * (width in tiles)) * (1 or 2)
// is identical to:
// (height in tiles) * ((width in tiles) * (1 or 2))
//
// Depth surfaces are also stored as 32bpp tiles, however, as opposed to color
// surfaces, 40x16-sample halves of each tile are swapped - game shaders (for
// example, in GTA IV, Halo 3) perform this swapping when writing specific
// depth/stencil values by drawing to a depth buffer's memory through a color
// render target (to reupload a depth/stencil surface previously evicted from
// the EDRAM to the main memory, for instance).

enum class MsaaSamples : uint32_t {
  k1X = 0,
  k2X = 1,
  k4X = 2,
};

constexpr uint32_t kMsaaSamplesBits = 2;

constexpr uint32_t kMaxColorRenderTargets = 4;

enum class ColorRenderTargetFormat : uint32_t {
  k_8_8_8_8 = 0,
  k_8_8_8_8_GAMMA = 1,
  k_2_10_10_10 = 2,
  // 7e3 [0, 32) RGB, unorm alpha.
  // http://fileadmin.cs.lth.se/cs/Personal/Michael_Doggett/talks/eg05-xenos-doggett.pdf
  k_2_10_10_10_FLOAT = 3,
  // Fixed point -32...32.
  // http://www.students.science.uu.nl/~3220516/advancedgraphics/papers/inferred_lighting.pdf
  k_16_16 = 4,
  // Fixed point -32...32.
  k_16_16_16_16 = 5,
  k_16_16_FLOAT = 6,
  k_16_16_16_16_FLOAT = 7,
  k_2_10_10_10_AS_10_10_10_10 = 10,
  // 16-bit fixed point at half speed, with full blending.
  // http://fileadmin.cs.lth.se/cs/Personal/Michael_Doggett/talks/unc-xenos-doggett.pdf
  k_2_10_10_10_FLOAT_AS_16_16_16_16 = 12,
  k_32_FLOAT = 14,
  k_32_32_FLOAT = 15,
};

const char* GetColorRenderTargetFormatName(ColorRenderTargetFormat format);

constexpr bool IsColorRenderTargetFormat64bpp(ColorRenderTargetFormat format) {
  return format == ColorRenderTargetFormat::k_16_16_16_16 ||
         format == ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
         format == ColorRenderTargetFormat::k_32_32_FLOAT;
}

inline uint32_t GetColorRenderTargetFormatComponentCount(
    ColorRenderTargetFormat format) {
  switch (format) {
    case ColorRenderTargetFormat::k_32_FLOAT:
      return 1;
    case ColorRenderTargetFormat::k_16_16:
    case ColorRenderTargetFormat::k_16_16_FLOAT:
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return 2;
    default:
      return 4;
  }
}

enum class DepthRenderTargetFormat : uint32_t {
  kD24S8 = 0,
  // 20e4 [0, 2).
  kD24FS8 = 1,
};

const char* GetDepthRenderTargetFormatName(DepthRenderTargetFormat format);

// Converts an IEEE-754 32-bit floating-point number to Xenos floating-point
// depth, rounding to the nearest even.
uint32_t Float32To20e4(float f32);
// Converts Xenos floating-point depth in bits 0:23 (not clamping) to an
// IEEE-754 32-bit floating-point number.
float Float20e4To32(uint32_t f24);

constexpr uint32_t kColorRenderTargetFormatBits = 4;
constexpr uint32_t kDepthRenderTargetFormatBits = 1;
constexpr uint32_t kRenderTargetFormatBits =
    std::max(kColorRenderTargetFormatBits, kDepthRenderTargetFormatBits);

constexpr uint32_t kEdramTileWidthSamples = 80;
constexpr uint32_t kEdramTileHeightSamples = 16;
constexpr uint32_t kEdramTileCount = 2048;
constexpr uint32_t kEdramSizeBytes = kEdramTileCount * kEdramTileHeightSamples *
                                     kEdramTileWidthSamples * sizeof(uint32_t);

// RB_SURFACE_INFO::surface_pitch width.
constexpr uint32_t kEdramPitchPixelsBits = 14;
// RB_COLOR_INFO::color_base/RB_DEPTH_INFO::depth_base width (though for the
// Xbox 360 only 11 make sense, but to avoid bounds checks).
constexpr uint32_t kEdramBaseTilesBits = 12;

inline uint32_t GetSurfacePitchTiles(uint32_t pitch_pixels,
                                     MsaaSamples msaa_samples, bool is_64bpp) {
  uint32_t pitch_samples = pitch_pixels
                           << uint32_t(msaa_samples >= MsaaSamples::k4X);
  uint32_t pitch_tiles =
      (pitch_samples + (kEdramTileWidthSamples - 1)) / kEdramTileWidthSamples;
  if (is_64bpp) {
    pitch_tiles <<= 1;
  }
  return pitch_tiles;
}

// log2_ceil of the maximum value of GetSurfacePitchTiles, assuming 16383 being
// the maximum pitch in pixels (not sure about the validity of values above
// 8192, but to avoid bounds checking).
// log2_ceil of 16383, multiplied by 2 for 4x MSAA, rounded to 80 samples,
// multiplied by 2 for 64bpp.
constexpr uint32_t kEdramPitchTilesBits = 10;

// Returns the maximum height of a render target, in pixels, that may fit in the
// EDRAM starting from the specified base address, until the end of the EDRAM or
// the specified tile (for instance, until the next render target).
inline uint32_t GetMaxRenderTargetHeight(
    uint32_t base_tiles, uint32_t pitch_pixels, MsaaSamples msaa_samples,
    bool is_64bpp, uint32_t until_tile = kEdramTileCount) {
  if (base_tiles >= until_tile) {
    return 0;
  }
  uint32_t pitch_tiles =
      GetSurfacePitchTiles(pitch_pixels, msaa_samples, is_64bpp);
  if (!pitch_tiles) {
    return 0;
  }
  return ((until_tile - base_tiles) / pitch_tiles) *
         (kEdramTileHeightSamples >>
          uint32_t(msaa_samples >= MsaaSamples::k2X));
}

constexpr uint32_t kFormatBits = 6;

// a2xx_sq_surfaceformat +
// https://github.com/indirivacua/RAGE-Console-Texture-Editor/blob/master/Console.Xbox360.Graphics.pas
enum class TextureFormat : uint32_t {
  k_1_REVERSE = 0,
  k_1 = 1,
  k_8 = 2,
  k_1_5_5_5 = 3,
  k_5_6_5 = 4,
  k_6_5_5 = 5,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_8_A = 8,
  k_8_B = 9,
  k_8_8 = 10,
  k_Cr_Y1_Cb_Y0_REP = 11,
  k_Y1_Cr_Y0_Cb_REP = 12,
  k_16_16_EDRAM = 13,
  k_8_8_8_8_A = 14,
  k_4_4_4_4 = 15,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_DXT1 = 18,
  k_DXT2_3 = 19,
  k_DXT4_5 = 20,
  k_16_16_16_16_EDRAM = 21,
  k_24_8 = 22,
  k_24_8_FLOAT = 23,
  k_16 = 24,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_EXPAND = 27,
  k_16_16_EXPAND = 28,
  k_16_16_16_16_EXPAND = 29,
  k_16_FLOAT = 30,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_AS_8 = 39,
  k_32_AS_8_8 = 40,
  k_16_MPEG = 41,
  k_16_16_MPEG = 42,
  k_8_INTERLACED = 43,
  k_32_AS_8_INTERLACED = 44,
  k_32_AS_8_8_INTERLACED = 45,
  k_16_INTERLACED = 46,
  k_16_MPEG_INTERLACED = 47,
  k_16_16_MPEG_INTERLACED = 48,
  k_DXN = 49,
  k_8_8_8_8_AS_16_16_16_16 = 50,
  k_DXT1_AS_16_16_16_16 = 51,
  k_DXT2_3_AS_16_16_16_16 = 52,
  k_DXT4_5_AS_16_16_16_16 = 53,
  k_2_10_10_10_AS_16_16_16_16 = 54,
  k_10_11_11_AS_16_16_16_16 = 55,
  k_11_11_10_AS_16_16_16_16 = 56,
  k_32_32_32_FLOAT = 57,
  k_DXT3A = 58,
  k_DXT5A = 59,
  k_CTX1 = 60,
  k_DXT3A_AS_1_1_1_1 = 61,
  k_8_8_8_8_GAMMA_EDRAM = 62,
  k_2_10_10_10_FLOAT_EDRAM = 63,

  kUnknown = 0xFFFFFFFFu,
};

// Subset of a2xx_sq_surfaceformat - formats that RTs can be resolved to.
enum class ColorFormat : uint32_t {
  k_8 = 2,
  k_1_5_5_5 = 3,
  k_5_6_5 = 4,
  k_6_5_5 = 5,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_8_A = 8,
  k_8_B = 9,
  k_8_8 = 10,
  k_8_8_8_8_A = 14,
  k_4_4_4_4 = 15,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16 = 24,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_FLOAT = 30,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_8_8_8_8_AS_16_16_16_16 = 50,
  k_2_10_10_10_AS_16_16_16_16 = 54,
  k_10_11_11_AS_16_16_16_16 = 55,
  k_11_11_10_AS_16_16_16_16 = 56,
};

// Resolve writes unsigned data for fixed-point formats (so k_16_16 and
// k_16_16_16_16 render target formats, which are signed and also have a
// different range, are not equivalent to the respective texture formats).
constexpr bool IsColorResolveFormatBitwiseEquivalent(
    ColorRenderTargetFormat render_target_format, ColorFormat color_format) {
  switch (render_target_format) {
    case ColorRenderTargetFormat::k_8_8_8_8:
    // Shaders fetch data copied from k_8_8_8_8_GAMMA with TextureSign::kGamma.
    case ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // TODO(Triang3l): Investigate k_8_8_8_8_A.
      return color_format == ColorFormat::k_8_8_8_8 ||
             color_format == ColorFormat::k_8_8_8_8_A ||
             color_format == ColorFormat::k_8_8_8_8_AS_16_16_16_16;
    case ColorRenderTargetFormat::k_2_10_10_10:
    case ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return color_format == ColorFormat::k_2_10_10_10 ||
             color_format == ColorFormat::k_2_10_10_10_AS_16_16_16_16;
    case ColorRenderTargetFormat::k_16_16_FLOAT:
      return color_format == ColorFormat::k_16_16_FLOAT;
    case ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return color_format == ColorFormat::k_16_16_16_16_FLOAT;
    case ColorRenderTargetFormat::k_32_FLOAT:
      return color_format == ColorFormat::k_32_FLOAT;
    case ColorRenderTargetFormat::k_32_32_FLOAT:
      return color_format == ColorFormat::k_32_32_FLOAT;
    default:
      return false;
  }
}

enum class VertexFormat : uint32_t {
  kUndefined = 0,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_32_32_FLOAT = 57,
};

inline int GetVertexFormatComponentCount(VertexFormat format) {
  switch (format) {
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return 1;
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_FLOAT:
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_FLOAT:
      return 2;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
    case VertexFormat::k_32_32_32_FLOAT:
      return 3;
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return 4;
    default:
      assert_unhandled_case(format);
      return 0;
  }
}

inline uint32_t GetVertexFormatNeededWords(VertexFormat format,
                                           uint32_t used_components) {
  assert_zero(used_components & ~uint32_t(0b1111));
  if (!used_components) {
    return 0;
  }
  switch (format) {
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
      return 0b0001;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
      return (used_components & 0b0111) ? 0b0001 : 0b0000;
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_FLOAT:
      return (used_components & 0b0011) ? 0b0001 : 0b0000;
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
      return ((used_components & 0b0011) ? 0b0001 : 0b0000) |
             ((used_components & 0b1100) ? 0b0010 : 0b0000);
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return used_components & 0b0001;
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_FLOAT:
      return used_components & 0b0011;
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return used_components;
    case VertexFormat::k_32_32_32_FLOAT:
      return used_components & 0b0111;
    default:
      assert_unhandled_case(format);
      return 0b0000;
  }
}

enum class CompareFunction : uint32_t {
  kNever = 0b000,
  kLess = 0b001,
  kEqual = 0b010,
  kLessEqual = 0b011,
  kGreater = 0b100,
  kNotEqual = 0b101,
  kGreaterEqual = 0b110,
  kAlways = 0b111,
};

enum class StencilOp : uint32_t {
  kKeep = 0,
  kZero = 1,
  kReplace = 2,
  kIncrementClamp = 3,
  kDecrementClamp = 4,
  kInvert = 5,
  kIncrementWrap = 6,
  kDecrementWrap = 7,
};

// adreno_rb_blend_factor
enum class BlendFactor : uint32_t {
  kZero = 0,
  kOne = 1,
  kSrcColor = 4,
  kOneMinusSrcColor = 5,
  kSrcAlpha = 6,
  kOneMinusSrcAlpha = 7,
  kDstColor = 8,
  kOneMinusDstColor = 9,
  kDstAlpha = 10,
  kOneMinusDstAlpha = 11,
  kConstantColor = 12,
  kOneMinusConstantColor = 13,
  kConstantAlpha = 14,
  kOneMinusConstantAlpha = 15,
  kSrcAlphaSaturate = 16,
  // SRC1 added on Adreno.
};

enum class BlendOp : uint32_t {
  kAdd = 0,
  kSubtract = 1,
  kMin = 2,
  kMax = 3,
  kRevSubtract = 4,
};

typedef enum {
  XE_GPU_INVALIDATE_MASK_VERTEX_SHADER = 1 << 8,
  XE_GPU_INVALIDATE_MASK_PIXEL_SHADER = 1 << 9,

  XE_GPU_INVALIDATE_MASK_ALL = 0x7FFF,
} XE_GPU_INVALIDATE_MASK;

// VGT_DRAW_INITIATOR::DI_SRC_SEL_*
enum class SourceSelect : uint32_t {
  kDMA,
  kImmediate,
  kAutoIndex,
};

// VGT_DRAW_INITIATOR::DI_MAJOR_MODE_*
enum class MajorMode : uint32_t {
  kImplicit,
  kExplicit,
};

inline bool IsMajorModeExplicit(MajorMode major_mode,
                                PrimitiveType primitive_type) {
  return major_mode != MajorMode::kImplicit ||
         primitive_type >= PrimitiveType::kExplicitMajorModeForceStart;
}

enum class SignedRepeatingFractionMode : uint32_t {
  // Microsoft-style representation with two -1 representations (one is slightly
  // past -1 but clamped).
  kZeroClampMinusOne,
  // OpenGL "alternate mapping" format lacking representation for zero.
  kNoZero,
};

// instr_arbitrary_filter_t
enum class ArbitraryFilter : uint32_t {
  k2x4Sym = 0,
  k2x4Asym = 1,
  k4x2Sym = 2,
  k4x2Asym = 3,
  k4x4Sym = 4,
  k4x4Asym = 5,
  kUseFetchConst = 7,
};

// a2xx_sq_ps_vtx_mode
enum class VertexShaderExportMode : uint32_t {
  kPosition1Vector = 0,
  kPosition2VectorsSprite = 2,
  kPosition2VectorsEdge = 3,
  kPosition2VectorsKill = 4,
  kPosition2VectorsSpriteKill = 5,
  kPosition2VectorsEdgeKill = 6,
  // Vertex shader outputs are ignored (kill all primitives) - see
  // SX_MISC::MULTIPASS on R6xx/R7xx.
  kMultipass = 7,
};

constexpr uint32_t kMaxInterpolators = 16;

enum class SampleControl : uint32_t {
  kCentroidsOnly = 0,
  kCentersOnly = 1,
  kCentroidsAndCenters = 2,
};

// - msaa_samples is RB_SURFACE_INFO::msaa_samples.
// - sample_control is SQ_CONTEXT_MISC::sc_sample_cntl.
// - interpolator_control_sampling_pattern is
//   SQ_INTERPOLATOR_CNTL::sampling_pattern.
// Centroid interpolation can be tested in Red Dead Redemption. If the GPU host
// backend implements guest MSAA properly, using host MSAA, with everything
// interpolated at centers, the Diez Coronas start screen background may have
// a few distinctly bright pixels on the mesas/buttes, where extrapolation
// happens. Interpolating certain values (ones that aren't used for gradient
// calculation, not texture coordinates) at centroids fixes this issue.
inline uint32_t GetInterpolatorSamplingPattern(
    MsaaSamples msaa_samples, SampleControl sample_control,
    uint32_t interpolator_control_sampling_pattern) {
  if (msaa_samples == MsaaSamples::k1X ||
      sample_control == SampleControl::kCentersOnly) {
    return ((1 << kMaxInterpolators) - 1) * uint32_t(SampleLocation::kCenter);
  }
  if (sample_control == SampleControl::kCentroidsOnly) {
    return ((1 << kMaxInterpolators) - 1) * uint32_t(SampleLocation::kCentroid);
  }
  assert_true(sample_control == SampleControl::kCentroidsAndCenters);
  return interpolator_control_sampling_pattern;
}

enum class VGTOutputPath : uint32_t {
  kVertexReuse = 0,
  kTessellationEnable = 1,
  kPassthru = 2,
};

enum class TessellationMode : uint32_t {
  kDiscrete = 0,
  kContinuous = 1,
  kAdaptive = 2,
};

enum class PolygonModeEnable : uint32_t {
  kDisabled = 0,  // Render triangles.
  kDualMode = 1,  // Send 2 sets of 3 polygons with the specified polygon type.
};

enum class PolygonType : uint32_t {
  kPoints = 0,
  kLines = 1,
  kTriangles = 2,
};

enum class ModeControl : uint32_t {
  kIgnore = 0,
  kColorDepth = 4,
  // TODO(Triang3l): Verify whether kDepth means the pixel shader is ignored
  // completely even if it writes depth, exports to memory or kills pixels.
  // Hints suggesting that it should be completely ignored (which is desirable
  // on real hardware to avoid scheduling the pixel shader at all and waiting
  // for it especially since the Xbox 360 doesn't have early per-sample depth /
  // stencil, only early hi-Z / hi-stencil, and other registers possibly
  // toggling pixel shader execution are yet to be found):
  // - Most of depth pre-pass draws in Call of Duty 4 use the kDepth more with
  //   a `oC0 = tfetch2D(tf0, r0.xy) * r1` shader, some use `oC0 = r0` though.
  //   However, when alphatested surfaces are drawn, kColorDepth is explicitly
  //   used with the same shader performing the texture fetch.
  // - Red Dead Redemption has some kDepth draws with alphatest enabled, but the
  //   shader is `oC0 = r0`, which makes no sense (alphatest based on an
  //   interpolant from the vertex shader) as no texture alpha cutout is
  //   involved.
  // - Red Dead Redemption also has kDepth draws with pretty complex shaders
  //   clearly for use only in the color pass - even fetching and filtering a
  //   shadowmap.
  // For now, based on these, let's assume the pixel shader is never used with
  // kDepth.
  kDepth = 5,
  kCopy = 6,
};

// Xenos copies EDRAM contents to a tiled 2D or 3D texture (resolves - from
// "MSAA resolve", but this name is also used for single-sampled copying) by
// drawing primitives with the EDRAM mode ModeControl::kCopy. Pixels covered by
// the drawn geometry are copied. It's likely that only rectangular regions can
// be resolved.
//
// Resolve operation can write color data in ColorFormat formats, with or
// without MSAA color sample averaging, endian swap, red/blue swap, and exponent
// bias. Depth resolving likely has a lot more restrictions, considering sample
// averaging, red/blue swap and exponent bias would be pretty meaningless for it
// (also, Direct3D 9 specifies k_8_8_8_8 as RB_COPY_DEST_INFO::copy_dest_format
// for depth, which is clearly not true - the right format would be k_24_8 or
// k_24_8_FLOAT, so depth resolving likely doesn't support format conversion),
// though endian swap is supported.
//
// In addition, a resolve draw may clear the region it copies (this feature is
// commonly used when going to the next tile with predicated tiling). While one
// resolve draw call may copy just one color or depth buffer, it may clear both
// color and depth at once (or just color or depth, or nothing) if copying a
// color buffer (the color render target cleared is the same as the one copied -
// however, depth resolves have RB_COPY_CONTROL::copy_src_select 4, so they
// can't clear color).
//
// Direct3D 9 does resolving by drawing kRectangleList with 3 vertices with a
// vertex shader that accepts k_32_32_FLOAT vertices with k8in32 endianness in
// SHADER_CONSTANT_FETCH_00_0, with the half-pixel offset, according to the
// PA_SU_VTX_CNTL::pix_center setting, pre-applied to the vertices (for Direct3D
// 9 pixel centers, 0.5 must be added to the vertex positions to get the
// coordinates of the corners).
//
// The rectangle is used for both the source render target and the destination
// texture, according to how it's used in Tales of Vesperia.
//
// Direct3D 9 gives the rectangle in source render target coordinates (for
// example, in Halo 3, the sniper rifle scope has a (128,64)->(448,256)
// rectangle). It doesn't adjust the EDRAM base pointer, otherwise (taking into
// account that 4x MSAA is used for the scope) it would have been
// (8,0)->(328,192), but it's not. However, it adjusts the destination texture
// address so (0,0) relative to the destination address is (0,0) relative to
// the render target (if resolving a part of a render target to the top-left
// corner of a texture, Direct3D 9 actually moves the destination pointer before
// the start of the texture, with tiled offset internally calculated for a
// negative offset). When copying, the pointer needs to be adjusted to the first
// 32x32 tile that will actually be modified, by adding the value of
// XGAddress2D/3DTiledOffset called for left/top & ~31.
//
// RB_COPY_DEST_PITCH's purpose appears to be not clamping or something like
// that, but just specifying pitch for going between rows, and height for going
// between 3D texture slices. copy_dest_pitch is rounded to 32 by Direct3D 9,
// copy_dest_height is not. In the Halo 3 sniper rifle scope example,
// copy_dest_pitch is 320, and copy_dest_height is 192 - the same as the resolve
// rectangle size (resolving from a 320x192 portion of the surface at 128,64 to
// the whole texture, at 0,0). Relative to RB_COPY_DEST_BASE, the height should
// have been 256, but it's not. Adreno doesn't have copy_dest_height at all (as
// well as RB_COPY_DEST_INFO::copy_dest_slice), suggesting (alongside the name
// of the register) that it exists purely to be able to go between 3D texture
// slices.
//
// Window scissor must also be applied - in the jigsaw puzzle in Banjo-Tooie,
// there are 1280x720 resolve rectangles, but only the scissored 1280x256
// needs to be copied, otherwise it overflows even beyond the EDRAM, and the
// depth buffer is visible on the screen. It also ensures the coordinates are
// not negative (in F.E.A.R., for example, the right tile is resolved with
// vertices (-640,0)->(640,720), however, the destination texture pointer is
// adjusted properly to the right half of the texture, and the source render
// target has a pitch of 800).

// Granularity of offset and size in resolve operations is 8x8 pixels
// (GPU_RESOLVE_ALIGNMENT - for example, Halo 3 resolves a 24x16 region for a
// 18x10 texture, 8x8 region for a 1x1 texture).
// https://github.com/jmfauvel/CSGO-SDK/blob/master/game/client/view.cpp#L944
// https://github.com/stanriders/hl2-asw-port/blob/master/src/game/client/vgui_int.cpp#L901
constexpr uint32_t kResolveAlignmentPixelsLog2 = 3;
constexpr uint32_t kResolveAlignmentPixels = 1 << kResolveAlignmentPixelsLog2;

// Same as RB_SURFACE_INFO::surface_pitch, RB_COPY_DEST_PITCH::copy_dest_pitch
// and RB_COPY_DEST_PITCH::copy_dest_height.
constexpr uint32_t kResolveSizeBits = 14;
constexpr uint32_t kMaxResolveSize =
    (1 << kResolveSizeBits) - kResolveAlignmentPixels;

enum class CopyCommand : uint32_t {
  kRaw = 0,
  kConvert = 1,
  kConstantOne = 2,
  kNull = 3,  // ?
};

// a2xx_rb_copy_sample_select
enum class CopySampleSelect : uint32_t {
  k0,
  k1,
  k2,
  k3,
  k01,
  k23,
  k0123,
};

constexpr bool IsSingleCopySampleSelected(CopySampleSelect copy_sample_select) {
  return copy_sample_select >= CopySampleSelect::k0 &&
         copy_sample_select <= CopySampleSelect::k3;
}

#define XE_GPU_MAKE_SWIZZLE(x, y, z, w)                        \
  (((XE_GPU_SWIZZLE_##x) << 0) | ((XE_GPU_SWIZZLE_##y) << 3) | \
   ((XE_GPU_SWIZZLE_##z) << 6) | ((XE_GPU_SWIZZLE_##w) << 9))
typedef enum {
  XE_GPU_SWIZZLE_X = 0,
  XE_GPU_SWIZZLE_R = 0,
  XE_GPU_SWIZZLE_Y = 1,
  XE_GPU_SWIZZLE_G = 1,
  XE_GPU_SWIZZLE_Z = 2,
  XE_GPU_SWIZZLE_B = 2,
  XE_GPU_SWIZZLE_W = 3,
  XE_GPU_SWIZZLE_A = 3,
  XE_GPU_SWIZZLE_0 = 4,
  XE_GPU_SWIZZLE_1 = 5,
  XE_GPU_SWIZZLE_RGBA = XE_GPU_MAKE_SWIZZLE(R, G, B, A),
  XE_GPU_SWIZZLE_BGRA = XE_GPU_MAKE_SWIZZLE(B, G, R, A),
  XE_GPU_SWIZZLE_RGB1 = XE_GPU_MAKE_SWIZZLE(R, G, B, 1),
  XE_GPU_SWIZZLE_BGR1 = XE_GPU_MAKE_SWIZZLE(B, G, R, 1),
  XE_GPU_SWIZZLE_000R = XE_GPU_MAKE_SWIZZLE(0, 0, 0, R),
  XE_GPU_SWIZZLE_RRR1 = XE_GPU_MAKE_SWIZZLE(R, R, R, 1),
  XE_GPU_SWIZZLE_R111 = XE_GPU_MAKE_SWIZZLE(R, 1, 1, 1),
  XE_GPU_SWIZZLE_R000 = XE_GPU_MAKE_SWIZZLE(R, 0, 0, 0),
} XE_GPU_SWIZZLE;

inline uint16_t GpuSwap(uint16_t value, Endian endianness) {
  switch (endianness) {
    case Endian::kNone:
      // No swap.
      return value;
    case Endian::k8in16:
      // Swap bytes in half words.
      return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    default:
      assert_unhandled_case(endianness);
      return value;
  }
}

inline uint32_t GpuSwap(uint32_t value, Endian endianness) {
  switch (endianness) {
    default:
    case Endian::kNone:
      // No swap.
      return value;
    case Endian::k8in16:
      // Swap bytes in half words.
      return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    case Endian::k8in32:
      // Swap bytes.
      // NOTE: we are likely doing two swaps here. Wasteful. Oh well.
      return xe::byte_swap(value);
    case Endian::k16in32:
      // Swap half words.
      return ((value >> 16) & 0xFFFF) | (value << 16);
  }
}

inline float GpuSwap(float value, Endian endianness) {
  union {
    uint32_t i;
    float f;
  } v;
  v.f = value;
  v.i = GpuSwap(v.i, endianness);
  return v.f;
}

inline uint32_t GpuToCpu(uint32_t p) { return p; }

inline uint32_t CpuToGpu(uint32_t p) { return p & 0x1FFFFFFF; }

// SQ_TEX_VTX_INVALID/VALID_TEXTURE/BUFFER
enum class FetchConstantType : uint32_t {
  kInvalidTexture,
  kInvalidVertex,
  kTexture,
  kVertex,
};

// XE_GPU_REG_SHADER_CONSTANT_FETCH_*
XEPACKEDUNION(xe_gpu_vertex_fetch_t, {
  XEPACKEDSTRUCTANONYMOUS({
    FetchConstantType type : 2;  // +0
    uint32_t address : 30;       // +2 address in dwords

    Endian endian : 2;   // +0
    uint32_t size : 24;  // +2 size in words
    uint32_t unk1 : 6;   // +26
  });
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t dword_0;
    uint32_t dword_1;
  });
});

// Byte alignment of texture subresources in memory - of each mip and stack
// slice / cube face (and of textures themselves), this number of bits is also
// omitted from base_address and mip_address.
constexpr uint32_t kTextureSubresourceAlignmentBytesLog2 = 12;
constexpr uint32_t kTextureSubresourceAlignmentBytes =
    1 << kTextureSubresourceAlignmentBytesLog2;

// Texture fetch constant size field widths.
constexpr uint32_t kTexture1DMaxWidthLog2 = 24;
constexpr uint32_t kTexture1DMaxWidth = 1 << kTexture1DMaxWidthLog2;
constexpr uint32_t kTexture2DCubeMaxWidthHeightLog2 = 13;
constexpr uint32_t kTexture2DCubeMaxWidthHeight =
    1 << kTexture2DCubeMaxWidthHeightLog2;
constexpr uint32_t kTexture2DMaxStackDepthLog2 = 6;
constexpr uint32_t kTexture2DMaxStackDepth = 1 << kTexture2DMaxStackDepthLog2;
constexpr uint32_t kTexture3DMaxWidthHeightLog2 = 11;
constexpr uint32_t kTexture3DMaxWidthHeight = 1 << kTexture3DMaxWidthHeightLog2;
constexpr uint32_t kTexture3DMaxDepthLog2 = 10;
constexpr uint32_t kTexture3DMaxDepth = 1 << kTexture3DMaxDepthLog2;

// Tiled texture sizes are in 32x32 increments for 2D, 32x32x4 for 3D.
// 2DTiledOffset(X * 32 + x, Y * 32 + y) ==
//     2DTiledOffset(X * 32, Y * 32) + 2DTiledOffset(x, y)
// 3DTiledOffset(X * 32 + x, Y * 32 + y, Z * 8 + z) ==
//     3DTiledOffset(X * 32, Y * 32, Z * 8) + 3DTiledOffset(x, y, z)
// Both are true for negative offsets too.
constexpr uint32_t kTextureTileWidthHeightLog2 = 5;
constexpr uint32_t kTextureTileWidthHeight = 1 << kTextureTileWidthHeightLog2;
// 3D tiled texture slices 0:3 and 4:7 are stored separately in memory, in
// non-overlapping ranges, but addressing in 4:7 is different than in 0:3.
constexpr uint32_t kTextureTiledDepthGranularityLog2 = 2;
constexpr uint32_t kTextureTiledDepthGranularity =
    1 << kTextureTiledDepthGranularityLog2;
constexpr uint32_t kTextureTiledZBaseGranularityLog2 = 3;
constexpr uint32_t kTextureTiledZBaseGranularity =
    1 << kTextureTiledZBaseGranularityLog2;

// Row pitch alignment of non-tiled textures.
constexpr uint32_t kTextureLinearRowAlignmentBytesLog2 = 8;
constexpr uint32_t kTextureLinearRowAlignmentBytes =
    1 << kTextureLinearRowAlignmentBytesLog2;

// XE_GPU_REG_SHADER_CONSTANT_FETCH_*
XEPACKEDUNION(xe_gpu_texture_fetch_t, {
  XEPACKEDSTRUCTANONYMOUS({
    FetchConstantType type : 2;  // +0 dword_0
    // Likely before the swizzle, seems logical from R5xx (SIGNED_COMP0/1/2/3
    // set the signedness of components 0/1/2/3, while SEL_ALPHA/RED/GREEN/BLUE
    // specify "swizzling for each channel at the input of the pixel shader",
    // which can be texture components 0/1/2/3 or constant 0/1) and R6xx
    // (signedness is FORMAT_COMP_X/Y/Z/W, while the swizzle is DST_SEL_X/Y/Z/W,
    // which is named in resources the same as DST_SEL in fetch clauses).
    TextureSign sign_x : 2;                              // +2
    TextureSign sign_y : 2;                              // +4
    TextureSign sign_z : 2;                              // +6
    TextureSign sign_w : 2;                              // +8
    ClampMode clamp_x : 3;                               // +10
    ClampMode clamp_y : 3;                               // +13
    ClampMode clamp_z : 3;                               // +16
    SignedRepeatingFractionMode signed_rf_mode_all : 1;  // +19
    uint32_t dim_tbd : 2;                                // +20
    uint32_t pitch : 9;                                  // +22 byte_pitch >> 5
    uint32_t tiled : 1;                                  // +31

    TextureFormat format : 6;           // +0 dword_1
    Endian endianness : 2;              // +6
    uint32_t request_size : 2;          // +8
    uint32_t stacked : 1;               // +10
    uint32_t nearest_clamp_policy : 1;  // +11 d3d/opengl
    uint32_t base_address : 20;         // +12 base address >> 12

    // Size is stored with 1 subtracted from each component.
    union {  // dword_2
      struct {
        uint32_t width : 24;
        uint32_t : 8;
      } size_1d;
      struct {
        uint32_t width : 13;
        uint32_t height : 13;
        // Should be 0 for k2D and 5 for kCube if not stacked, but not very
        // meaningful in this case, likely should be ignored for non-stacked.
        uint32_t stack_depth : 6;
      } size_2d;
      struct {
        uint32_t width : 11;
        uint32_t height : 11;
        uint32_t depth : 10;
      } size_3d;
    };

    uint32_t num_format : 1;  // +0 dword_3 frac/int
    // xyzw, 3b each (XE_GPU_SWIZZLE)
    uint32_t swizzle : 12;                 // +1
    int32_t exp_adjust : 6;                // +13
    TextureFilter mag_filter : 2;          // +19
    TextureFilter min_filter : 2;          // +21
    TextureFilter mip_filter : 2;          // +23
    AnisoFilter aniso_filter : 3;          // +25
    ArbitraryFilter arbitrary_filter : 3;  // +28
    uint32_t border_size : 1;              // +31

    uint32_t vol_mag_filter : 1;  // +0 dword_4
    uint32_t vol_min_filter : 1;  // +1
    uint32_t mip_min_level : 4;   // +2
    uint32_t mip_max_level : 4;   // +6
    uint32_t mag_aniso_walk : 1;  // +10
    uint32_t min_aniso_walk : 1;  // +11
    // 5 fractional bits (A2XX_SQ_TEX_4_LOD_BIAS).
    int32_t lod_bias : 10;  // +12
    // Also known as LodBiasH/V in sys2gmem.
    int32_t grad_exp_adjust_h : 5;  // +22
    int32_t grad_exp_adjust_v : 5;  // +27

    BorderColor border_color : 2;    // +0 dword_5
    uint32_t force_bc_w_to_max : 1;  // +2
    // Also known as TriJuice.
    uint32_t tri_clamp : 2;       // +3
    int32_t aniso_bias : 4;       // +5
    DataDimension dimension : 2;  // +9
    uint32_t packed_mips : 1;     // +11
    uint32_t mip_address : 20;    // +12 mip address >> 12
  });
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t dword_0;
    uint32_t dword_1;
    uint32_t dword_2;
    uint32_t dword_3;
    uint32_t dword_4;
    uint32_t dword_5;
  });
});

// XE_GPU_REG_SHADER_CONSTANT_FETCH_*
XEPACKEDUNION(xe_gpu_fetch_group_t, {
  xe_gpu_texture_fetch_t texture_fetch;
  XEPACKEDSTRUCTANONYMOUS({
    xe_gpu_vertex_fetch_t vertex_fetch_0;
    xe_gpu_vertex_fetch_t vertex_fetch_1;
    xe_gpu_vertex_fetch_t vertex_fetch_2;
  });
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t dword_0;
    uint32_t dword_1;
    uint32_t dword_2;
    uint32_t dword_3;
    uint32_t dword_4;
    uint32_t dword_5;
  });
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t type_0 : 2;
    uint32_t data_0_a : 30;
    uint32_t data_0_b : 32;
    uint32_t type_1 : 2;
    uint32_t data_1_a : 30;
    uint32_t data_1_b : 32;
    uint32_t type_2 : 2;
    uint32_t data_2_a : 30;
    uint32_t data_2_b : 32;
  });
});

// GPU_MEMEXPORT_STREAM_CONSTANT from a game .pdb - float constant for memexport
// stream configuration.
// This is used with the floating-point ALU in shaders (written to eA using
// mad), so the dwords have a normalized exponent when reinterpreted as floats
// (otherwise they would be flushed to zero), but actually these are packed
// integers. dword_1 specifically is 2^23 because
// powf(2.0f, 23.0f) + float(i) == 0x4B000000 | i
// so mad can pack indices as integers in the lower bits.
XEPACKEDUNION(xe_gpu_memexport_stream_t, {
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t base_address : 30;  // +0 dword_0 physical address >> 2
    uint32_t const_0x1 : 2;      // +30

    uint32_t const_0x4b000000;  // +0 dword_1

    Endian128 endianness : 3;         // +0 dword_2
    uint32_t unused_0 : 5;            // +3
    ColorFormat format : 6;           // +8
    uint32_t unused_1 : 2;            // +14
    SurfaceNumFormat num_format : 3;  // +16
    uint32_t red_blue_swap : 1;       // +19
    uint32_t const_0x4b0 : 12;        // +20

    uint32_t index_count : 23;  // +0 dword_3
    uint32_t const_0x96 : 9;    // +23
  });
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t dword_0;
    uint32_t dword_1;
    uint32_t dword_2;
    uint32_t dword_3;
  });
});

XEPACKEDSTRUCT(xe_gpu_depth_sample_counts, {
  // This is little endian as it is swapped in D3D code.
  // Corresponding A and B values are summed up by D3D.
  // Occlusion there is calculated by substracting begin from end struct.
  uint32_t Total_A;
  uint32_t Total_B;
  uint32_t ZFail_A;
  uint32_t ZFail_B;
  uint32_t ZPass_A;
  uint32_t ZPass_B;
  uint32_t StencilFail_A;
  uint32_t StencilFail_B;
});

// Enum of event values used for VGT_EVENT_INITIATOR
enum Event {
  VS_DEALLOC = 0,
  PS_DEALLOC = 1,
  VS_DONE_TS = 2,
  PS_DONE_TS = 3,
  CACHE_FLUSH_TS = 4,
  CONTEXT_DONE = 5,
  CACHE_FLUSH = 6,
  VIZQUERY_START = 7,
  VIZQUERY_END = 8,
  SC_WAIT_WC = 9,
  MPASS_PS_CP_REFETCH = 10,
  MPASS_PS_RST_START = 11,
  MPASS_PS_INCR_START = 12,
  RST_PIX_CNT = 13,
  RST_VTX_CNT = 14,
  TILE_FLUSH = 15,
  CACHE_FLUSH_AND_INV_TS_EVENT = 20,
  ZPASS_DONE = 21,
  CACHE_FLUSH_AND_INV_EVENT = 22,
  PERFCOUNTER_START = 23,
  PERFCOUNTER_STOP = 24,
  SCREEN_EXT_INIT = 25,
  SCREEN_EXT_RPT = 26,
  VS_FETCH_DONE_TS = 27,
};

// Opcodes (IT_OPCODE) for Type-3 commands in the ringbuffer.
// https://github.com/freedreno/amd-gpu/blob/master/include/api/gsl_pm4types.h
// Not sure if all of these are used.
// clang-format off
enum Type3Opcode {
  PM4_ME_INIT               = 0x48,   // initialize CP's micro-engine

  PM4_NOP                   = 0x10,   // skip N 32-bit words to get to the next packet

  PM4_INDIRECT_BUFFER       = 0x3f,   // indirect buffer dispatch.  prefetch parser uses this packet type to determine whether to pre-fetch the IB
  PM4_INDIRECT_BUFFER_PFD   = 0x37,   // indirect buffer dispatch.  same as IB, but init is pipelined

  PM4_WAIT_FOR_IDLE         = 0x26,   // wait for the IDLE state of the engine
  PM4_WAIT_REG_MEM          = 0x3c,   // wait until a register or memory location is a specific value
  PM4_WAIT_REG_EQ           = 0x52,   // wait until a register location is equal to a specific value
  PM4_WAIT_REG_GTE          = 0x53,   // wait until a register location is >= a specific value
  PM4_WAIT_UNTIL_READ       = 0x5c,   // wait until a read completes
  PM4_WAIT_IB_PFD_COMPLETE  = 0x5d,   // wait until all base/size writes from an IB_PFD packet have completed

  PM4_REG_RMW               = 0x21,   // register read/modify/write
  PM4_REG_TO_MEM            = 0x3e,   // reads register in chip and writes to memory
  PM4_MEM_WRITE             = 0x3d,   // write N 32-bit words to memory
  PM4_MEM_WRITE_CNTR        = 0x4f,   // write CP_PROG_COUNTER value to memory
  PM4_COND_EXEC             = 0x44,   // conditional execution of a sequence of packets
  PM4_COND_WRITE            = 0x45,   // conditional write to memory or register

  PM4_EVENT_WRITE           = 0x46,   // generate an event that creates a write to memory when completed
  PM4_EVENT_WRITE_SHD       = 0x58,   // generate a VS|PS_done event
  PM4_EVENT_WRITE_CFL       = 0x59,   // generate a cache flush done event
  PM4_EVENT_WRITE_EXT       = 0x5a,   // generate a screen extent event
  PM4_EVENT_WRITE_ZPD       = 0x5b,   // generate a z_pass done event

  PM4_DRAW_INDX             = 0x22,   // initiate fetch of index buffer and draw
  PM4_DRAW_INDX_2           = 0x36,   // draw using supplied indices in packet
  PM4_DRAW_INDX_BIN         = 0x34,   // initiate fetch of index buffer and binIDs and draw
  PM4_DRAW_INDX_2_BIN       = 0x35,   // initiate fetch of bin IDs and draw using supplied indices

  PM4_VIZ_QUERY             = 0x23,   // begin/end initiator for viz query extent processing
  PM4_SET_STATE             = 0x25,   // fetch state sub-blocks and initiate shader code DMAs
  PM4_SET_CONSTANT          = 0x2d,   // load constant into chip and to memory
  PM4_SET_CONSTANT2         = 0x55,   // INCR_UPDATE_STATE
  PM4_SET_SHADER_CONSTANTS  = 0x56,   // INCR_UPDT_CONST
  PM4_LOAD_ALU_CONSTANT     = 0x2f,   // load constants from memory
  PM4_IM_LOAD               = 0x27,   // load sequencer instruction memory (pointer-based)
  PM4_IM_LOAD_IMMEDIATE     = 0x2b,   // load sequencer instruction memory (code embedded in packet)
  PM4_LOAD_CONSTANT_CONTEXT = 0x2e,   // load constants from a location in memory
  PM4_INVALIDATE_STATE      = 0x3b,   // selective invalidation of state pointers

  PM4_SET_SHADER_BASES      = 0x4A,   // dynamically changes shader instruction memory partition
  PM4_SET_BIN_BASE_OFFSET   = 0x4B,   // program an offset that will added to the BIN_BASE value of the 3D_DRAW_INDX_BIN packet
  PM4_SET_BIN_MASK          = 0x50,   // sets the 64-bit BIN_MASK register in the PFP
  PM4_SET_BIN_SELECT        = 0x51,   // sets the 64-bit BIN_SELECT register in the PFP

  PM4_CONTEXT_UPDATE        = 0x5e,   // updates the current context, if needed
  PM4_INTERRUPT             = 0x54,   // generate interrupt from the command stream

  PM4_XE_SWAP               = 0x64,   // Xenia only: VdSwap uses this to trigger a swap.

  PM4_IM_STORE              = 0x2c,   // copy sequencer instruction memory to system memory

  // Tiled rendering:
  // https://www.google.com/patents/US20060055701
  PM4_SET_BIN_MASK_LO       = 0x60,
  PM4_SET_BIN_MASK_HI       = 0x61,
  PM4_SET_BIN_SELECT_LO     = 0x62,
  PM4_SET_BIN_SELECT_HI     = 0x63,
};
// clang-format on

inline uint32_t MakePacketType0(uint16_t index, uint16_t count,
                                bool one_reg = false) {
  // ttcccccc cccccccc oiiiiiii iiiiiiii
  assert(index <= 0x7FFF);
  assert(count >= 1 && count <= 0x4000);
  return (0u << 30) | (((count - 1) & 0x3FFF) << 16) | (index & 0x7FFF);
}

inline uint32_t MakePacketType1(uint16_t index_1, uint16_t index_2) {
  // tt?????? ??222222 22222111 11111111
  assert(index_1 <= 0x7FF);
  assert(index_2 <= 0x7FF);
  return (1u << 30) | ((index_2 & 0x7FF) << 11) | (index_1 & 0x7FF);
}

constexpr inline uint32_t MakePacketType2() {
  // tt?????? ???????? ???????? ????????
  return (2u << 30);
}

inline uint32_t MakePacketType3(Type3Opcode opcode, uint16_t count,
                                bool predicate = false) {
  // ttcccccc cccccccc ?ooooooo ???????p
  assert(opcode <= 0x7F);
  assert(count >= 1 && count <= 0x4000);
  return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8) |
         (predicate ? 1 : 0);
}

}  // namespace xenos
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XENOS_H_
