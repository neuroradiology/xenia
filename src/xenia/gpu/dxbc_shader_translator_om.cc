/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/dxbc_shader_translator.h"

#include "xenia/base/assert.h"
#include "xenia/base/math.h"

namespace xe {
namespace gpu {
using namespace ucode;

void DxbcShaderTranslator::ROV_GetColorFormatSystemConstants(
    xenos::ColorRenderTargetFormat format, uint32_t write_mask,
    float& clamp_rgb_low, float& clamp_alpha_low, float& clamp_rgb_high,
    float& clamp_alpha_high, uint32_t& keep_mask_low,
    uint32_t& keep_mask_high) {
  keep_mask_low = keep_mask_high = 0;
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0xFF) << (i * 8);
        }
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= uint32_t(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
      clamp_rgb_low = clamp_alpha_low = 0.0f;
      clamp_rgb_high = 31.875f;
      clamp_alpha_high = 1.0f;
      for (uint32_t i = 0; i < 3; ++i) {
        if (!(write_mask & (1 << i))) {
          keep_mask_low |= uint32_t(0x3FF) << (i * 10);
        }
      }
      if (!(write_mask & 0b1000)) {
        keep_mask_low |= uint32_t(3) << 30;
      }
    } break;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      // Alpha clamping affects blending source, so it's non-zero for alpha for
      // k_16_16 (the render target is fixed-point). There's one deviation from
      // how Direct3D 11.3 functional specification defines SNorm conversion
      // (NaN should be 0, not the lowest negative number), but NaN handling in
      // output shouldn't be very important.
      clamp_rgb_low = clamp_alpha_low = -32.0f;
      clamp_rgb_high = clamp_alpha_high = 32.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      // No NaNs on the Xbox 360 GPU, though can't use the extended range with
      // f32tof16.
      clamp_rgb_low = clamp_alpha_low = -65504.0f;
      clamp_rgb_high = clamp_alpha_high = 65504.0f;
      if (!(write_mask & 0b0001)) {
        keep_mask_low |= 0xFFFFu;
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_low |= 0xFFFF0000u;
      }
      if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) {
        if (!(write_mask & 0b0100)) {
          keep_mask_high |= 0xFFFFu;
        }
        if (!(write_mask & 0b1000)) {
          keep_mask_high |= 0xFFFF0000u;
        }
      } else {
        write_mask &= 0b0011;
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      // No clamping - let min/max always pick the original value.
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high =
          std::nanf("");
      write_mask &= 0b0001;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~uint32_t(0);
      }
      break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      // No clamping - let min/max always pick the original value.
      clamp_rgb_low = clamp_alpha_low = clamp_rgb_high = clamp_alpha_high =
          std::nanf("");
      write_mask &= 0b0011;
      if (!(write_mask & 0b0001)) {
        keep_mask_low = ~uint32_t(0);
      }
      if (!(write_mask & 0b0010)) {
        keep_mask_high = ~uint32_t(0);
      }
      break;
    default:
      assert_unhandled_case(format);
      // Disable invalid render targets.
      write_mask = 0;
      break;
  }
  // Special case handled in the shaders for empty write mask to completely skip
  // a disabled render target: all keep bits are set.
  if (!write_mask) {
    keep_mask_low = keep_mask_high = ~uint32_t(0);
  }
}

void DxbcShaderTranslator::StartPixelShader_LoadROVParameters() {
  bool any_color_targets_written = current_shader().writes_color_targets() != 0;

  // ***************************************************************************
  // Get EDRAM offsets for the pixel:
  // system_temp_rov_params_.y - for depth (absolute).
  // system_temp_rov_params_.z - for 32bpp color (base-relative).
  // system_temp_rov_params_.w - for 64bpp color (base-relative).
  // ***************************************************************************

  // Extract the resolution scale as log2(scale)/2 specific for 1 (-> 0) and
  // 4 (-> 1) to a temp SGPR.
  uint32_t resolution_scale_log2_temp = PushSystemTemp();
  system_constants_used_ |= 1ull << kSysConst_EdramResolutionSquareScale_Index;
  a_.OpUShR(dxbc::Dest::R(resolution_scale_log2_temp, 0b0001),
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramResolutionSquareScale_Vec)
                .Select(kSysConst_EdramResolutionSquareScale_Comp),
            dxbc::Src::LU(2));
  // Convert the pixel position (if resolution scale is 4, this will be 2x2
  // bigger) to integer to system_temp_rov_params_.zw.
  // system_temp_rov_params_.z = X host pixel position as uint
  // system_temp_rov_params_.w = Y host pixel position as uint
  in_position_used_ |= 0b0011;
  a_.OpFToU(dxbc::Dest::R(system_temp_rov_params_, 0b1100),
            dxbc::Src::V(uint32_t(InOutRegister::kPSInPosition), 0b01000000));
  // Revert the resolution scale to convert the position to guest pixels.
  // system_temp_rov_params_.z = X guest pixel position / sample width
  // system_temp_rov_params_.w = Y guest pixel position / sample height
  a_.OpUShR(dxbc::Dest::R(system_temp_rov_params_, 0b1100),
            dxbc::Src::R(system_temp_rov_params_),
            dxbc::Src::R(resolution_scale_log2_temp, dxbc::Src::kXXXX));

  // Convert the position from pixels to samples.
  // system_temp_rov_params_.z = X guest sample 0 position
  // system_temp_rov_params_.w = Y guest sample 0 position
  system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
  a_.OpIShL(dxbc::Dest::R(system_temp_rov_params_, 0b1100),
            dxbc::Src::R(system_temp_rov_params_),
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_SampleCountLog2_Vec,
                          (kSysConst_SampleCountLog2_Comp << 4) |
                              ((kSysConst_SampleCountLog2_Comp + 1) << 6)));
  // Get 80x16 samples tile index - start dividing X by 80 by getting the high
  // part of the result of multiplication of X by 0xCCCCCCCD into X.
  // system_temp_rov_params_.x = (X * 0xCCCCCCCD) >> 32, or X / 80 * 64
  // system_temp_rov_params_.z = X guest sample 0 position
  // system_temp_rov_params_.w = Y guest sample 0 position
  a_.OpUMul(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Dest::Null(),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ),
            dxbc::Src::LU(0xCCCCCCCDu));
  // Get 80x16 samples tile index - finish dividing X by 80 and divide Y by 16
  // into system_temp_rov_params_.xy.
  // system_temp_rov_params_.x = X tile position
  // system_temp_rov_params_.y = Y tile position
  // system_temp_rov_params_.z = X guest sample 0 position
  // system_temp_rov_params_.w = Y guest sample 0 position
  a_.OpUShR(dxbc::Dest::R(system_temp_rov_params_, 0b0011),
            dxbc::Src::R(system_temp_rov_params_, 0b00001100),
            dxbc::Src::LU(6, 4, 0, 0));
  // Get the tile index to system_temp_rov_params_.y.
  // system_temp_rov_params_.x = X tile position
  // system_temp_rov_params_.y = tile index
  // system_temp_rov_params_.z = X guest sample 0 position
  // system_temp_rov_params_.w = Y guest sample 0 position
  system_constants_used_ |= 1ull << kSysConst_EdramPitchTiles_Index;
  a_.OpUMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramPitchTiles_Vec)
                .Select(kSysConst_EdramPitchTiles_Comp),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
  // Convert the tile index into a tile offset.
  // system_temp_rov_params_.x = X tile position
  // system_temp_rov_params_.y = tile offset
  // system_temp_rov_params_.z = X guest sample 0 position
  // system_temp_rov_params_.w = Y guest sample 0 position
  a_.OpUMul(dxbc::Dest::Null(), dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
            dxbc::Src::LU(1280));
  // Get tile-local X sample index into system_temp_rov_params_.z.
  // system_temp_rov_params_.y = tile offset
  // system_temp_rov_params_.z = X sample 0 position within the tile
  // system_temp_rov_params_.w = Y guest sample 0 position
  a_.OpIMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0100),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
            dxbc::Src::LI(-80),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ));
  // Get tile-local Y sample index into system_temp_rov_params_.w.
  // system_temp_rov_params_.y = tile offset
  // system_temp_rov_params_.z = X sample 0 position within the tile
  // system_temp_rov_params_.w = Y sample 0 position within the tile
  a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b1000),
           dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
           dxbc::Src::LU(15));
  // Go to the target row within the tile in system_temp_rov_params_.y.
  // system_temp_rov_params_.y = row offset
  // system_temp_rov_params_.z = X sample 0 position within the tile
  a_.OpIMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
            dxbc::Src::LI(80),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));
  // Choose in which 40-sample half of the tile the pixel is, for swapping
  // 40-sample columns when accessing the depth buffer - games expect this
  // behavior when writing depth back to the EDRAM via color writing (GTA IV,
  // Halo 3).
  // system_temp_rov_params_.x = tile-local sample 0 X >= 40
  // system_temp_rov_params_.y = row offset
  // system_temp_rov_params_.z = X sample 0 position within the tile
  a_.OpUGE(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
           dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ),
           dxbc::Src::LU(40));
  // Choose what to add to the depth/stencil X position.
  // system_temp_rov_params_.x = 40 or -40 offset for the depth buffer
  // system_temp_rov_params_.y = row offset
  // system_temp_rov_params_.z = X sample 0 position within the tile
  a_.OpMovC(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
            dxbc::Src::LI(-40), dxbc::Src::LI(40));
  // Flip tile halves for the depth/stencil buffer.
  // system_temp_rov_params_.x = X sample 0 position within the depth tile
  // system_temp_rov_params_.y = row offset
  // system_temp_rov_params_.z = X sample 0 position within the tile
  a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
  if (any_color_targets_written) {
    // Write 32bpp color offset to system_temp_rov_params_.z.
    // system_temp_rov_params_.x = X sample 0 position within the depth tile
    // system_temp_rov_params_.y = row offset
    // system_temp_rov_params_.z = unscaled 32bpp color offset
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0100),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ));
  }
  // Write depth/stencil offset to system_temp_rov_params_.y.
  // system_temp_rov_params_.y = unscaled 32bpp depth/stencil offset
  // system_temp_rov_params_.z = unscaled 32bpp color offset if needed
  a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
  // Add the EDRAM base for depth/stencil.
  // system_temp_rov_params_.y = unscaled 32bpp depth/stencil address
  // system_temp_rov_params_.z = unscaled 32bpp color offset if needed
  system_constants_used_ |= 1ull << kSysConst_EdramDepthBaseDwords_Index;
  a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY),
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramDepthBaseDwords_Vec)
                .Select(kSysConst_EdramDepthBaseDwords_Comp));

  // Apply the resolution scale.
  a_.OpIf(true, dxbc::Src::R(resolution_scale_log2_temp, dxbc::Src::kXXXX));
  // Release resolution_scale_log2_temp.
  PopSystemTemp();
  {
    dxbc::Dest offsets_dest(dxbc::Dest::R(
        system_temp_rov_params_, any_color_targets_written ? 0b0110 : 0b0010));
    // Scale the offsets by the resolution scale.
    // system_temp_rov_params_.y = scaled 32bpp depth/stencil first host pixel
    //                             address
    // system_temp_rov_params_.z = scaled 32bpp color first host pixel offset if
    //                             needed
    a_.OpIShL(offsets_dest, dxbc::Src::R(system_temp_rov_params_),
              dxbc::Src::LU(2));
    // Add host pixel offsets.
    // system_temp_rov_params_.y = scaled 32bpp depth/stencil address
    // system_temp_rov_params_.z = scaled 32bpp color offset if needed
    in_position_used_ |= 0b0011;
    for (uint32_t i = 0; i < 2; ++i) {
      // Convert a position component to integer.
      a_.OpFToU(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                dxbc::Src::V(uint32_t(InOutRegister::kPSInPosition)).Select(i));
      // Insert the host pixel offset on each axis.
      a_.OpBFI(offsets_dest, dxbc::Src::LU(1), dxbc::Src::LU(i),
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::R(system_temp_rov_params_));
    }
  }
  // Close the resolution scale conditional.
  a_.OpEndIf();

  if (any_color_targets_written) {
    // Get the 64bpp color offset to system_temp_rov_params_.w.
    // TODO(Triang3l): Find some game that aliases 64bpp with 32bpp to emulate
    // the real layout.
    // system_temp_rov_params_.y = scaled 32bpp depth/stencil address
    // system_temp_rov_params_.z = scaled 32bpp color offset
    // system_temp_rov_params_.w = scaled 64bpp color offset
    a_.OpIShL(dxbc::Dest::R(system_temp_rov_params_, 0b1000),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ),
              dxbc::Src::LU(1));
  }

  // ***************************************************************************
  // Sample coverage to system_temp_rov_params_.x.
  // ***************************************************************************

  // Using ForcedSampleCount of 4 (2 is not supported on Nvidia), so for 2x
  // MSAA, handling samples 0 and 3 (upper-left and lower-right) as 0 and 1.

  // Check if 4x MSAA is enabled.
  system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
  a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                              uint32_t(CbufferRegister::kSystemConstants),
                              kSysConst_SampleCountLog2_Vec)
                    .Select(kSysConst_SampleCountLog2_Comp));
  {
    // Copy the 4x AA coverage to system_temp_rov_params_.x, making top-right
    // the sample [2] and bottom-left the sample [1] (the opposite of Direct3D
    // 12), because on the Xbox 360, 2x MSAA doubles the storage width, 4x MSAA
    // doubles the storage height.
    // Flip samples in bits 0:1 to bits 29:30.
    a_.OpBFRev(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
               dxbc::Src::VCoverage());
    a_.OpUShR(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
              dxbc::Src::LU(29));
    a_.OpBFI(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(2),
             dxbc::Src::LU(1),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::VCoverage());
  }
  // Handle 1 or 2 samples.
  a_.OpElse();
  {
    // Extract sample 3 coverage, which will be used as sample 1.
    a_.OpUBFE(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(1),
              dxbc::Src::LU(3), dxbc::Src::VCoverage());
    // Combine coverage of samples 0 (in bit 0 of vCoverage) and 3 (in bit 0 of
    // system_temp_rov_params_.x).
    a_.OpBFI(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(31),
             dxbc::Src::LU(1),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::VCoverage());
  }
  // Close the 4x MSAA conditional.
  a_.OpEndIf();
}

void DxbcShaderTranslator::ROV_DepthStencilTest() {
  uint32_t temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src temp_y_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
  dxbc::Dest temp_z_dest(dxbc::Dest::R(temp, 0b0100));
  dxbc::Src temp_z_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));
  dxbc::Dest temp_w_dest(dxbc::Dest::R(temp, 0b1000));
  dxbc::Src temp_w_src(dxbc::Src::R(temp, dxbc::Src::kWWWW));

  // Check whether depth/stencil is enabled.
  // temp.x = kSysFlag_ROVDepthStencil
  system_constants_used_ |= 1ull << kSysConst_Flags_Index;
  a_.OpAnd(temp_x_dest,
           dxbc::Src::CB(cbuffer_index_system_constants_,
                         uint32_t(CbufferRegister::kSystemConstants),
                         kSysConst_Flags_Vec)
               .Select(kSysConst_Flags_Comp),
           dxbc::Src::LU(kSysFlag_ROVDepthStencil));
  // Open the depth/stencil enabled conditional.
  // temp.x = free
  a_.OpIf(true, temp_x_src);

  bool depth_stencil_early = ROV_IsDepthStencilEarly();
  bool shader_writes_depth = current_shader().writes_depth();

  for (uint32_t i = 0; i < 4; ++i) {
    // With early depth/stencil, depth/stencil writing may be deferred to the
    // end of the shader to prevent writing in case something (like alpha test,
    // which is dynamic GPU state) discards the pixel. So, write directly to the
    // persistent register, system_temp_depth_stencil_, instead of a local
    // temporary register.
    dxbc::Dest sample_depth_stencil_dest(
        depth_stencil_early ? dxbc::Dest::R(system_temp_depth_stencil_, 1 << i)
                            : temp_x_dest);
    dxbc::Src sample_depth_stencil_src(
        depth_stencil_early ? dxbc::Src::R(system_temp_depth_stencil_).Select(i)
                            : temp_x_src);

    if (!i) {
      if (shader_writes_depth) {
        // Clamp oDepth to the lower viewport depth bound (depth clamp happens
        // after the pixel shader in the pipeline, at least on Direct3D 11 and
        // Vulkan, thus applies to the shader's depth output too).
        system_constants_used_ |= 1ull << kSysConst_EdramDepthRange_Index;
        a_.OpMax(dxbc::Dest::R(system_temp_depth_stencil_, 0b0001),
                 dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX),
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeOffset_Comp));
        // Calculate the upper Z range bound to temp.x for clamping after
        // biasing.
        // temp.x = viewport maximum depth
        system_constants_used_ |= 1ull << kSysConst_EdramDepthRange_Index;
        a_.OpAdd(temp_x_dest,
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeOffset_Comp),
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeScale_Comp));
        // Clamp oDepth to the upper viewport depth bound (already not above 1,
        // but saturate for total safety).
        // temp.x = free
        a_.OpMin(dxbc::Dest::R(system_temp_depth_stencil_, 0b0001),
                 dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX),
                 temp_x_src, true);
        // Convert the shader-generated depth to 24-bit, using temp.x as
        // temporary.
        ROV_DepthTo24Bit(system_temp_depth_stencil_, 0,
                         system_temp_depth_stencil_, 0, temp, 0);
      } else {
        // Load the first sample's Z*W and W to temp.xy - need this regardless
        // of coverage for polygon offset.
        // temp.x = first sample's clip space Z*W
        // temp.y = first sample's clip space W
        a_.OpEvalSampleIndex(
            dxbc::Dest::R(temp, 0b0011),
            dxbc::Src::V(uint32_t(InOutRegister::kPSInClipSpaceZW)),
            dxbc::Src::LU(0));
        // Calculate the first sample's Z/W to temp.x for conversion to 24-bit
        // and depth test.
        // temp.x? = first sample's clip space Z
        // temp.y = free
        a_.OpDiv(sample_depth_stencil_dest, temp_x_src, temp_y_src, true);
        // Apply viewport Z range to the first sample because this would affect
        // the slope-scaled depth bias (tested on PC on Direct3D 12, by
        // comparing the fraction of the polygon's area with depth clamped -
        // affected by the constant bias, but not affected by the slope-scaled
        // bias, also depth range clamping should be done after applying the
        // offset as well).
        // temp.x? = first sample's viewport space Z
        system_constants_used_ |= 1ull << kSysConst_EdramDepthRange_Index;
        a_.OpMAd(sample_depth_stencil_dest, sample_depth_stencil_src,
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeScale_Comp),
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeOffset_Comp),
                 true);
        // Get the derivatives of a sample's depth, for the slope-scaled polygon
        // offset. Probably not very significant that it's for the sample 0
        // rather than for the center, likely neither is accurate because Xenos
        // probably calculates the slope between 16ths of a pixel according to
        // the meaning of the slope-scaled polygon offset in R5xx Acceleration.
        // temp.x? = first sample's viewport space Z
        // temp.y = ddx(z)
        // temp.z = ddy(z)
        a_.OpDerivRTXCoarse(temp_y_dest, sample_depth_stencil_src);
        a_.OpDerivRTYCoarse(temp_z_dest, sample_depth_stencil_src);
        // Get the maximum depth slope for polygon offset to temp.y.
        // https://docs.microsoft.com/en-us/windows/desktop/direct3d9/depth-bias
        // temp.x? = first sample's viewport space Z
        // temp.y = max(|ddx(z)|, |ddy(z)|)
        // temp.z = free
        a_.OpMax(temp_y_dest, temp_y_src.Abs(), temp_z_src.Abs());
        // Copy the needed polygon offset values to temp.zw.
        // temp.x? = first sample's viewport space Z
        // temp.y = max(|ddx(z)|, |ddy(z)|)
        // temp.z = polygon offset scale
        // temp.w = polygon offset bias
        in_front_face_used_ = true;
        system_constants_used_ |=
            (1ull << kSysConst_EdramPolyOffsetFront_Index) |
            (1ull << kSysConst_EdramPolyOffsetBack_Index);
        a_.OpMovC(
            dxbc::Dest::R(temp, 0b1100),
            dxbc::Src::V(uint32_t(InOutRegister::kPSInFrontFace),
                         dxbc::Src::kXXXX),
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramPolyOffsetFront_Vec,
                          (kSysConst_EdramPolyOffsetFrontScale_Comp << 4) |
                              (kSysConst_EdramPolyOffsetFrontOffset_Comp << 6)),
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramPolyOffsetBack_Vec,
                          (kSysConst_EdramPolyOffsetBackScale_Comp << 4) |
                              (kSysConst_EdramPolyOffsetBackOffset_Comp << 6)));
        // Apply the slope scale and the constant bias to the offset.
        // temp.x? = first sample's viewport space Z
        // temp.y = polygon offset
        // temp.z = free
        // temp.w = free
        a_.OpMAd(temp_y_dest, temp_y_src, temp_z_src, temp_w_src);
        // Calculate the upper Z range bound to temp.z for clamping after
        // biasing.
        // temp.x? = first sample's viewport space Z
        // temp.y = polygon offset
        // temp.z = viewport maximum depth
        system_constants_used_ |= 1ull << kSysConst_EdramDepthRange_Index;
        a_.OpAdd(temp_z_dest,
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeOffset_Comp),
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeScale_Comp));
      }
    }

    // Get if the current sample is covered to temp.w.
    // temp.x = first sample's viewport space Z if not writing to oDepth
    // temp.y = polygon offset if not writing to oDepth
    // temp.z = viewport maximum depth if not writing to oDepth
    // temp.w = coverage of the current sample
    a_.OpAnd(temp_w_dest,
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(1 << i));
    // Check if the current sample is covered. Release 1 VGPR.
    // temp.x = first sample's viewport space Z if not writing to oDepth
    // temp.y = polygon offset if not writing to oDepth
    // temp.z = viewport maximum depth if not writing to oDepth
    // temp.w = free
    a_.OpIf(true, temp_w_src);

    if (shader_writes_depth) {
      // Copy the 24-bit depth common to all samples to sample_depth_stencil.
      // temp.x = shader-generated 24-bit depth
      a_.OpMov(sample_depth_stencil_dest,
               dxbc::Src::R(system_temp_depth_stencil_, dxbc::Src::kXXXX));
    } else {
      if (i) {
        // Sample's depth precalculated for sample 0 (for slope-scaled depth
        // bias calculation), but need to calculate it for other samples.
        //
        // Reusing temp.x because it may contain the depth value for the first
        // sample, but it has been written already.
        //
        // For 2x:
        // Using ForcedSampleCount of 4 (2 is not supported on Nvidia), so for
        // 2x MSAA, handling samples 0 and 3 (upper-left and lower-right) as 0
        // and 1. Thus, evaluating Z/W at sample 3 when 4x is not enabled.
        //
        // For 4x:
        // Direct3D 12's sample pattern has 1 as top-right, 2 as bottom-left.
        // Xbox 360's render targets are 2x taller with 2x MSAA, 2x wider with
        // 4x, thus, likely 1 is bottom-left, 2 is top-right - swapping these.
        //
        // temp.x = sample's clip space Z*W
        // temp.y = polygon offset if not writing to oDepth
        // temp.z = viewport maximum depth if not writing to oDepth
        // temp.w = sample's clip space W
        if (i == 1) {
          system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
          a_.OpMovC(sample_depth_stencil_dest,
                    dxbc::Src::CB(cbuffer_index_system_constants_,
                                  uint32_t(CbufferRegister::kSystemConstants),
                                  kSysConst_SampleCountLog2_Vec)
                        .Select(kSysConst_SampleCountLog2_Comp),
                    dxbc::Src::LU(3), dxbc::Src::LU(2));
          a_.OpEvalSampleIndex(
              dxbc::Dest::R(temp, 0b1001),
              dxbc::Src::V(uint32_t(InOutRegister::kPSInClipSpaceZW),
                           0b01000000),
              sample_depth_stencil_src);
        } else {
          a_.OpEvalSampleIndex(
              dxbc::Dest::R(temp, 0b1001),
              dxbc::Src::V(uint32_t(InOutRegister::kPSInClipSpaceZW),
                           0b01000000),
              dxbc::Src::LU(i == 2 ? 1 : i));
        }
        // Calculate Z/W for the current sample from the evaluated Z*W and W.
        // temp.x? = sample's clip space Z
        // temp.y = polygon offset if not writing to oDepth
        // temp.z = viewport maximum depth if not writing to oDepth
        // temp.w = free
        a_.OpDiv(sample_depth_stencil_dest, temp_x_src, temp_w_src, true);
        // Apply viewport Z range the same way as it was applied to sample 0.
        // temp.x? = sample's viewport space Z
        // temp.y = polygon offset if not writing to oDepth
        // temp.z = viewport maximum depth if not writing to oDepth
        system_constants_used_ |= 1ull << kSysConst_EdramDepthRange_Index;
        a_.OpMAd(sample_depth_stencil_dest, sample_depth_stencil_src,
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeScale_Comp),
                 dxbc::Src::CB(cbuffer_index_system_constants_,
                               uint32_t(CbufferRegister::kSystemConstants),
                               kSysConst_EdramDepthRange_Vec)
                     .Select(kSysConst_EdramDepthRangeOffset_Comp),
                 true);
      }
      // Add the bias to the depth of the sample.
      // temp.x? = sample's unclamped biased Z
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      a_.OpAdd(sample_depth_stencil_dest, sample_depth_stencil_src, temp_y_src);
      // Clamp the biased depth to the lower viewport depth bound.
      // temp.x? = sample's lower-clamped biased Z
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      system_constants_used_ |= 1ull << kSysConst_EdramDepthRange_Index;
      a_.OpMax(sample_depth_stencil_dest, sample_depth_stencil_src,
               dxbc::Src::CB(cbuffer_index_system_constants_,
                             uint32_t(CbufferRegister::kSystemConstants),
                             kSysConst_EdramDepthRange_Vec)
                   .Select(kSysConst_EdramDepthRangeOffset_Comp));
      // Clamp the biased depth to the upper viewport depth bound.
      // temp.x? = sample's biased Z
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      a_.OpMin(sample_depth_stencil_dest, sample_depth_stencil_src, temp_z_src,
               true);
      // Convert the sample's depth to 24-bit, using temp.w as a temporary.
      // temp.x? = sample's 24-bit Z
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      ROV_DepthTo24Bit(sample_depth_stencil_src.index_1d_.index_,
                       sample_depth_stencil_src.swizzle_ & 3,
                       sample_depth_stencil_src.index_1d_.index_,
                       sample_depth_stencil_src.swizzle_ & 3, temp, 3);
    }
    // Load the old depth/stencil value to temp.w.
    // temp.x? = sample's 24-bit Z
    // temp.y = polygon offset if not writing to oDepth
    // temp.z = viewport maximum depth if not writing to oDepth
    // temp.w = old depth/stencil
    if (uav_index_edram_ == kBindingIndexUnallocated) {
      uav_index_edram_ = uav_count_++;
    }
    a_.OpLdUAVTyped(
        temp_w_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
        dxbc::Src::U(uav_index_edram_, uint32_t(UAVRegister::kEdram),
                     dxbc::Src::kXXXX));

    uint32_t sample_temp = PushSystemTemp();
    dxbc::Dest sample_temp_x_dest(dxbc::Dest::R(sample_temp, 0b0001));
    dxbc::Src sample_temp_x_src(dxbc::Src::R(sample_temp, dxbc::Src::kXXXX));
    dxbc::Dest sample_temp_y_dest(dxbc::Dest::R(sample_temp, 0b0010));
    dxbc::Src sample_temp_y_src(dxbc::Src::R(sample_temp, dxbc::Src::kYYYY));
    dxbc::Dest sample_temp_z_dest(dxbc::Dest::R(sample_temp, 0b0100));
    dxbc::Src sample_temp_z_src(dxbc::Src::R(sample_temp, dxbc::Src::kZZZZ));

    // Depth test.

    // Extract the old depth part to sample_depth_stencil.
    // sample_temp.x = old depth
    a_.OpUShR(sample_temp_x_dest, temp_w_src, dxbc::Src::LU(8));
    // Get the difference between the new and the old depth, > 0 - greater,
    // == 0 - equal, < 0 - less.
    // sample_temp.x = old depth
    // sample_temp.y = depth difference
    a_.OpIAdd(sample_temp_y_dest, sample_depth_stencil_src, -sample_temp_x_src);
    // Check if the depth is "less" or "greater or equal".
    // sample_temp.x = old depth
    // sample_temp.y = depth difference
    // sample_temp.z = depth difference less than 0
    a_.OpILT(sample_temp_z_dest, sample_temp_y_src, dxbc::Src::LI(0));
    // Choose the passed depth function bits for "less" or for "greater".
    // sample_temp.x = old depth
    // sample_temp.y = depth difference
    // sample_temp.z = depth function passed bits for "less" or "greater"
    a_.OpMovC(sample_temp_z_dest, sample_temp_z_src,
              dxbc::Src::LU(kSysFlag_ROVDepthPassIfLess),
              dxbc::Src::LU(kSysFlag_ROVDepthPassIfGreater));
    // Do the "equal" testing.
    // sample_temp.x = old depth
    // sample_temp.y = depth function passed bits
    // sample_temp.z = free
    a_.OpMovC(sample_temp_y_dest, sample_temp_y_src, sample_temp_z_src,
              dxbc::Src::LU(kSysFlag_ROVDepthPassIfEqual));
    // Mask the resulting bits with the ones that should pass.
    // sample_temp.x = old depth
    // sample_temp.y = masked depth function passed bits
    // sample_temp.z = free
    system_constants_used_ |= 1ull << kSysConst_Flags_Index;
    a_.OpAnd(sample_temp_y_dest, sample_temp_y_src,
             dxbc::Src::CB(cbuffer_index_system_constants_,
                           uint32_t(CbufferRegister::kSystemConstants),
                           kSysConst_Flags_Vec)
                 .Select(kSysConst_Flags_Comp));
    // Check if depth test has passed.
    // sample_temp.x = old depth
    // sample_temp.y = free
    a_.OpIf(true, sample_temp_y_src);
    {
      // Extract the depth write flag.
      // sample_temp.x = old depth
      // sample_temp.y = depth write mask
      system_constants_used_ |= 1ull << kSysConst_Flags_Index;
      a_.OpAnd(sample_temp_y_dest,
               dxbc::Src::CB(cbuffer_index_system_constants_,
                             uint32_t(CbufferRegister::kSystemConstants),
                             kSysConst_Flags_Vec)
                   .Select(kSysConst_Flags_Comp),
               dxbc::Src::LU(kSysFlag_ROVDepthWrite));
      // If depth writing is disabled, don't change the depth.
      // temp.x? = resulting sample depth after the depth test
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      // temp.w = old depth/stencil
      // sample_temp.x = free
      // sample_temp.y = free
      a_.OpMovC(sample_depth_stencil_dest, sample_temp_y_src,
                sample_depth_stencil_src, sample_temp_x_src);
    }
    // Depth test has failed.
    a_.OpElse();
    {
      // Exclude the bit from the covered sample mask.
      // sample_temp.x = old depth
      a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::LU(~uint32_t(1 << i)));
    }
    a_.OpEndIf();
    // Create packed depth/stencil, with the stencil value unchanged at this
    // point.
    // temp.x? = resulting sample depth, current resulting stencil
    // temp.y = polygon offset if not writing to oDepth
    // temp.z = viewport maximum depth if not writing to oDepth
    // temp.w = old depth/stencil
    a_.OpBFI(sample_depth_stencil_dest, dxbc::Src::LU(24), dxbc::Src::LU(8),
             sample_depth_stencil_src, temp_w_src);

    // Stencil test.

    // Extract the stencil test bit.
    // sample_temp.x = stencil test enabled
    system_constants_used_ |= 1ull << kSysConst_Flags_Index;
    a_.OpAnd(sample_temp_x_dest,
             dxbc::Src::CB(cbuffer_index_system_constants_,
                           uint32_t(CbufferRegister::kSystemConstants),
                           kSysConst_Flags_Vec)
                 .Select(kSysConst_Flags_Comp),
             dxbc::Src::LU(kSysFlag_ROVStencilTest));
    // Check if stencil test is enabled.
    // sample_temp.x = free
    a_.OpIf(true, sample_temp_x_src);
    {
      dxbc::Src stencil_front_src(
          dxbc::Src::CB(cbuffer_index_system_constants_,
                        uint32_t(CbufferRegister::kSystemConstants),
                        kSysConst_EdramStencil_Front_Vec));
      dxbc::Src stencil_back_src(
          dxbc::Src::CB(cbuffer_index_system_constants_,
                        uint32_t(CbufferRegister::kSystemConstants),
                        kSysConst_EdramStencil_Back_Vec));

      // Check the current face to get the reference and apply the read mask.
      in_front_face_used_ = true;
      a_.OpIf(true, dxbc::Src::V(uint32_t(InOutRegister::kPSInFrontFace),
                                 dxbc::Src::kXXXX));
      system_constants_used_ |= 1ull << kSysConst_EdramStencil_Index;
      for (uint32_t j = 0; j < 2; ++j) {
        if (j) {
          // Go to the back face.
          a_.OpElse();
        }
        dxbc::Src stencil_side_src(j ? stencil_back_src : stencil_front_src);
        // Read-mask the stencil reference.
        // sample_temp.x = read-masked stencil reference
        a_.OpAnd(sample_temp_x_dest,
                 stencil_side_src.Select(kSysConst_EdramStencil_Reference_Comp),
                 stencil_side_src.Select(kSysConst_EdramStencil_ReadMask_Comp));
        // Read-mask the old stencil value (also dropping the depth bits).
        // sample_temp.x = read-masked stencil reference
        // sample_temp.y = read-masked old stencil
        a_.OpAnd(sample_temp_y_dest, temp_w_src,
                 stencil_side_src.Select(kSysConst_EdramStencil_ReadMask_Comp));
      }
      // Close the face check.
      a_.OpEndIf();
      // Get the difference between the stencil reference and the old stencil,
      // > 0 - greater, == 0 - equal, < 0 - less.
      // sample_temp.x = stencil difference
      // sample_temp.y = free
      a_.OpIAdd(sample_temp_x_dest, sample_temp_x_src, -sample_temp_y_src);
      // Check if the stencil is "less" or "greater or equal".
      // sample_temp.x = stencil difference
      // sample_temp.y = stencil difference less than 0
      a_.OpILT(sample_temp_y_dest, sample_temp_x_src, dxbc::Src::LI(0));
      // Choose the passed depth function bits for "less" or for "greater".
      // sample_temp.x = stencil difference
      // sample_temp.y = stencil function passed bits for "less" or "greater"
      a_.OpMovC(sample_temp_y_dest, sample_temp_y_src,
                dxbc::Src::LU(uint32_t(xenos::CompareFunction::kLess)),
                dxbc::Src::LU(uint32_t(xenos::CompareFunction::kGreater)));
      // Do the "equal" testing.
      // sample_temp.x = stencil function passed bits
      // sample_temp.y = free
      a_.OpMovC(sample_temp_x_dest, sample_temp_x_src, sample_temp_y_src,
                dxbc::Src::LU(uint32_t(xenos::CompareFunction::kEqual)));
      // Get the comparison function and the operations for the current face.
      // sample_temp.x = stencil function passed bits
      // sample_temp.y = stencil function and operations
      in_front_face_used_ = true;
      system_constants_used_ |= 1ull << kSysConst_EdramStencil_Index;
      a_.OpMovC(sample_temp_y_dest,
                dxbc::Src::V(uint32_t(InOutRegister::kPSInFrontFace),
                             dxbc::Src::kXXXX),
                stencil_front_src.Select(kSysConst_EdramStencil_FuncOps_Comp),
                stencil_back_src.Select(kSysConst_EdramStencil_FuncOps_Comp));
      // Mask the resulting bits with the ones that should pass (the comparison
      // function is in the low 3 bits of the constant, and only ANDing 3-bit
      // values with it, so safe not to UBFE the function).
      // sample_temp.x = stencil test result
      // sample_temp.y = stencil function and operations
      a_.OpAnd(sample_temp_x_dest, sample_temp_x_src, sample_temp_y_src);
      // Handle passing and failure of the stencil test, to choose the operation
      // and to discard the sample.
      // sample_temp.x = free
      // sample_temp.y = stencil function and operations
      a_.OpIf(true, sample_temp_x_src);
      {
        // Check if depth test has passed for this sample to sample_temp.y (the
        // sample will only be processed if it's covered, so the only thing that
        // could unset the bit at this point that matters is the depth test).
        // sample_temp.x = depth test result
        // sample_temp.y = stencil function and operations
        a_.OpAnd(sample_temp_x_dest,
                 dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                 dxbc::Src::LU(1 << i));
        // Choose the bit offset of the stencil operation.
        // sample_temp.x = sample operation offset
        // sample_temp.y = stencil function and operations
        a_.OpMovC(sample_temp_x_dest, sample_temp_x_src, dxbc::Src::LU(6),
                  dxbc::Src::LU(9));
        // Extract the stencil operation.
        // sample_temp.x = stencil operation
        // sample_temp.y = free
        a_.OpUBFE(sample_temp_x_dest, dxbc::Src::LU(3), sample_temp_x_src,
                  sample_temp_y_src);
      }
      // Stencil test has failed.
      a_.OpElse();
      {
        // Extract the stencil fail operation.
        // sample_temp.x = stencil operation
        // sample_temp.y = free
        a_.OpUBFE(sample_temp_x_dest, dxbc::Src::LU(3), dxbc::Src::LU(3),
                  sample_temp_y_src);
        // Exclude the bit from the covered sample mask.
        // sample_temp.x = stencil operation
        a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                 dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                 dxbc::Src::LU(~uint32_t(1 << i)));
      }
      // Close the stencil pass check.
      a_.OpEndIf();

      // Open the stencil operation switch for writing the new stencil (not
      // caring about bits 8:31).
      // sample_temp.x = will contain unmasked new stencil in 0:7 and junk above
      a_.OpSwitch(sample_temp_x_src);
      {
        // Zero.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kZero)));
        a_.OpMov(sample_temp_x_dest, dxbc::Src::LU(0));
        a_.OpBreak();
        // Replace.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kReplace)));
        in_front_face_used_ = true;
        system_constants_used_ |= 1ull << kSysConst_EdramStencil_Index;
        a_.OpMovC(
            sample_temp_x_dest,
            dxbc::Src::V(uint32_t(InOutRegister::kPSInFrontFace),
                         dxbc::Src::kXXXX),
            stencil_front_src.Select(kSysConst_EdramStencil_Reference_Comp),
            stencil_back_src.Select(kSysConst_EdramStencil_Reference_Comp));
        a_.OpBreak();
        // Increment and clamp.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kIncrementClamp)));
        {
          // Clear the upper bits for saturation.
          a_.OpAnd(sample_temp_x_dest, temp_w_src, dxbc::Src::LU(UINT8_MAX));
          // Increment.
          a_.OpIAdd(sample_temp_x_dest, sample_temp_x_src, dxbc::Src::LI(1));
          // Clamp.
          a_.OpIMin(sample_temp_x_dest, sample_temp_x_src,
                    dxbc::Src::LI(UINT8_MAX));
        }
        a_.OpBreak();
        // Decrement and clamp.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kDecrementClamp)));
        {
          // Clear the upper bits for saturation.
          a_.OpAnd(sample_temp_x_dest, temp_w_src, dxbc::Src::LU(UINT8_MAX));
          // Increment.
          a_.OpIAdd(sample_temp_x_dest, sample_temp_x_src, dxbc::Src::LI(-1));
          // Clamp.
          a_.OpIMax(sample_temp_x_dest, sample_temp_x_src, dxbc::Src::LI(0));
        }
        a_.OpBreak();
        // Invert.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kInvert)));
        a_.OpNot(sample_temp_x_dest, temp_w_src);
        a_.OpBreak();
        // Increment and wrap.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kIncrementWrap)));
        a_.OpIAdd(sample_temp_x_dest, temp_w_src, dxbc::Src::LI(1));
        a_.OpBreak();
        // Decrement and wrap.
        a_.OpCase(dxbc::Src::LU(uint32_t(xenos::StencilOp::kDecrementWrap)));
        a_.OpIAdd(sample_temp_x_dest, temp_w_src, dxbc::Src::LI(-1));
        a_.OpBreak();
        // Keep.
        a_.OpDefault();
        a_.OpMov(sample_temp_x_dest, temp_w_src);
        a_.OpBreak();
      }
      // Close the new stencil switch.
      a_.OpEndSwitch();

      // Select the stencil write mask for the face.
      // sample_temp.x = unmasked new stencil in 0:7 and junk above
      // sample_temp.y = stencil write mask
      in_front_face_used_ = true;
      system_constants_used_ |= 1ull << kSysConst_EdramStencil_Index;
      a_.OpMovC(sample_temp_y_dest,
                dxbc::Src::V(uint32_t(InOutRegister::kPSInFrontFace),
                             dxbc::Src::kXXXX),
                stencil_front_src.Select(kSysConst_EdramStencil_WriteMask_Comp),
                stencil_back_src.Select(kSysConst_EdramStencil_WriteMask_Comp));
      // Apply the write mask to the new stencil, also dropping the upper 24
      // bits.
      // sample_temp.x = masked new stencil
      // sample_temp.y = stencil write mask
      a_.OpAnd(sample_temp_x_dest, sample_temp_x_src, sample_temp_y_src);
      // Invert the write mask for keeping the old stencil and the depth bits.
      // sample_temp.x = masked new stencil
      // sample_temp.y = inverted stencil write mask
      a_.OpNot(sample_temp_y_dest, sample_temp_y_src);
      // Remove the bits that will be replaced from the new combined
      // depth/stencil.
      // sample_temp.x = masked new stencil
      // sample_temp.y = free
      a_.OpAnd(sample_depth_stencil_dest, sample_depth_stencil_src,
               sample_temp_y_src);
      // Merge the old and the new stencil.
      // temp.x? = resulting sample depth/stencil
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      // temp.w = old depth/stencil
      // sample_temp.x = free
      a_.OpOr(sample_depth_stencil_dest, sample_depth_stencil_src,
              sample_temp_x_src);
    }
    // Close the stencil test check.
    a_.OpEndIf();

    // Check if the depth/stencil has failed not to modify the depth if it has.
    // sample_temp.x = whether depth/stencil has passed for this sample
    a_.OpAnd(sample_temp_x_dest,
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(1 << i));
    // If the depth/stencil test has failed, don't change the depth.
    // sample_temp.x = free
    a_.OpIf(false, sample_temp_x_src);
    {
      // Copy the new stencil over the old depth.
      // temp.x? = resulting sample depth/stencil
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      // temp.w = old depth/stencil
      a_.OpBFI(sample_depth_stencil_dest, dxbc::Src::LU(8), dxbc::Src::LU(0),
               sample_depth_stencil_src, temp_w_src);
    }
    // Close the depth/stencil passing check.
    a_.OpEndIf();
    // Check if the new depth/stencil is different, and thus needs to be
    // written, to temp.w.
    // temp.x? = resulting sample depth/stencil
    // temp.y = polygon offset if not writing to oDepth
    // temp.z = viewport maximum depth if not writing to oDepth
    // temp.w = whether depth/stencil has been modified
    a_.OpINE(temp_w_dest, sample_depth_stencil_src, temp_w_src);
    if (depth_stencil_early &&
        !current_shader().implicit_early_z_write_allowed()) {
      // Set the sample bit in bits 4:7 of system_temp_rov_params_.x - always
      // need to write late in this shader, as it may do something like
      // explicitly killing pixels.
      a_.OpBFI(dxbc::Dest::R(system_temp_rov_params_, 0b0001), dxbc::Src::LU(1),
               dxbc::Src::LU(4 + i), temp_w_src,
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX));
    } else {
      // Check if need to write.
      // temp.x? = resulting sample depth/stencil
      // temp.y = polygon offset if not writing to oDepth
      // temp.z = viewport maximum depth if not writing to oDepth
      // temp.w = free
      a_.OpIf(true, temp_w_src);
      {
        if (depth_stencil_early) {
          // Get if early depth/stencil write is enabled to temp.w.
          // temp.w = whether early depth/stencil write is enabled
          system_constants_used_ |= 1ull << kSysConst_Flags_Index;
          a_.OpAnd(temp_w_dest,
                   dxbc::Src::CB(cbuffer_index_system_constants_,
                                 uint32_t(CbufferRegister::kSystemConstants),
                                 kSysConst_Flags_Vec)
                       .Select(kSysConst_Flags_Comp),
                   dxbc::Src::LU(kSysFlag_ROVDepthStencilEarlyWrite));
          // Check if need to write early.
          // temp.w = free
          a_.OpIf(true, temp_w_src);
        }
        // Write the new depth/stencil.
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(
            dxbc::Dest::U(uav_index_edram_, uint32_t(UAVRegister::kEdram)),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
            sample_depth_stencil_src);
        if (depth_stencil_early) {
          // Need to still run the shader to know whether to write the
          // depth/stencil value.
          a_.OpElse();
          // Set the sample bit in bits 4:7 of system_temp_rov_params_.x if need
          // to write later (after checking if the sample is not discarded by a
          // kill instruction, alphatest or alpha-to-coverage).
          a_.OpOr(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
                  dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
                  dxbc::Src::LU(1 << (4 + i)));
          // Close the early depth/stencil check.
          a_.OpEndIf();
        }
      }
      // Close the write check.
      a_.OpEndIf();
    }

    // Release sample_temp.
    PopSystemTemp();

    // Close the sample conditional.
    a_.OpEndIf();

    // Go to the next sample (samples are at +0, +80, +1, +81, so need to do
    // +80, -79, +80 and -81 after each sample).
    system_constants_used_ |= 1ull
                              << kSysConst_EdramResolutionSquareScale_Index;
    a_.OpIMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
              dxbc::Src::LI((i & 1) ? -78 - i : 80),
              dxbc::Src::CB(cbuffer_index_system_constants_,
                            uint32_t(CbufferRegister::kSystemConstants),
                            kSysConst_EdramResolutionSquareScale_Vec)
                  .Select(kSysConst_EdramResolutionSquareScale_Comp),
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));
  }

  if (ROV_IsDepthStencilEarly()) {
    // Check if safe to discard the whole 2x2 quad early, without running the
    // translated pixel shader, by checking if coverage is 0 in all pixels in
    // the quad and if there are no samples which failed the depth test, but
    // where stencil was modified and needs to be written in the end. Must
    // reject at 2x2 quad granularity because texture fetches need derivatives.

    // temp.x = coverage | deferred depth/stencil write
    a_.OpAnd(dxbc::Dest::R(temp, 0b0001),
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(0b11111111));
    // temp.x = 1.0 if any sample is covered or potentially needs stencil write
    // in the end of the shader in the current pixel
    a_.OpMovC(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(temp, dxbc::Src::kXXXX),
              dxbc::Src::LF(1.0f), dxbc::Src::LF(0.0f));
    // temp.x = 1.0 if any sample is covered or potentially needs stencil write
    // in the end of the shader in the current pixel
    // temp.y = non-zero if anything is covered in the pixel across X
    a_.OpDerivRTXFine(dxbc::Dest::R(temp, 0b0010),
                      dxbc::Src::R(temp, dxbc::Src::kXXXX));
    // temp.x = 1.0 if anything is covered in the current half of the quad
    // temp.y = free
    a_.OpMovC(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(temp, dxbc::Src::kYYYY),
              dxbc::Src::LF(1.0f), dxbc::Src::R(temp, dxbc::Src::kXXXX));
    // temp.x = 1.0 if anything is covered in the current half of the quad
    // temp.y = non-zero if anything is covered in the two pixels across Y
    a_.OpDerivRTYCoarse(dxbc::Dest::R(temp, 0b0010),
                        dxbc::Src::R(temp, dxbc::Src::kXXXX));
    // temp.x = 1.0 if anything is covered in the current whole quad
    // temp.y = free
    a_.OpMovC(dxbc::Dest::R(temp, 0b0001), dxbc::Src::R(temp, dxbc::Src::kYYYY),
              dxbc::Src::LF(1.0f), dxbc::Src::R(temp, dxbc::Src::kXXXX));
    // End the shader if nothing is covered in the 2x2 quad after early
    // depth/stencil.
    // temp.x = free
    a_.OpRetC(false, dxbc::Src::R(temp, dxbc::Src::kXXXX));
  }

  // Close the large depth/stencil conditional.
  a_.OpEndIf();

  // Release temp.
  PopSystemTemp();
}

void DxbcShaderTranslator::ROV_UnpackColor(
    uint32_t rt_index, uint32_t packed_temp, uint32_t packed_temp_components,
    uint32_t color_temp, uint32_t temp1, uint32_t temp1_component,
    uint32_t temp2, uint32_t temp2_component) {
  assert_true(color_temp != packed_temp || packed_temp_components == 0);

  dxbc::Src packed_temp_low(
      dxbc::Src::R(packed_temp).Select(packed_temp_components));
  dxbc::Dest temp1_dest(dxbc::Dest::R(temp1, 1 << temp1_component));
  dxbc::Src temp1_src(dxbc::Src::R(temp1).Select(temp1_component));
  dxbc::Dest temp2_dest(dxbc::Dest::R(temp2, 1 << temp2_component));
  dxbc::Src temp2_src(dxbc::Src::R(temp2).Select(temp2_component));

  // Break register dependencies and initialize if there are not enough
  // components. The rest of the function will write at least RG (k_32_FLOAT and
  // k_32_32_FLOAT handled with the same default label), and if packed_temp is
  // the same as color_temp, the packed color won't be touched.
  a_.OpMov(dxbc::Dest::R(color_temp, 0b1100),
           dxbc::Src::LF(0.0f, 0.0f, 0.0f, 1.0f));

  // Choose the packing based on the render target's format.
  system_constants_used_ |= 1ull << kSysConst_EdramRTFormatFlags_Index;
  a_.OpSwitch(dxbc::Src::CB(cbuffer_index_system_constants_,
                            uint32_t(CbufferRegister::kSystemConstants),
                            kSysConst_EdramRTFormatFlags_Vec)
                  .Select(rt_index));

  // ***************************************************************************
  // k_8_8_8_8
  // k_8_8_8_8_GAMMA
  // ***************************************************************************
  for (uint32_t i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA
          : xenos::ColorRenderTargetFormat::k_8_8_8_8)));
    // Unpack the components.
    a_.OpUBFE(dxbc::Dest::R(color_temp), dxbc::Src::LU(8),
              dxbc::Src::LU(0, 8, 16, 24), packed_temp_low);
    // Convert from fixed-point.
    a_.OpUToF(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp));
    // Normalize.
    a_.OpMul(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp),
             dxbc::Src::LF(1.0f / 255.0f));
    if (i) {
      for (uint32_t j = 0; j < 3; ++j) {
        ConvertPWLGamma(false, color_temp, j, color_temp, j, temp1,
                        temp1_component, temp2, temp2_component);
      }
    }
    a_.OpBreak();
  }

  // ***************************************************************************
  // k_2_10_10_10
  // k_2_10_10_10_AS_10_10_10_10
  // ***************************************************************************
  a_.OpCase(dxbc::Src::LU(
      ROV_AddColorFormatFlags(xenos::ColorRenderTargetFormat::k_2_10_10_10)));
  a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)));
  {
    // Unpack the components.
    a_.OpUBFE(dxbc::Dest::R(color_temp), dxbc::Src::LU(10, 10, 10, 2),
              dxbc::Src::LU(0, 10, 20, 30), packed_temp_low);
    // Convert from fixed-point.
    a_.OpUToF(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp));
    // Normalize.
    a_.OpMul(dxbc::Dest::R(color_temp), dxbc::Src::R(color_temp),
             dxbc::Src::LF(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f,
                           1.0f / 3.0f));
  }
  a_.OpBreak();

  // ***************************************************************************
  // k_2_10_10_10_FLOAT
  // k_2_10_10_10_FLOAT_AS_16_16_16_16
  // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp
  // ***************************************************************************
  a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)));
  a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)));
  {
    // Unpack the alpha.
    a_.OpUBFE(dxbc::Dest::R(color_temp, 0b1000), dxbc::Src::LU(2),
              dxbc::Src::LU(30), packed_temp_low);
    // Convert the alpha from fixed-point.
    a_.OpUToF(dxbc::Dest::R(color_temp, 0b1000),
              dxbc::Src::R(color_temp, dxbc::Src::kWWWW));
    // Normalize the alpha.
    a_.OpMul(dxbc::Dest::R(color_temp, 0b1000),
             dxbc::Src::R(color_temp, dxbc::Src::kWWWW),
             dxbc::Src::LF(1.0f / 3.0f));
    // Process the components in reverse order because color_temp.r stores the
    // packed color which shouldn't be touched until G and B are converted if
    // packed_temp and color_temp are the same.
    for (int32_t i = 2; i >= 0; --i) {
      dxbc::Dest color_component_dest(dxbc::Dest::R(color_temp, 1 << i));
      dxbc::Src color_component_src(dxbc::Src::R(color_temp).Select(i));
      // Unpack the exponent to the temp.
      a_.OpUBFE(temp1_dest, dxbc::Src::LU(3), dxbc::Src::LU(i * 10 + 7),
                packed_temp_low);
      // Unpack the mantissa to the result.
      a_.OpUBFE(color_component_dest, dxbc::Src::LU(7), dxbc::Src::LU(i * 10),
                packed_temp_low);
      // Check if the number is denormalized.
      a_.OpIf(false, temp1_src);
      {
        // Check if the number is non-zero (if the mantissa isn't zero - the
        // exponent is known to be zero at this point).
        a_.OpIf(true, color_component_src);
        {
          // Normalize the mantissa.
          // Note that HLSL firstbithigh(x) is compiled to DXBC like:
          // `x ? 31 - firstbit_hi(x) : -1`
          // (returns the index from the LSB, not the MSB, but -1 for zero too).
          // temp = firstbit_hi(mantissa)
          a_.OpFirstBitHi(temp1_dest, color_component_src);
          // temp  = 7 - (31 - firstbit_hi(mantissa))
          // Or, if expanded:
          // temp = firstbit_hi(mantissa) - 24
          a_.OpIAdd(temp1_dest, temp1_src, dxbc::Src::LI(-24));
          // mantissa = mantissa << (7 - firstbithigh(mantissa))
          // AND 0x7F not needed after this - BFI will do it.
          a_.OpIShL(color_component_dest, color_component_src, temp1_src);
          // Get the normalized exponent.
          // exponent = 1 - (7 - firstbithigh(mantissa))
          a_.OpIAdd(temp1_dest, dxbc::Src::LI(1), -temp1_src);
        }
        // The number is zero.
        a_.OpElse();
        {
          // Set the unbiased exponent to -124 for zero - 124 will be added
          // later, resulting in zero float32.
          a_.OpMov(temp1_dest, dxbc::Src::LI(-124));
        }
        // Close the non-zero check.
        a_.OpEndIf();
      }
      // Close the denormal check.
      a_.OpEndIf();
      // Bias the exponent and move it to the correct location in f32.
      a_.OpIMAd(temp1_dest, temp1_src, dxbc::Src::LI(1 << 23),
                dxbc::Src::LI(124 << 23));
      // Combine the mantissa and the exponent.
      a_.OpBFI(color_component_dest, dxbc::Src::LU(7), dxbc::Src::LU(16),
               color_component_src, temp1_src);
    }
  }
  a_.OpBreak();

  // ***************************************************************************
  // k_16_16
  // k_16_16_16_16 (64bpp)
  // ***************************************************************************
  for (uint32_t i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_16_16_16_16
          : xenos::ColorRenderTargetFormat::k_16_16)));
    dxbc::Dest color_components_dest(
        dxbc::Dest::R(color_temp, i ? 0b1111 : 0b0011));
    // Unpack the components.
    a_.OpIBFE(color_components_dest, dxbc::Src::LU(16),
              dxbc::Src::LU(0, 16, 0, 16),
              dxbc::Src::R(packed_temp,
                           0b01010000 + packed_temp_components * 0b01010101));
    // Convert from fixed-point.
    a_.OpIToF(color_components_dest, dxbc::Src::R(color_temp));
    // Normalize.
    a_.OpMul(color_components_dest, dxbc::Src::R(color_temp),
             dxbc::Src::LF(32.0f / 32767.0f));
    a_.OpBreak();
  }

  // ***************************************************************************
  // k_16_16_FLOAT
  // k_16_16_16_16_FLOAT (64bpp)
  // ***************************************************************************
  for (uint32_t i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT
          : xenos::ColorRenderTargetFormat::k_16_16_FLOAT)));
    dxbc::Dest color_components_dest(
        dxbc::Dest::R(color_temp, i ? 0b1111 : 0b0011));
    // Unpack the components.
    a_.OpUBFE(color_components_dest, dxbc::Src::LU(16),
              dxbc::Src::LU(0, 16, 0, 16),
              dxbc::Src::R(packed_temp,
                           0b01010000 + packed_temp_components * 0b01010101));
    // Convert from 16-bit float.
    a_.OpF16ToF32(color_components_dest, dxbc::Src::R(color_temp));
    a_.OpBreak();
  }

  if (packed_temp != color_temp) {
    // Assume k_32_FLOAT or k_32_32_FLOAT for the rest.
    a_.OpDefault();
    a_.OpMov(
        dxbc::Dest::R(color_temp, 0b0011),
        dxbc::Src::R(packed_temp, 0b0100 + packed_temp_components * 0b0101));
    a_.OpBreak();
  }

  a_.OpEndSwitch();
}

void DxbcShaderTranslator::ROV_PackPreClampedColor(
    uint32_t rt_index, uint32_t color_temp, uint32_t packed_temp,
    uint32_t packed_temp_components, uint32_t temp1, uint32_t temp1_component,
    uint32_t temp2, uint32_t temp2_component) {
  // Packing normalized formats according to the Direct3D 11.3 functional
  // specification, but assuming clamping was done by the caller.

  assert_true(color_temp != packed_temp || packed_temp_components == 0);

  dxbc::Dest packed_dest_low(
      dxbc::Dest::R(packed_temp, 1 << packed_temp_components));
  dxbc::Src packed_src_low(
      dxbc::Src::R(packed_temp).Select(packed_temp_components));
  dxbc::Dest temp1_dest(dxbc::Dest::R(temp1, 1 << temp1_component));
  dxbc::Src temp1_src(dxbc::Src::R(temp1).Select(temp1_component));
  dxbc::Dest temp2_dest(dxbc::Dest::R(temp2, 1 << temp2_component));
  dxbc::Src temp2_src(dxbc::Src::R(temp2).Select(temp2_component));

  // Break register dependency after 32bpp cases.
  a_.OpMov(dxbc::Dest::R(packed_temp, 1 << (packed_temp_components + 1)),
           dxbc::Src::LU(0));

  // Choose the packing based on the render target's format.
  system_constants_used_ |= 1ull << kSysConst_EdramRTFormatFlags_Index;
  a_.OpSwitch(dxbc::Src::CB(cbuffer_index_system_constants_,
                            uint32_t(CbufferRegister::kSystemConstants),
                            kSysConst_EdramRTFormatFlags_Vec)
                  .Select(rt_index));

  // ***************************************************************************
  // k_8_8_8_8
  // k_8_8_8_8_GAMMA
  // ***************************************************************************
  for (uint32_t i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA
          : xenos::ColorRenderTargetFormat::k_8_8_8_8)));
    for (uint32_t j = 0; j < 4; ++j) {
      if (i && j < 3) {
        ConvertPWLGamma(true, color_temp, j, temp1, temp1_component, temp1,
                        temp1_component, temp2, temp2_component);
        // Denormalize and add 0.5 for rounding.
        a_.OpMAd(temp1_dest, temp1_src, dxbc::Src::LF(255.0f),
                 dxbc::Src::LF(0.5f));
      } else {
        // Denormalize and add 0.5 for rounding.
        a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp).Select(j),
                 dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
      }
      // Convert to fixed-point.
      a_.OpFToU(j ? temp1_dest : packed_dest_low, temp1_src);
      // Pack the upper components.
      if (j) {
        a_.OpBFI(packed_dest_low, dxbc::Src::LU(8), dxbc::Src::LU(j * 8),
                 temp1_src, packed_src_low);
      }
    }
    a_.OpBreak();
  }

  // ***************************************************************************
  // k_2_10_10_10
  // k_2_10_10_10_AS_10_10_10_10
  // ***************************************************************************
  a_.OpCase(dxbc::Src::LU(
      ROV_AddColorFormatFlags(xenos::ColorRenderTargetFormat::k_2_10_10_10)));
  a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)));
  for (uint32_t i = 0; i < 4; ++i) {
    // Denormalize and convert to fixed-point.
    a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp).Select(i),
             dxbc::Src::LF(i < 3 ? 1023.0f : 3.0f), dxbc::Src::LF(0.5f));
    a_.OpFToU(i ? temp1_dest : packed_dest_low, temp1_src);
    // Pack the upper components.
    if (i) {
      a_.OpBFI(packed_dest_low, dxbc::Src::LU(i < 3 ? 10 : 2),
               dxbc::Src::LU(i * 10), temp1_src, packed_src_low);
    }
  }
  a_.OpBreak();

  // ***************************************************************************
  // k_2_10_10_10_FLOAT
  // k_2_10_10_10_FLOAT_AS_16_16_16_16
  // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp
  // ***************************************************************************
  a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)));
  a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)));
  {
    for (uint32_t i = 0; i < 3; ++i) {
      dxbc::Src color_component_src(dxbc::Src::R(color_temp).Select(i));
      // Check if the number is too small to be represented as normalized 7e3.
      // temp2 = f32 < 2^-2
      a_.OpULT(temp2_dest, color_component_src, dxbc::Src::LU(0x3E800000));
      // Handle denormalized numbers separately.
      a_.OpIf(true, temp2_src);
      {
        // temp2 = f32 >> 23
        a_.OpUShR(temp2_dest, color_component_src, dxbc::Src::LU(23));
        // temp2 = 125 - (f32 >> 23)
        a_.OpIAdd(temp2_dest, dxbc::Src::LI(125), -temp2_src);
        // Don't allow the shift to overflow, since in DXBC the lower 5 bits of
        // the shift amount are used.
        // temp2 = min(125 - (f32 >> 23), 24)
        a_.OpUMin(temp2_dest, temp2_src, dxbc::Src::LU(24));
        // biased_f32 = (f32 & 0x7FFFFF) | 0x800000
        a_.OpBFI(temp1_dest, dxbc::Src::LU(9), dxbc::Src::LU(23),
                 dxbc::Src::LU(1), color_component_src);
        // biased_f32 =
        //     ((f32 & 0x7FFFFF) | 0x800000) >> min(125 - (f32 >> 23), 24)
        a_.OpUShR(temp1_dest, temp1_src, temp2_src);
      }
      // Not denormalized?
      a_.OpElse();
      {
        // Bias the exponent.
        // biased_f32 = f32 + (-124 << 23)
        // (left shift of a negative value is undefined behavior)
        a_.OpIAdd(temp1_dest, color_component_src, dxbc::Src::LU(0xC2000000u));
      }
      // Close the denormal check.
      a_.OpEndIf();
      // Build the 7e3 number.
      // temp2 = (biased_f32 >> 16) & 1
      a_.OpUBFE(temp2_dest, dxbc::Src::LU(1), dxbc::Src::LU(16), temp1_src);
      // f10 = biased_f32 + 0x7FFF
      a_.OpIAdd(temp1_dest, temp1_src, dxbc::Src::LU(0x7FFF));
      // f10 = biased_f32 + 0x7FFF + ((biased_f32 >> 16) & 1)
      a_.OpIAdd(temp1_dest, temp1_src, temp2_src);
      // f10 = ((biased_f32 + 0x7FFF + ((biased_f32 >> 16) & 1)) >> 16) & 0x3FF
      a_.OpUBFE(i ? temp1_dest : packed_dest_low, dxbc::Src::LU(10),
                dxbc::Src::LU(16), temp1_src);
      // Pack the upper components.
      if (i) {
        a_.OpBFI(packed_dest_low, dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                 temp1_src, packed_src_low);
      }
    }
    // Denormalize the alpha and convert it to fixed-point.
    a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp, dxbc::Src::kWWWW),
             dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
    a_.OpFToU(temp1_dest, temp1_src);
    // Pack the alpha.
    a_.OpBFI(packed_dest_low, dxbc::Src::LU(2), dxbc::Src::LU(30), temp1_src,
             packed_src_low);
  }
  a_.OpBreak();

  // ***************************************************************************
  // k_16_16
  // k_16_16_16_16 (64bpp)
  // ***************************************************************************
  for (uint32_t i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_16_16_16_16
          : xenos::ColorRenderTargetFormat::k_16_16)));
    for (uint32_t j = 0; j < (uint32_t(2) << i); ++j) {
      // Denormalize and convert to fixed-point, making 0.5 with the proper sign
      // in temp2.
      a_.OpGE(temp2_dest, dxbc::Src::R(color_temp).Select(j),
              dxbc::Src::LF(0.0f));
      a_.OpMovC(temp2_dest, temp2_src, dxbc::Src::LF(0.5f),
                dxbc::Src::LF(-0.5f));
      a_.OpMAd(temp1_dest, dxbc::Src::R(color_temp).Select(j),
               dxbc::Src::LF(32767.0f / 32.0f), temp2_src);
      dxbc::Dest packed_dest_half(
          dxbc::Dest::R(packed_temp, 1 << (packed_temp_components + (j >> 1))));
      // Convert to fixed-point.
      a_.OpFToI((j & 1) ? temp1_dest : packed_dest_half, temp1_src);
      // Pack green or alpha.
      if (j & 1) {
        a_.OpBFI(packed_dest_half, dxbc::Src::LU(16), dxbc::Src::LU(16),
                 temp1_src,
                 dxbc::Src::R(packed_temp)
                     .Select(packed_temp_components + (j >> 1)));
      }
    }
    a_.OpBreak();
  }

  // ***************************************************************************
  // k_16_16_FLOAT
  // k_16_16_16_16_FLOAT (64bpp)
  // ***************************************************************************
  for (uint32_t i = 0; i < 2; ++i) {
    a_.OpCase(dxbc::Src::LU(ROV_AddColorFormatFlags(
        i ? xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT
          : xenos::ColorRenderTargetFormat::k_16_16_FLOAT)));
    for (uint32_t j = 0; j < (uint32_t(2) << i); ++j) {
      dxbc::Dest packed_dest_half(
          dxbc::Dest::R(packed_temp, 1 << (packed_temp_components + (j >> 1))));
      // Convert to 16-bit float.
      a_.OpF32ToF16((j & 1) ? temp1_dest : packed_dest_half,
                    dxbc::Src::R(color_temp).Select(j));
      // Pack green or alpha.
      if (j & 1) {
        a_.OpBFI(packed_dest_half, dxbc::Src::LU(16), dxbc::Src::LU(16),
                 temp1_src,
                 dxbc::Src::R(packed_temp)
                     .Select(packed_temp_components + (j >> 1)));
      }
    }
    a_.OpBreak();
  }

  if (packed_temp != color_temp) {
    // Assume k_32_FLOAT or k_32_32_FLOAT for the rest.
    a_.OpDefault();
    a_.OpMov(dxbc::Dest::R(packed_temp, 0b11 << packed_temp_components),
             dxbc::Src::R(color_temp, 0b0100 << (packed_temp_components * 2)));
    a_.OpBreak();
  }

  a_.OpEndSwitch();
}

void DxbcShaderTranslator::ROV_HandleColorBlendFactorCases(
    uint32_t src_temp, uint32_t dst_temp, uint32_t factor_temp) {
  dxbc::Dest factor_dest(dxbc::Dest::R(factor_temp, 0b0111));
  dxbc::Src one_src(dxbc::Src::LF(1.0f));

  // kOne.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOne)));
  a_.OpMov(factor_dest, one_src);
  a_.OpBreak();

  // kSrcColor
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kSrcColor)));
  if (factor_temp != src_temp) {
    a_.OpMov(factor_dest, dxbc::Src::R(src_temp));
  }
  a_.OpBreak();

  // kOneMinusSrcColor
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusSrcColor)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(src_temp));
  a_.OpBreak();

  // kSrcAlpha
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kSrcAlpha)));
  a_.OpMov(factor_dest, dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kOneMinusSrcAlpha
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusSrcAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kDstColor
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kDstColor)));
  if (factor_temp != dst_temp) {
    a_.OpMov(factor_dest, dxbc::Src::R(dst_temp));
  }
  a_.OpBreak();

  // kOneMinusDstColor
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusDstColor)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(dst_temp));
  a_.OpBreak();

  // kDstAlpha
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kDstAlpha)));
  a_.OpMov(factor_dest, dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kOneMinusDstAlpha
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusDstAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  // Factors involving the constant.
  system_constants_used_ |= 1ull << kSysConst_EdramBlendConstant_Index;

  // kConstantColor
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kConstantColor)));
  a_.OpMov(factor_dest,
           dxbc::Src::CB(cbuffer_index_system_constants_,
                         uint32_t(CbufferRegister::kSystemConstants),
                         kSysConst_EdramBlendConstant_Vec));
  a_.OpBreak();

  // kOneMinusConstantColor
  a_.OpCase(
      dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusConstantColor)));
  a_.OpAdd(factor_dest, one_src,
           -dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramBlendConstant_Vec));
  a_.OpBreak();

  // kConstantAlpha
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kConstantAlpha)));
  a_.OpMov(factor_dest,
           dxbc::Src::CB(cbuffer_index_system_constants_,
                         uint32_t(CbufferRegister::kSystemConstants),
                         kSysConst_EdramBlendConstant_Vec, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kOneMinusConstantAlpha
  a_.OpCase(
      dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusConstantAlpha)));
  a_.OpAdd(factor_dest, one_src,
           -dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramBlendConstant_Vec, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kSrcAlphaSaturate
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kSrcAlphaSaturate)));
  a_.OpAdd(dxbc::Dest::R(factor_temp, 0b0001), one_src,
           -dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpMin(factor_dest, dxbc::Src::R(src_temp, dxbc::Src::kWWWW),
           dxbc::Src::R(factor_temp, dxbc::Src::kXXXX));
  a_.OpBreak();

  // kZero default.
  a_.OpDefault();
  a_.OpMov(factor_dest, dxbc::Src::LF(0.0f));
  a_.OpBreak();
}

void DxbcShaderTranslator::ROV_HandleAlphaBlendFactorCases(
    uint32_t src_temp, uint32_t dst_temp, uint32_t factor_temp,
    uint32_t factor_component) {
  dxbc::Dest factor_dest(dxbc::Dest::R(factor_temp, 1 << factor_component));
  dxbc::Src one_src(dxbc::Src::LF(1.0f));

  // kOne, kSrcAlphaSaturate.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOne)));
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kSrcAlphaSaturate)));
  a_.OpMov(factor_dest, one_src);
  a_.OpBreak();

  // kSrcColor, kSrcAlpha.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kSrcColor)));
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kSrcAlpha)));
  if (factor_temp != src_temp || factor_component != 3) {
    a_.OpMov(factor_dest, dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  }
  a_.OpBreak();

  // kOneMinusSrcColor, kOneMinusSrcAlpha.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusSrcColor)));
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusSrcAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(src_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kDstColor, kDstAlpha.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kDstColor)));
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kDstAlpha)));
  if (factor_temp != dst_temp || factor_component != 3) {
    a_.OpMov(factor_dest, dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  }
  a_.OpBreak();

  // kOneMinusDstColor, kOneMinusDstAlpha.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusDstColor)));
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusDstAlpha)));
  a_.OpAdd(factor_dest, one_src, -dxbc::Src::R(dst_temp, dxbc::Src::kWWWW));
  a_.OpBreak();

  // Factors involving the constant.
  system_constants_used_ |= 1ull << kSysConst_EdramBlendConstant_Index;

  // kConstantColor, kConstantAlpha.
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kConstantColor)));
  a_.OpCase(dxbc::Src::LU(uint32_t(xenos::BlendFactor::kConstantAlpha)));
  a_.OpMov(factor_dest,
           dxbc::Src::CB(cbuffer_index_system_constants_,
                         uint32_t(CbufferRegister::kSystemConstants),
                         kSysConst_EdramBlendConstant_Vec, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kOneMinusConstantColor, kOneMinusConstantAlpha.
  a_.OpCase(
      dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusConstantColor)));
  a_.OpCase(
      dxbc::Src::LU(uint32_t(xenos::BlendFactor::kOneMinusConstantAlpha)));
  a_.OpAdd(factor_dest, one_src,
           -dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_EdramBlendConstant_Vec, dxbc::Src::kWWWW));
  a_.OpBreak();

  // kZero default.
  a_.OpDefault();
  a_.OpMov(factor_dest, dxbc::Src::LF(0.0f));
  a_.OpBreak();
}

void DxbcShaderTranslator::CompletePixelShader_WriteToRTVs_AlphaToMask() {
  // Check if alpha to coverage can be done at all in this shader.
  if (!current_shader().writes_color_target(0)) {
    return;
  }

  // Check if alpha to coverage is enabled.
  system_constants_used_ |= 1ull << kSysConst_AlphaToMask_Index;
  a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                              uint32_t(CbufferRegister::kSystemConstants),
                              kSysConst_AlphaToMask_Vec)
                    .Select(kSysConst_AlphaToMask_Comp));

  uint32_t temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src temp_y_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
  dxbc::Dest temp_z_dest(dxbc::Dest::R(temp, 0b0100));
  dxbc::Src temp_z_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));

  // Convert SSAA sample position to integer to temp.xy (not caring about the
  // resolution scale because it's not supported anywhere on the RTV output
  // path).
  in_position_used_ |= 0b0011;
  a_.OpFToU(dxbc::Dest::R(temp, 0b0011),
            dxbc::Src::V(uint32_t(InOutRegister::kPSInPosition)));

  // Check if SSAA is enabled.
  system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
  a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                              uint32_t(CbufferRegister::kSystemConstants),
                              kSysConst_SampleCountLog2_Vec)
                    .Select(kSysConst_SampleCountLog2_Comp + 1));
  {
    // Check if SSAA is 4x or 2x.
    system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
    a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                                uint32_t(CbufferRegister::kSystemConstants),
                                kSysConst_SampleCountLog2_Vec)
                      .Select(kSysConst_SampleCountLog2_Comp));
    {
      // 4x SSAA.
      // Build the sample index in temp.z where X is the low bit and Y is the
      // high bit, for calculation of the dithering base according to the sample
      // position (left/right and top/bottom).
      a_.OpAnd(temp_z_dest, temp_y_src, dxbc::Src::LU(1));
      a_.OpBFI(temp_z_dest, dxbc::Src::LU(31), dxbc::Src::LU(1), temp_z_src,
               temp_x_src);
      // Top-left sample base: 0.75.
      // Top-right sample base: 0.5.
      // Bottom-left sample base: 0.25.
      // Bottom-right sample base: 1.0.
      // The threshold would be 1 - frac(0.25 + 0.25 * (x | (y << 1))) - offset.
      // Multiplication here will result in exactly 1 (power of 2 multiplied by
      // an integer).
      // Calculate the base.
      a_.OpUToF(temp_z_dest, temp_z_src);
      a_.OpMAd(temp_z_dest, temp_z_src, dxbc::Src::LF(0.25f),
               dxbc::Src::LF(0.25f));
      a_.OpFrc(temp_z_dest, temp_z_src);
      a_.OpAdd(temp_z_dest, dxbc::Src::LF(1.0f), -temp_z_src);
      // Get the dithering threshold offset index for the guest pixel to temp.x,
      // Y - low bit of offset index, X - high bit.
      a_.OpUBFE(dxbc::Dest::R(temp, 0b0011), dxbc::Src::LU(1), dxbc::Src::LU(1),
                dxbc::Src::R(temp));
      a_.OpBFI(temp_x_dest, dxbc::Src::LU(1), dxbc::Src::LU(1), temp_x_src,
               temp_y_src);
      // Write the offset scale to temp.y.
      a_.OpMov(temp_y_dest, dxbc::Src::LF(-1.0f / 16.0f));
    }
    a_.OpElse();
    {
      // 2x SSAA.
      // Check if the top (base 0.5) or the bottom (base 1.0) sample to temp.z,
      // and also extract the guest pixel Y parity to temp.y.
      a_.OpUBFE(dxbc::Dest::R(temp, 0b0110), dxbc::Src::LU(1),
                dxbc::Src::LU(0, 1, 0, 0), temp_y_src);
      a_.OpMovC(temp_z_dest, temp_z_src, dxbc::Src::LF(1.0f),
                dxbc::Src::LF(0.5f));
      // Get the dithering threshold offset index for the guest pixel to temp.x,
      // Y - low bit of offset index, X - high bit.
      a_.OpBFI(temp_x_dest, dxbc::Src::LU(1), dxbc::Src::LU(1), temp_x_src,
               temp_y_src);
      // Write the offset scale to temp.y.
      a_.OpMov(temp_y_dest, dxbc::Src::LF(-1.0f / 8.0f));
    }
    // Close the 4x check.
    a_.OpEndIf();
  }
  // SSAA is disabled.
  a_.OpElse();
  {
    // Write the base 1.0 to temp.z.
    a_.OpMov(temp_z_dest, dxbc::Src::LF(1.0f));
    // Get the dithering threshold offset index for the guest pixel to temp.x,
    // Y - low bit of offset index, X - high bit.
    a_.OpAnd(temp_y_dest, temp_y_src, dxbc::Src::LU(1));
    a_.OpBFI(temp_x_dest, dxbc::Src::LU(1), dxbc::Src::LU(1), temp_x_src,
             temp_y_src);
    // Write the offset scale to temp.y.
    a_.OpMov(temp_y_dest, dxbc::Src::LF(-1.0f / 4.0f));
  }
  // Close the 2x/4x check.
  a_.OpEndIf();

  // Extract the dithering offset to temp.x for the quad pixel index.
  a_.OpIShL(temp_x_dest, temp_x_src, dxbc::Src::LU(1));
  system_constants_used_ |= 1ull << kSysConst_AlphaToMask_Index;
  a_.OpUBFE(temp_x_dest, dxbc::Src::LU(2), temp_x_src,
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_AlphaToMask_Vec)
                .Select(kSysConst_AlphaToMask_Comp));
  a_.OpUToF(temp_x_dest, temp_x_src);
  // Combine the base and the offset to temp.x.
  a_.OpMAd(temp_x_dest, temp_x_src, temp_y_src, temp_z_src);
  // Check if alpha of oC0 is at or greater than the threshold (handling NaN
  // according to the Direct3D 11.3 functional specification, as not covered).
  a_.OpGE(temp_x_dest, dxbc::Src::R(system_temps_color_[0], dxbc::Src::kWWWW),
          temp_x_src);
  // Discard the SSAA sample if it's not covered.
  a_.OpDiscard(false, temp_x_src);

  // Release temp.
  PopSystemTemp();

  // Close the alpha to coverage check.
  a_.OpEndIf();
}

void DxbcShaderTranslator::CompletePixelShader_WriteToRTVs() {
  uint32_t shader_writes_color_targets =
      current_shader().writes_color_targets();
  if (!shader_writes_color_targets) {
    return;
  }

  // Check if this sample needs to be discarded by alpha to coverage.
  CompletePixelShader_WriteToRTVs_AlphaToMask();

  uint32_t gamma_temp = PushSystemTemp();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(shader_writes_color_targets & (1 << i))) {
      continue;
    }
    // Apply the exponent bias after alpha to coverage because it needs the
    // unbiased alpha from the shader
    system_constants_used_ |= 1ull << kSysConst_ColorExpBias_Index;
    a_.OpMul(dxbc::Dest::R(system_temps_color_[i]),
             dxbc::Src::R(system_temps_color_[i]),
             dxbc::Src::CB(cbuffer_index_system_constants_,
                           uint32_t(CbufferRegister::kSystemConstants),
                           kSysConst_ColorExpBias_Vec)
                 .Select(i));
    // Convert to gamma space - this is incorrect, since it must be done after
    // blending on the Xbox 360, but this is just one of many blending issues in
    // the RTV path.
    system_constants_used_ |= 1ull << kSysConst_Flags_Index;
    a_.OpAnd(dxbc::Dest::R(gamma_temp, 0b0001),
             dxbc::Src::CB(cbuffer_index_system_constants_,
                           uint32_t(CbufferRegister::kSystemConstants),
                           kSysConst_Flags_Vec)
                 .Select(kSysConst_Flags_Comp),
             dxbc::Src::LU(kSysFlag_Color0Gamma << i));
    a_.OpIf(true, dxbc::Src::R(gamma_temp, dxbc::Src::kXXXX));
    for (uint32_t j = 0; j < 3; ++j) {
      ConvertPWLGamma(true, system_temps_color_[i], j, system_temps_color_[i],
                      j, gamma_temp, 0, gamma_temp, 1);
    }
    a_.OpEndIf();
  }
  // Release gamma_temp.
  PopSystemTemp();

  // Remap guest render target indices to host since because on the host, the
  // indices of the bound render targets are consecutive. This is done using 16
  // movc instructions because indexable temps are known to be causing
  // performance issues on some Nvidia GPUs. In the map, the components are host
  // render target indices, and the values are the guest ones.
  uint32_t remap_movc_mask_temp = PushSystemTemp();
  uint32_t remap_movc_target_temp = PushSystemTemp();
  system_constants_used_ |= 1ull << kSysConst_ColorOutputMap_Index;
  // Host RT i, guest RT j.
  for (uint32_t i = 0; i < 4; ++i) {
    // mask = map.iiii == (0, 1, 2, 3)
    a_.OpIEq(dxbc::Dest::R(remap_movc_mask_temp, shader_writes_color_targets),
             dxbc::Src::CB(cbuffer_index_system_constants_,
                           uint32_t(CbufferRegister::kSystemConstants),
                           kSysConst_ColorOutputMap_Vec)
                 .Select(i),
             dxbc::Src::LU(0, 1, 2, 3));
    bool guest_rt_first = true;
    for (uint32_t j = 0; j < 4; ++j) {
      // If map.i == j, move guest color j to the temporary host color.
      if (!(shader_writes_color_targets & (1 << j))) {
        continue;
      }
      a_.OpMovC(dxbc::Dest::R(remap_movc_target_temp),
                dxbc::Src::R(remap_movc_mask_temp).Select(j),
                dxbc::Src::R(system_temps_color_[j]),
                guest_rt_first ? dxbc::Src::LF(0.0f)
                               : dxbc::Src::R(remap_movc_target_temp));
      guest_rt_first = false;
    }
    // Write the remapped color to host render target i.
    a_.OpMov(dxbc::Dest::O(i), dxbc::Src::R(remap_movc_target_temp));
  }
  // Release remap_movc_mask_temp and remap_movc_target_temp.
  PopSystemTemp(2);
}

void DxbcShaderTranslator::CompletePixelShader_DSV_DepthTo24Bit() {
  if (!DSV_IsWritingFloat24Depth()) {
    return;
  }

  bool shader_writes_depth = current_shader().writes_depth();

  uint32_t temp;
  if (shader_writes_depth) {
    // The depth is already written to system_temp_depth_stencil_.x and clamped
    // to 0...1 with NaNs dropped (saturating in StoreResult); yzw are free.
    temp = system_temp_depth_stencil_;
  } else {
    // Need a temporary variable; copy the sample's depth input to it and
    // saturate it (in Direct3D 11, depth is clamped to the viewport bounds
    // after the pixel shader, and SV_Position.z contains the unclamped depth,
    // which may be outside the viewport's depth range if it's biased); though
    // it will be clamped to the viewport bounds anyway, but to be able to make
    // the assumption of it being clamped while working with the bit
    // representation.
    temp = PushSystemTemp();
    in_position_used_ |= 0b0100;
    a_.OpMov(
        dxbc::Dest::R(temp, 0b0001),
        dxbc::Src::V(uint32_t(InOutRegister::kPSInPosition), dxbc::Src::kZZZZ),
        true);
  }

  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src temp_y_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));

  if (GetDxbcShaderModification().depth_stencil_mode ==
      Modification::DepthStencilMode::kFloat24Truncating) {
    // Simplified conversion, always less than or equal to the original value -
    // just drop the lower bits.
    // The float32 exponent bias is 127.
    // After saturating, the exponent range is -127...0.
    // The smallest normalized 20e4 exponent is -14 - should drop 3 mantissa
    // bits at -14 or above.
    // The smallest denormalized 20e4 number is -34 - should drop 23 mantissa
    // bits at -34.
    // Anything smaller than 2^-34 becomes 0.
    dxbc::Dest truncate_dest(shader_writes_depth ? dxbc::Dest::ODepth()
                                                 : dxbc::Dest::ODepthLE());
    // Check if the number is representable as a float24 after truncation - the
    // exponent is at least -34.
    a_.OpUGE(temp_y_dest, temp_x_src, dxbc::Src::LU(0x2E800000));
    a_.OpIf(true, temp_y_src);
    {
      // Extract the biased float32 exponent to temp.y.
      // temp.y = 113+ at exponent -14+.
      // temp.y = 93 at exponent -34.
      a_.OpUBFE(temp_y_dest, dxbc::Src::LU(8), dxbc::Src::LU(23), temp_x_src);
      // Convert exponent to the unclamped number of bits to truncate.
      // 116 - 113 = 3.
      // 116 - 93 = 23.
      // temp.y = 3+ at exponent -14+.
      // temp.y = 23 at exponent -34.
      a_.OpIAdd(temp_y_dest, dxbc::Src::LI(116), -temp_y_src);
      // Clamp the truncated bit count to drop 3 bits of any normal number.
      // Exponents below -34 are handled separately.
      // temp.y = 3 at exponent -14.
      // temp.y = 23 at exponent -34.
      a_.OpIMax(temp_y_dest, temp_y_src, dxbc::Src::LI(3));
      // Truncate the mantissa - fill the low bits with zeros.
      a_.OpBFI(truncate_dest, temp_y_src, dxbc::Src::LU(0), dxbc::Src::LU(0),
               temp_x_src);
    }
    // The number is not representable as float24 after truncation - zero.
    a_.OpElse();
    a_.OpMov(truncate_dest, dxbc::Src::LF(0.0f));
    // Close the non-zero result check.
    a_.OpEndIf();
  } else {
    // Properly convert to 20e4, with rounding to the nearest even.
    PreClampedDepthTo20e4(temp, 0, temp, 0, temp, 1);
    // Convert back to float32.
    // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp
    // Unpack the exponent to temp.y.
    a_.OpUShR(temp_y_dest, temp_x_src, dxbc::Src::LU(20));
    // Unpack the mantissa to temp.x.
    a_.OpAnd(temp_x_dest, temp_x_src, dxbc::Src::LU(0xFFFFF));
    // Check if the number is denormalized.
    a_.OpIf(false, temp_y_src);
    {
      // Check if the number is non-zero (if the mantissa isn't zero - the
      // exponent is known to be zero at this point).
      a_.OpIf(true, temp_x_src);
      {
        // Normalize the mantissa.
        // Note that HLSL firstbithigh(x) is compiled to DXBC like:
        // `x ? 31 - firstbit_hi(x) : -1`
        // (returns the index from the LSB, not the MSB, but -1 for zero too).
        // temp.y = firstbit_hi(mantissa)
        a_.OpFirstBitHi(temp_y_dest, temp_x_src);
        // temp.y = 20 - firstbithigh(mantissa)
        // Or:
        // temp.y = 20 - (31 - firstbit_hi(mantissa))
        a_.OpIAdd(temp_y_dest, temp_y_src, dxbc::Src::LI(20 - 31));
        // mantissa = mantissa << (20 - firstbithigh(mantissa))
        // AND 0xFFFFF not needed after this - BFI will do it.
        a_.OpIShL(temp_x_dest, temp_x_src, temp_y_src);
        // Get the normalized exponent.
        // exponent = 1 - (20 - firstbithigh(mantissa))
        a_.OpIAdd(temp_y_dest, dxbc::Src::LI(1), -temp_y_src);
      }
      // The number is zero.
      a_.OpElse();
      {
        // Set the unbiased exponent to -112 for zero - 112 will be added later,
        // resulting in zero float32.
        a_.OpMov(temp_y_dest, dxbc::Src::LI(-112));
      }
      // Close the non-zero check.
      a_.OpEndIf();
    }
    // Close the denormal check.
    a_.OpEndIf();
    // Bias the exponent and move it to the correct location in float32 to
    // temp.y.
    a_.OpIMAd(temp_y_dest, temp_y_src, dxbc::Src::LI(1 << 23),
              dxbc::Src::LI(112 << 23));
    // Combine the mantissa and the exponent into the result.
    a_.OpBFI(dxbc::Dest::ODepth(), dxbc::Src::LU(20), dxbc::Src::LU(3),
             temp_x_src, temp_y_src);
  }

  if (!shader_writes_depth) {
    // Release temp.
    PopSystemTemp();
  }
}

void DxbcShaderTranslator::CompletePixelShader_ROV_AlphaToMaskSample(
    uint32_t sample_index, float threshold_base, dxbc::Src threshold_offset,
    float threshold_offset_scale, uint32_t temp, uint32_t temp_component) {
  dxbc::Dest temp_dest(dxbc::Dest::R(temp, 1 << temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp).Select(temp_component));
  // Calculate the threshold.
  a_.OpMAd(temp_dest, threshold_offset, dxbc::Src::LF(-threshold_offset_scale),
           dxbc::Src::LF(threshold_base));
  // Check if alpha of oC0 is at or greater than the threshold (handling NaN
  // according to the Direct3D 11.3 functional specification, as not covered).
  a_.OpGE(temp_dest, dxbc::Src::R(system_temps_color_[0], dxbc::Src::kWWWW),
          temp_src);
  // Keep all bits in system_temp_rov_params_.x but the ones that need to be
  // removed in case of failure (coverage and deferred depth/stencil write are
  // removed).
  a_.OpOr(temp_dest, temp_src,
          dxbc::Src::LU(~(uint32_t(0b00010001) << sample_index)));
  // Clear the coverage for samples that have failed the test.
  a_.OpAnd(dxbc::Dest::R(system_temp_rov_params_, 0b0001),
           dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX), temp_src);
}

void DxbcShaderTranslator::CompletePixelShader_ROV_AlphaToMask() {
  // Check if alpha to coverage can be done at all in this shader.
  if (!current_shader().writes_color_target(0)) {
    return;
  }

  // Check if alpha to coverage is enabled.
  system_constants_used_ |= 1ull << kSysConst_AlphaToMask_Index;
  a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                              uint32_t(CbufferRegister::kSystemConstants),
                              kSysConst_AlphaToMask_Vec)
                    .Select(kSysConst_AlphaToMask_Comp));

  uint32_t temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));

  // Get the dithering threshold offset index for the pixel, Y - low bit of
  // offset index, X - high bit, and extract the offset and convert it to
  // floating-point. With resolution scaling, still using host pixels, to
  // preserve the idea of dithering.
  // temp.x = alpha to coverage offset as float 0.0...3.0.
  in_position_used_ |= 0b0011;
  a_.OpFToU(dxbc::Dest::R(temp, 0b0011),
            dxbc::Src::V(uint32_t(InOutRegister::kPSInPosition)));
  a_.OpAnd(dxbc::Dest::R(temp, 0b0010), dxbc::Src::R(temp, dxbc::Src::kYYYY),
           dxbc::Src::LU(1));
  a_.OpBFI(temp_x_dest, dxbc::Src::LU(1), dxbc::Src::LU(1), temp_x_src,
           dxbc::Src::R(temp, dxbc::Src::kYYYY));
  a_.OpIShL(temp_x_dest, temp_x_src, dxbc::Src::LU(1));
  system_constants_used_ |= 1ull << kSysConst_AlphaToMask_Index;
  a_.OpUBFE(temp_x_dest, dxbc::Src::LU(2), temp_x_src,
            dxbc::Src::CB(cbuffer_index_system_constants_,
                          uint32_t(CbufferRegister::kSystemConstants),
                          kSysConst_AlphaToMask_Vec)
                .Select(kSysConst_AlphaToMask_Comp));
  a_.OpUToF(temp_x_dest, temp_x_src);

  // The test must effect not only the coverage bits, but also the deferred
  // depth/stencil write bits since the coverage is zeroed for samples that have
  // failed the depth/stencil test, but stencil may still require writing - but
  // if the sample is discarded by alpha to coverage, it must not be written at
  // all.

  // Check if MSAA is enabled.
  system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
  a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                              uint32_t(CbufferRegister::kSystemConstants),
                              kSysConst_SampleCountLog2_Vec)
                    .Select(kSysConst_SampleCountLog2_Comp + 1));
  {
    // Check if MSAA is 4x or 2x.
    system_constants_used_ |= 1ull << kSysConst_SampleCountLog2_Index;
    a_.OpIf(true, dxbc::Src::CB(cbuffer_index_system_constants_,
                                uint32_t(CbufferRegister::kSystemConstants),
                                kSysConst_SampleCountLog2_Vec)
                      .Select(kSysConst_SampleCountLog2_Comp));
    // 4x MSAA.
    CompletePixelShader_ROV_AlphaToMaskSample(0, 0.75f, temp_x_src,
                                              1.0f / 16.0f, temp, 1);
    CompletePixelShader_ROV_AlphaToMaskSample(1, 0.25f, temp_x_src,
                                              1.0f / 16.0f, temp, 1);
    CompletePixelShader_ROV_AlphaToMaskSample(2, 0.5f, temp_x_src, 1.0f / 16.0f,
                                              temp, 1);
    CompletePixelShader_ROV_AlphaToMaskSample(3, 1.0f, temp_x_src, 1.0f / 16.0f,
                                              temp, 1);
    // 2x MSAA.
    a_.OpElse();
    CompletePixelShader_ROV_AlphaToMaskSample(0, 0.5f, temp_x_src, 1.0f / 8.0f,
                                              temp, 1);
    CompletePixelShader_ROV_AlphaToMaskSample(1, 1.0f, temp_x_src, 1.0f / 8.0f,
                                              temp, 1);
    // Close the 4x check.
    a_.OpEndIf();
  }
  // MSAA is disabled.
  a_.OpElse();
  CompletePixelShader_ROV_AlphaToMaskSample(0, 1.0f, temp_x_src, 1.0f / 4.0f,
                                            temp, 1);
  // Close the 2x/4x check.
  a_.OpEndIf();

  // Check if any sample is still covered (the mask includes both 0:3 and 4:7
  // parts because there may be samples which passed alpha to coverage, but not
  // stencil test, and the stencil buffer needs to be modified - in this case,
  // samples would be dropped in 0:3, but not in 4:7).
  a_.OpAnd(temp_x_dest, dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
           dxbc::Src::LU(0b11111111));
  a_.OpRetC(false, temp_x_src);

  // Release temp.
  PopSystemTemp();

  // Close the alpha to coverage check.
  a_.OpEndIf();
}

void DxbcShaderTranslator::CompletePixelShader_WriteToROV() {
  // Discard samples with alpha to coverage.
  CompletePixelShader_ROV_AlphaToMask();

  uint32_t temp = PushSystemTemp();
  dxbc::Dest temp_x_dest(dxbc::Dest::R(temp, 0b0001));
  dxbc::Src temp_x_src(dxbc::Src::R(temp, dxbc::Src::kXXXX));
  dxbc::Dest temp_y_dest(dxbc::Dest::R(temp, 0b0010));
  dxbc::Src temp_y_src(dxbc::Src::R(temp, dxbc::Src::kYYYY));
  dxbc::Dest temp_z_dest(dxbc::Dest::R(temp, 0b0100));
  dxbc::Src temp_z_src(dxbc::Src::R(temp, dxbc::Src::kZZZZ));
  dxbc::Dest temp_w_dest(dxbc::Dest::R(temp, 0b1000));
  dxbc::Src temp_w_src(dxbc::Src::R(temp, dxbc::Src::kWWWW));

  // Do late depth/stencil test (which includes writing) if needed or deferred
  // depth writing.
  if (ROV_IsDepthStencilEarly()) {
    // Write modified depth/stencil.
    for (uint32_t i = 0; i < 4; ++i) {
      // Get if need to write to temp.x.
      // temp.x = whether the depth sample needs to be written.
      a_.OpAnd(temp_x_dest,
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::LU(1 << (4 + i)));
      // Check if need to write.
      // temp.x = free.
      a_.OpIf(true, temp_x_src);
      {
        // Write the new depth/stencil.
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(
            dxbc::Dest::U(uav_index_edram_, uint32_t(UAVRegister::kEdram)),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY), 1,
            dxbc::Src::R(system_temp_depth_stencil_).Select(i));
      }
      // Close the write check.
      a_.OpEndIf();
      // Go to the next sample (samples are at +0, +80, +1, +81, so need to do
      // +80, -79, +80 and -81 after each sample).
      if (i < 3) {
        system_constants_used_ |= 1ull
                                  << kSysConst_EdramResolutionSquareScale_Index;
        a_.OpIMAd(dxbc::Dest::R(system_temp_rov_params_, 0b0010),
                  dxbc::Src::LI((i & 1) ? -78 - i : 80),
                  dxbc::Src::CB(cbuffer_index_system_constants_,
                                uint32_t(CbufferRegister::kSystemConstants),
                                kSysConst_EdramResolutionSquareScale_Vec)
                      .Select(kSysConst_EdramResolutionSquareScale_Comp),
                  dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kYYYY));
      }
    }
  } else {
    ROV_DepthStencilTest();
  }

  if (!is_depth_only_pixel_shader_) {
    // Check if any sample is still covered after depth testing and writing,
    // skip color writing completely in this case.
    // temp.x = whether any sample is still covered.
    a_.OpAnd(temp_x_dest,
             dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
             dxbc::Src::LU(0b1111));
    // temp.x = free.
    a_.OpRetC(false, temp_x_src);
  }

  // Write color values.
  uint32_t shader_writes_color_targets =
      current_shader().writes_color_targets();
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(shader_writes_color_targets & (1 << i))) {
      continue;
    }

    dxbc::Src keep_mask_vec_src(
        dxbc::Src::CB(cbuffer_index_system_constants_,
                      uint32_t(CbufferRegister::kSystemConstants),
                      kSysConst_EdramRTKeepMask_Vec + (i >> 1)));
    uint32_t keep_mask_component = (i & 1) * 2;
    uint32_t keep_mask_swizzle = keep_mask_component * 0b0101 + 0b0100;

    // Check if color writing is disabled - special keep mask constant case,
    // both 32bpp parts are forced UINT32_MAX, but also check whether the shader
    // has written anything to this target at all.

    // Combine both parts of the keep mask to check if both are 0xFFFFFFFF.
    // temp.x = whether all bits need to be kept.
    system_constants_used_ |= 1ull << kSysConst_EdramRTKeepMask_Index;
    a_.OpAnd(temp_x_dest, keep_mask_vec_src.Select(keep_mask_component),
             keep_mask_vec_src.Select(keep_mask_component + 1));
    // Flip the bits so both UINT32_MAX would result in 0 - not writing.
    // temp.x = whether any bits need to be written.
    a_.OpNot(temp_x_dest, temp_x_src);
    // Get the bits that will be used for checking wherther the render target
    // has been written to on the taken execution path - if the write mask is
    // empty, AND zero with the test bit to always get zero.
    // temp.x = bits for checking whether the render target has been written to.
    a_.OpMovC(temp_x_dest, temp_x_src,
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
              dxbc::Src::LU(0));
    // Check if the render target was written to on the execution path.
    // temp.x = whether anything was written and needs to be stored.
    a_.OpAnd(temp_x_dest, temp_x_src, dxbc::Src::LU(1 << (8 + i)));
    // Check if need to write anything to the render target.
    // temp.x = free.
    a_.OpIf(true, temp_x_src);

    // Apply the exponent bias after alpha to coverage because it needs the
    // unbiased alpha from the shader.
    system_constants_used_ |= 1ull << kSysConst_ColorExpBias_Index;
    a_.OpMul(dxbc::Dest::R(system_temps_color_[i]),
             dxbc::Src::R(system_temps_color_[i]),
             dxbc::Src::CB(cbuffer_index_system_constants_,
                           uint32_t(CbufferRegister::kSystemConstants),
                           kSysConst_ColorExpBias_Vec)
                 .Select(i));

    // Add the EDRAM bases of the render target to system_temp_rov_params_.zw.
    system_constants_used_ |= 1ull << kSysConst_EdramRTBaseDwordsScaled_Index;
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b1100),
              dxbc::Src::R(system_temp_rov_params_),
              dxbc::Src::CB(cbuffer_index_system_constants_,
                            uint32_t(CbufferRegister::kSystemConstants),
                            kSysConst_EdramRTBaseDwordsScaled_Vec)
                  .Select(i));

    dxbc::Src rt_blend_factors_ops_src(
        dxbc::Src::CB(cbuffer_index_system_constants_,
                      uint32_t(CbufferRegister::kSystemConstants),
                      kSysConst_EdramRTBlendFactorsOps_Vec)
            .Select(i));
    dxbc::Src rt_clamp_vec_src(
        dxbc::Src::CB(cbuffer_index_system_constants_,
                      uint32_t(CbufferRegister::kSystemConstants),
                      kSysConst_EdramRTClamp_Vec + i));
    // Get if not blending to pack the color once for all 4 samples.
    // temp.x = whether blending is disabled.
    system_constants_used_ |= 1ull << kSysConst_EdramRTBlendFactorsOps_Index;
    a_.OpIEq(temp_x_dest, rt_blend_factors_ops_src, dxbc::Src::LU(0x00010001));
    // Check if not blending.
    // temp.x = free.
    a_.OpIf(true, temp_x_src);
    {
      // Clamp the color to the render target's representable range - will be
      // packed.
      system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
      a_.OpMax(dxbc::Dest::R(system_temps_color_[i]),
               dxbc::Src::R(system_temps_color_[i]),
               rt_clamp_vec_src.Swizzle(0b01000000));
      a_.OpMin(dxbc::Dest::R(system_temps_color_[i]),
               dxbc::Src::R(system_temps_color_[i]),
               rt_clamp_vec_src.Swizzle(0b11101010));
      // Pack the color once if blending.
      // temp.xy = packed color.
      ROV_PackPreClampedColor(i, system_temps_color_[i], temp, 0, temp, 2, temp,
                              3);
    }
    // Blending is enabled.
    a_.OpElse();
    {
      // Get if the blending source color is fixed-point for clamping if it is.
      // temp.x = whether color is fixed-point.
      system_constants_used_ |= 1ull << kSysConst_EdramRTFormatFlags_Index;
      a_.OpAnd(temp_x_dest,
               dxbc::Src::CB(cbuffer_index_system_constants_,
                             uint32_t(CbufferRegister::kSystemConstants),
                             kSysConst_EdramRTFormatFlags_Vec)
                   .Select(i),
               dxbc::Src::LU(kRTFormatFlag_FixedPointColor));
      // Check if the blending source color is fixed-point and needs clamping.
      // temp.x = free.
      a_.OpIf(true, temp_x_src);
      {
        // Clamp the blending source color if needed.
        system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
        a_.OpMax(dxbc::Dest::R(system_temps_color_[i], 0b0111),
                 dxbc::Src::R(system_temps_color_[i]),
                 rt_clamp_vec_src.Select(0));
        a_.OpMin(dxbc::Dest::R(system_temps_color_[i], 0b0111),
                 dxbc::Src::R(system_temps_color_[i]),
                 rt_clamp_vec_src.Select(2));
      }
      // Close the fixed-point color check.
      a_.OpEndIf();

      // Get if the blending source alpha is fixed-point for clamping if it is.
      // temp.x = whether alpha is fixed-point.
      system_constants_used_ |= 1ull << kSysConst_EdramRTFormatFlags_Index;
      a_.OpAnd(temp_x_dest,
               dxbc::Src::CB(cbuffer_index_system_constants_,
                             uint32_t(CbufferRegister::kSystemConstants),
                             kSysConst_EdramRTFormatFlags_Vec)
                   .Select(i),
               dxbc::Src::LU(kRTFormatFlag_FixedPointAlpha));
      // Check if the blending source alpha is fixed-point and needs clamping.
      // temp.x = free.
      a_.OpIf(true, temp_x_src);
      {
        // Clamp the blending source alpha if needed.
        system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
        a_.OpMax(dxbc::Dest::R(system_temps_color_[i], 0b1000),
                 dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                 rt_clamp_vec_src.Select(1));
        a_.OpMin(dxbc::Dest::R(system_temps_color_[i], 0b1000),
                 dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                 rt_clamp_vec_src.Select(3));
      }
      // Close the fixed-point alpha check.
      a_.OpEndIf();
      // Break register dependency in the color sample raster operation.
      // temp.xy = 0 instead of packed color.
      a_.OpMov(dxbc::Dest::R(temp, 0b0011), dxbc::Src::LU(0));
    }
    a_.OpEndIf();

    dxbc::Src rt_format_flags_src(
        dxbc::Src::CB(cbuffer_index_system_constants_,
                      uint32_t(CbufferRegister::kSystemConstants),
                      kSysConst_EdramRTFormatFlags_Vec)
            .Select(i));

    // Blend, mask and write all samples.
    for (uint32_t j = 0; j < 4; ++j) {
      // Get if the sample is covered.
      // temp.z = whether the sample is covered.
      a_.OpAnd(temp_z_dest,
               dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kXXXX),
               dxbc::Src::LU(1 << j));

      // Check if the sample is covered.
      // temp.z = free.
      a_.OpIf(true, temp_z_src);

      // Only temp.xy are used at this point (containing the packed color from
      // the shader if not blending).

      // ***********************************************************************
      // Color sample raster operation.
      // ***********************************************************************

      // ***********************************************************************
      // Checking if color loading must be done - if any component needs to be
      // kept or if blending is enabled.
      // ***********************************************************************

      // Get if need to keep any components to temp.z.
      // temp.z = whether any components must be kept (OR of keep masks).
      system_constants_used_ |= 1ull << kSysConst_EdramRTKeepMask_Index;
      a_.OpOr(temp_z_dest, keep_mask_vec_src.Select(keep_mask_component),
              keep_mask_vec_src.Select(keep_mask_component + 1));
      // Blending isn't done if it's 1 * source + 0 * destination. But since the
      // previous color also needs to be loaded if any original components need
      // to be kept, force the blend control to something with blending in this
      // case in temp.z.
      // temp.z = blending mode used to check if need to load.
      system_constants_used_ |= 1ull << kSysConst_EdramRTBlendFactorsOps_Index;
      a_.OpMovC(temp_z_dest, temp_z_src, dxbc::Src::LU(0),
                rt_blend_factors_ops_src);
      // Get if the blend control register requires loading the color to temp.z.
      // temp.z = whether need to load the color.
      a_.OpINE(temp_z_dest, temp_z_src, dxbc::Src::LU(0x00010001));
      // Check if need to do something with the previous color.
      // temp.z = free.
      a_.OpIf(true, temp_z_src);
      {
        // *********************************************************************
        // Loading the previous color to temp.zw.
        // *********************************************************************

        // Get if the format is 64bpp to temp.z.
        // temp.z = whether the render target is 64bpp.
        system_constants_used_ |= 1ull << kSysConst_EdramRTFormatFlags_Index;
        a_.OpAnd(temp_z_dest, rt_format_flags_src,
                 dxbc::Src::LU(kRTFormatFlag_64bpp));
        // Check if the format is 64bpp.
        // temp.z = free.
        a_.OpIf(true, temp_z_src);
        {
          // Load the lower 32 bits of the 64bpp color to temp.z.
          // temp.z = lower 32 bits of the packed color.
          if (uav_index_edram_ == kBindingIndexUnallocated) {
            uav_index_edram_ = uav_count_++;
          }
          a_.OpLdUAVTyped(
              temp_z_dest,
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW), 1,
              dxbc::Src::U(uav_index_edram_, uint32_t(UAVRegister::kEdram),
                           dxbc::Src::kXXXX));
          // Get the address of the upper 32 bits of the color to temp.w.
          // temp.w = address of the upper 32 bits of the packed color.
          a_.OpIAdd(temp_w_dest,
                    dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
                    dxbc::Src::LU(1));
          // Load the upper 32 bits of the 64bpp color to temp.w.
          // temp.zw = packed destination color/alpha.
          if (uav_index_edram_ == kBindingIndexUnallocated) {
            uav_index_edram_ = uav_count_++;
          }
          a_.OpLdUAVTyped(
              temp_w_dest, temp_w_src, 1,
              dxbc::Src::U(uav_index_edram_, uint32_t(UAVRegister::kEdram),
                           dxbc::Src::kXXXX));
        }
        // The color is 32bpp.
        a_.OpElse();
        {
          // Load the 32bpp color to temp.z.
          // temp.z = packed 32bpp destination color.
          if (uav_index_edram_ == kBindingIndexUnallocated) {
            uav_index_edram_ = uav_count_++;
          }
          a_.OpLdUAVTyped(
              temp_z_dest,
              dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ), 1,
              dxbc::Src::U(uav_index_edram_, uint32_t(UAVRegister::kEdram),
                           dxbc::Src::kXXXX));
          // Break register dependency in temp.w if the color is 32bpp.
          // temp.zw = packed destination color/alpha.
          a_.OpMov(temp_w_dest, dxbc::Src::LU(0));
        }
        // Close the color format check.
        a_.OpEndIf();

        uint32_t color_temp = PushSystemTemp();
        dxbc::Dest color_temp_rgb_dest(dxbc::Dest::R(color_temp, 0b0111));
        dxbc::Dest color_temp_a_dest(dxbc::Dest::R(color_temp, 0b1000));
        dxbc::Src color_temp_src(dxbc::Src::R(color_temp));
        dxbc::Src color_temp_a_src(dxbc::Src::R(color_temp, dxbc::Src::kWWWW));

        // Get if blending is enabled to color_temp.x.
        // color_temp.x = whether blending is enabled.
        system_constants_used_ |= 1ull
                                  << kSysConst_EdramRTBlendFactorsOps_Index;
        a_.OpINE(dxbc::Dest::R(color_temp, 0b0001), rt_blend_factors_ops_src,
                 dxbc::Src::LU(0x00010001));
        // Check if need to blend.
        // color_temp.x = free.
        a_.OpIf(true, dxbc::Src::R(color_temp, dxbc::Src::kXXXX));
        {
          // Now, when blending is enabled, temp.xy are used as scratch since
          // the color is packed after blending.

          // Unpack the destination color to color_temp, using temp.xy as temps.
          // The destination color never needs clamping because out-of-range
          // values can't be loaded.
          // color_temp.xyzw = destination color/alpha.
          ROV_UnpackColor(i, temp, 2, color_temp, temp, 0, temp, 1);

          // *******************************************************************
          // Color blending.
          // *******************************************************************

          // Extract the color min/max bit to temp.x.
          // temp.x = whether min/max should be used for color.
          system_constants_used_ |= 1ull
                                    << kSysConst_EdramRTBlendFactorsOps_Index;
          a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                   dxbc::Src::LU(1 << (5 + 1)));
          // Check if need to do blend the color with factors.
          // temp.x = free.
          a_.OpIf(false, temp_x_src);
          {
            uint32_t blend_src_temp = PushSystemTemp();
            dxbc::Dest blend_src_temp_rgb_dest(
                dxbc::Dest::R(blend_src_temp, 0b0111));
            dxbc::Src blend_src_temp_src(dxbc::Src::R(blend_src_temp));

            // Extract the source color factor to temp.x.
            // temp.x = source color factor index.
            system_constants_used_ |= 1ull
                                      << kSysConst_EdramRTBlendFactorsOps_Index;
            a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                     dxbc::Src::LU((1 << 5) - 1));
            // Check if the source color factor is not zero - if it is, the
            // source must be ignored completely, and Infinity and NaN in it
            // shouldn't affect blending.
            a_.OpIf(true, temp_x_src);
            {
              // Open the switch for choosing the source color blend factor.
              // temp.x = free.
              a_.OpSwitch(temp_x_src);
              // Write the source color factor to blend_src_temp.xyz.
              // blend_src_temp.xyz = unclamped source color factor.
              ROV_HandleColorBlendFactorCases(system_temps_color_[i],
                                              color_temp, blend_src_temp);
              // Close the source color factor switch.
              a_.OpEndSwitch();
              // Get if the render target color is fixed-point and the source
              // color factor needs clamping to temp.x.
              // temp.x = whether color is fixed-point.
              system_constants_used_ |= 1ull
                                        << kSysConst_EdramRTFormatFlags_Index;
              a_.OpAnd(temp_x_dest, rt_format_flags_src,
                       dxbc::Src::LU(kRTFormatFlag_FixedPointColor));
              // Check if the source color factor needs clamping.
              a_.OpIf(true, temp_x_src);
              {
                // Clamp the source color factor in blend_src_temp.xyz.
                // blend_src_temp.xyz = source color factor.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(blend_src_temp_rgb_dest, blend_src_temp_src,
                         rt_clamp_vec_src.Select(0));
                a_.OpMin(blend_src_temp_rgb_dest, blend_src_temp_src,
                         rt_clamp_vec_src.Select(2));
              }
              // Close the source color factor clamping check.
              a_.OpEndIf();
              // Apply the factor to the source color.
              // blend_src_temp.xyz = unclamped source color part without
              //                      addition sign.
              a_.OpMul(blend_src_temp_rgb_dest,
                       dxbc::Src::R(system_temps_color_[i]),
                       blend_src_temp_src);
              // Check if the source color part needs clamping after the
              // multiplication.
              // temp.x = free.
              a_.OpIf(true, temp_x_src);
              {
                // Clamp the source color part.
                // blend_src_temp.xyz = source color part without addition sign.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(blend_src_temp_rgb_dest, blend_src_temp_src,
                         rt_clamp_vec_src.Select(0));
                a_.OpMin(blend_src_temp_rgb_dest, blend_src_temp_src,
                         rt_clamp_vec_src.Select(2));
              }
              // Close the source color part clamping check.
              a_.OpEndIf();
              // Extract the source color sign to temp.x.
              // temp.x = source color sign as zero for 1 and non-zero for -1.
              system_constants_used_ |=
                  1ull << kSysConst_EdramRTBlendFactorsOps_Index;
              a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                       dxbc::Src::LU(1 << (5 + 2)));
              // Apply the source color sign.
              // blend_src_temp.xyz = source color part.
              // temp.x = free.
              a_.OpMovC(blend_src_temp_rgb_dest, temp_x_src,
                        -blend_src_temp_src, blend_src_temp_src);
            }
            // The source color factor is zero.
            a_.OpElse();
            {
              // Write zero to the source color part.
              // blend_src_temp.xyz = source color part.
              // temp.x = free.
              a_.OpMov(blend_src_temp_rgb_dest, dxbc::Src::LF(0.0f));
            }
            // Close the source color factor zero check.
            a_.OpEndIf();

            // Extract the destination color factor to temp.x.
            // temp.x = destination color factor index.
            system_constants_used_ |= 1ull
                                      << kSysConst_EdramRTBlendFactorsOps_Index;
            a_.OpUBFE(temp_x_dest, dxbc::Src::LU(5), dxbc::Src::LU(8),
                      rt_blend_factors_ops_src);
            // Check if the destination color factor is not zero.
            a_.OpIf(true, temp_x_src);
            {
              uint32_t blend_dest_factor_temp = PushSystemTemp();
              dxbc::Src blend_dest_factor_temp_src(
                  dxbc::Src::R(blend_dest_factor_temp));
              // Open the switch for choosing the destination color blend
              // factor.
              // temp.x = free.
              a_.OpSwitch(temp_x_src);
              // Write the destination color factor to
              // blend_dest_factor_temp.xyz.
              // blend_dest_factor_temp.xyz = unclamped destination color
              //                              factor.
              ROV_HandleColorBlendFactorCases(
                  system_temps_color_[i], color_temp, blend_dest_factor_temp);
              // Close the destination color factor switch.
              a_.OpEndSwitch();
              // Get if the render target color is fixed-point and the
              // destination color factor needs clamping to temp.x.
              // temp.x = whether color is fixed-point.
              system_constants_used_ |= 1ull
                                        << kSysConst_EdramRTFormatFlags_Index;
              a_.OpAnd(temp_x_dest, rt_format_flags_src,
                       dxbc::Src::LU(kRTFormatFlag_FixedPointColor));
              // Check if the destination color factor needs clamping.
              a_.OpIf(true, temp_x_src);
              {
                // Clamp the destination color factor in
                // blend_dest_factor_temp.xyz.
                // blend_dest_factor_temp.xyz = destination color factor.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(dxbc::Dest::R(blend_dest_factor_temp, 0b0111),
                         blend_dest_factor_temp_src,
                         rt_clamp_vec_src.Select(0));
                a_.OpMin(dxbc::Dest::R(blend_dest_factor_temp, 0b0111),
                         blend_dest_factor_temp_src,
                         rt_clamp_vec_src.Select(2));
              }
              // Close the destination color factor clamping check.
              a_.OpEndIf();
              // Apply the factor to the destination color in color_temp.xyz.
              // color_temp.xyz = unclamped destination color part without
              //                  addition sign.
              // blend_dest_temp.xyz = free.
              a_.OpMul(color_temp_rgb_dest, color_temp_src,
                       blend_dest_factor_temp_src);
              // Release blend_dest_factor_temp.
              PopSystemTemp();
              // Check if the destination color part needs clamping after the
              // multiplication.
              // temp.x = free.
              a_.OpIf(true, temp_x_src);
              {
                // Clamp the destination color part.
                // color_temp.xyz = destination color part without addition
                // sign.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(color_temp_rgb_dest, color_temp_src,
                         rt_clamp_vec_src.Select(0));
                a_.OpMin(color_temp_rgb_dest, color_temp_src,
                         rt_clamp_vec_src.Select(2));
              }
              // Close the destination color part clamping check.
              a_.OpEndIf();
              // Extract the destination color sign to temp.x.
              // temp.x = destination color sign as zero for 1 and non-zero for
              //          -1.
              system_constants_used_ |=
                  1ull << kSysConst_EdramRTBlendFactorsOps_Index;
              a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                       dxbc::Src::LU(1 << 5));
              // Select the sign for destination multiply-add as 1.0 or -1.0 to
              // temp.x.
              // temp.x = destination color sign as float.
              a_.OpMovC(temp_x_dest, temp_x_src, dxbc::Src::LF(-1.0f),
                        dxbc::Src::LF(1.0f));
              // Perform color blending to color_temp.xyz.
              // color_temp.xyz = unclamped blended color.
              // blend_src_temp.xyz = free.
              // temp.x = free.
              a_.OpMAd(color_temp_rgb_dest, color_temp_src, temp_x_src,
                       blend_src_temp_src);
            }
            // The destination color factor is zero.
            a_.OpElse();
            {
              // Write the source color part without applying the destination
              // color.
              // color_temp.xyz = unclamped blended color.
              // blend_src_temp.xyz = free.
              // temp.x = free.
              a_.OpMov(color_temp_rgb_dest, blend_src_temp_src);
            }
            // Close the destination color factor zero check.
            a_.OpEndIf();

            // Release blend_src_temp.
            PopSystemTemp();

            // Clamp the color in color_temp.xyz before packing.
            // color_temp.xyz = blended color.
            system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
            a_.OpMax(color_temp_rgb_dest, color_temp_src,
                     rt_clamp_vec_src.Select(0));
            a_.OpMin(color_temp_rgb_dest, color_temp_src,
                     rt_clamp_vec_src.Select(2));
          }
          // Need to do min/max for color.
          a_.OpElse();
          {
            // Extract the color min (0) or max (1) bit to temp.x
            // temp.x = whether min or max should be used for color.
            system_constants_used_ |= 1ull
                                      << kSysConst_EdramRTBlendFactorsOps_Index;
            a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                     dxbc::Src::LU(1 << 5));
            // Check if need to do min or max for color.
            // temp.x = free.
            a_.OpIf(true, temp_x_src);
            {
              // Choose max of the colors without applying the factors to
              // color_temp.xyz.
              // color_temp.xyz = blended color.
              a_.OpMax(color_temp_rgb_dest,
                       dxbc::Src::R(system_temps_color_[i]), color_temp_src);
            }
            // Need to do min.
            a_.OpElse();
            {
              // Choose min of the colors without applying the factors to
              // color_temp.xyz.
              // color_temp.xyz = blended color.
              a_.OpMin(color_temp_rgb_dest,
                       dxbc::Src::R(system_temps_color_[i]), color_temp_src);
            }
            // Close the min or max check.
            a_.OpEndIf();
          }
          // Close the color factor blending or min/max check.
          a_.OpEndIf();

          // *******************************************************************
          // Alpha blending.
          // *******************************************************************

          // Extract the alpha min/max bit to temp.x.
          // temp.x = whether min/max should be used for alpha.
          system_constants_used_ |= 1ull
                                    << kSysConst_EdramRTBlendFactorsOps_Index;
          a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                   dxbc::Src::LU(1 << (21 + 1)));
          // Check if need to do blend the color with factors.
          // temp.x = free.
          a_.OpIf(false, temp_x_src);
          {
            // Extract the source alpha factor to temp.x.
            // temp.x = source alpha factor index.
            system_constants_used_ |= 1ull
                                      << kSysConst_EdramRTBlendFactorsOps_Index;
            a_.OpUBFE(temp_x_dest, dxbc::Src::LU(5), dxbc::Src::LU(16),
                      rt_blend_factors_ops_src);
            // Check if the source alpha factor is not zero.
            a_.OpIf(true, temp_x_src);
            {
              // Open the switch for choosing the source alpha blend factor.
              // temp.x = free.
              a_.OpSwitch(temp_x_src);
              // Write the source alpha factor to temp.x.
              // temp.x = unclamped source alpha factor.
              ROV_HandleAlphaBlendFactorCases(system_temps_color_[i],
                                              color_temp, temp, 0);
              // Close the source alpha factor switch.
              a_.OpEndSwitch();
              // Get if the render target alpha is fixed-point and the source
              // alpha factor needs clamping to temp.y.
              // temp.y = whether alpha is fixed-point.
              system_constants_used_ |= 1ull
                                        << kSysConst_EdramRTFormatFlags_Index;
              a_.OpAnd(temp_y_dest, rt_format_flags_src,
                       dxbc::Src::LU(kRTFormatFlag_FixedPointAlpha));
              // Check if the source alpha factor needs clamping.
              a_.OpIf(true, temp_y_src);
              {
                // Clamp the source alpha factor in temp.x.
                // temp.x = source alpha factor.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(3));
              }
              // Close the source alpha factor clamping check.
              a_.OpEndIf();
              // Apply the factor to the source alpha.
              // temp.x = unclamped source alpha part without addition sign.
              a_.OpMul(temp_x_dest,
                       dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                       temp_x_src);
              // Check if the source alpha part needs clamping after the
              // multiplication.
              // temp.y = free.
              a_.OpIf(true, temp_y_src);
              {
                // Clamp the source alpha part.
                // temp.x = source alpha part without addition sign.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(temp_x_dest, temp_x_src, rt_clamp_vec_src.Select(3));
              }
              // Close the source alpha part clamping check.
              a_.OpEndIf();
              // Extract the source alpha sign to temp.y.
              // temp.y = source alpha sign as zero for 1 and non-zero for -1.
              system_constants_used_ |=
                  1ull << kSysConst_EdramRTBlendFactorsOps_Index;
              a_.OpAnd(temp_y_dest, rt_blend_factors_ops_src,
                       dxbc::Src::LU(1 << (21 + 2)));
              // Apply the source alpha sign.
              // temp.x = source alpha part.
              a_.OpMovC(temp_x_dest, temp_y_src, -temp_x_src, temp_x_src);
            }
            // The source alpha factor is zero.
            a_.OpElse();
            {
              // Write zero to the source alpha part.
              // temp.x = source alpha part.
              a_.OpMov(temp_x_dest, dxbc::Src::LF(0.0f));
            }
            // Close the source alpha factor zero check.
            a_.OpEndIf();

            // Extract the destination alpha factor to temp.y.
            // temp.y = destination alpha factor index.
            system_constants_used_ |= 1ull
                                      << kSysConst_EdramRTBlendFactorsOps_Index;
            a_.OpUBFE(temp_y_dest, dxbc::Src::LU(5), dxbc::Src::LU(24),
                      rt_blend_factors_ops_src);
            // Check if the destination alpha factor is not zero.
            a_.OpIf(true, temp_y_src);
            {
              // Open the switch for choosing the destination alpha blend
              // factor.
              // temp.y = free.
              a_.OpSwitch(temp_y_src);
              // Write the destination alpha factor to temp.y.
              // temp.y = unclamped destination alpha factor.
              ROV_HandleAlphaBlendFactorCases(system_temps_color_[i],
                                              color_temp, temp, 1);
              // Close the destination alpha factor switch.
              a_.OpEndSwitch();
              // Get if the render target alpha is fixed-point and the
              // destination alpha factor needs clamping.
              // alpha_is_fixed_temp.x = whether alpha is fixed-point.
              uint32_t alpha_is_fixed_temp = PushSystemTemp();
              system_constants_used_ |= 1ull
                                        << kSysConst_EdramRTFormatFlags_Index;
              a_.OpAnd(dxbc::Dest::R(alpha_is_fixed_temp, 0b0001),
                       rt_format_flags_src,
                       dxbc::Src::LU(kRTFormatFlag_FixedPointAlpha));
              // Check if the destination alpha factor needs clamping.
              a_.OpIf(true,
                      dxbc::Src::R(alpha_is_fixed_temp, dxbc::Src::kXXXX));
              {
                // Clamp the destination alpha factor in temp.y.
                // temp.y = destination alpha factor.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(temp_y_dest, temp_y_src, rt_clamp_vec_src.Select(1));
                a_.OpMin(temp_y_dest, temp_y_src, rt_clamp_vec_src.Select(3));
              }
              // Close the destination alpha factor clamping check.
              a_.OpEndIf();
              // Apply the factor to the destination alpha in color_temp.w.
              // color_temp.w = unclamped destination alpha part without
              //                addition sign.
              a_.OpMul(color_temp_a_dest, color_temp_a_src, temp_y_src);
              // Check if the destination alpha part needs clamping after the
              // multiplication.
              // alpha_is_fixed_temp.x = free.
              a_.OpIf(true,
                      dxbc::Src::R(alpha_is_fixed_temp, dxbc::Src::kXXXX));
              // Release alpha_is_fixed_temp.
              PopSystemTemp();
              {
                // Clamp the destination alpha part.
                // color_temp.w = destination alpha part without addition sign.
                system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
                a_.OpMax(color_temp_a_dest, color_temp_a_src,
                         rt_clamp_vec_src.Select(1));
                a_.OpMin(color_temp_a_dest, color_temp_a_src,
                         rt_clamp_vec_src.Select(3));
              }
              // Close the destination alpha factor clamping check.
              a_.OpEndIf();
              // Extract the destination alpha sign to temp.y.
              // temp.y = destination alpha sign as zero for 1 and non-zero for
              //          -1.
              system_constants_used_ |=
                  1ull << kSysConst_EdramRTBlendFactorsOps_Index;
              a_.OpAnd(temp_y_dest, rt_blend_factors_ops_src,
                       dxbc::Src::LU(1 << 21));
              // Select the sign for destination multiply-add as 1.0 or -1.0 to
              // temp.y.
              // temp.y = destination alpha sign as float.
              a_.OpMovC(temp_y_dest, temp_y_src, dxbc::Src::LF(-1.0f),
                        dxbc::Src::LF(1.0f));
              // Perform alpha blending to color_temp.w.
              // color_temp.w = unclamped blended alpha.
              // temp.xy = free.
              a_.OpMAd(color_temp_a_dest, color_temp_a_src, temp_y_src,
                       temp_x_src);
            }
            // The destination alpha factor is zero.
            a_.OpElse();
            {
              // Write the source alpha part without applying the destination
              // alpha.
              // color_temp.w = unclamped blended alpha.
              // temp.xy = free.
              a_.OpMov(color_temp_a_dest, temp_x_src);
            }
            // Close the destination alpha factor zero check.
            a_.OpEndIf();

            // Clamp the alpha in color_temp.w before packing.
            // color_temp.w = blended alpha.
            system_constants_used_ |= 1ull << kSysConst_EdramRTClamp_Index;
            a_.OpMax(color_temp_a_dest, color_temp_a_src,
                     rt_clamp_vec_src.Select(1));
            a_.OpMin(color_temp_a_dest, color_temp_a_src,
                     rt_clamp_vec_src.Select(3));
          }
          // Need to do min/max for alpha.
          a_.OpElse();
          {
            // Extract the alpha min (0) or max (1) bit to temp.x.
            // temp.x = whether min or max should be used for alpha.
            system_constants_used_ |= 1ull
                                      << kSysConst_EdramRTBlendFactorsOps_Index;
            a_.OpAnd(temp_x_dest, rt_blend_factors_ops_src,
                     dxbc::Src::LU(1 << 21));
            // Check if need to do min or max for alpha.
            // temp.x = free.
            a_.OpIf(true, temp_x_src);
            {
              // Choose max of the alphas without applying the factors to
              // color_temp.w.
              // color_temp.w = blended alpha.
              a_.OpMax(color_temp_a_dest,
                       dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                       color_temp_a_src);
            }
            // Need to do min.
            a_.OpElse();
            {
              // Choose min of the alphas without applying the factors to
              // color_temp.w.
              // color_temp.w = blended alpha.
              a_.OpMin(color_temp_a_dest,
                       dxbc::Src::R(system_temps_color_[i], dxbc::Src::kWWWW),
                       color_temp_a_src);
            }
            // Close the min or max check.
            a_.OpEndIf();
          }
          // Close the alpha factor blending or min/max check.
          a_.OpEndIf();

          // Pack the new color/alpha to temp.xy.
          // temp.xy = packed new color/alpha.
          uint32_t color_pack_temp = PushSystemTemp();
          ROV_PackPreClampedColor(i, color_temp, temp, 0, color_pack_temp, 0,
                                  color_pack_temp, 1);
          // Release color_pack_temp.
          PopSystemTemp();
        }
        // Close the blending check.
        a_.OpEndIf();

        // *********************************************************************
        // Write mask application
        // *********************************************************************

        // Apply the keep mask to the previous packed color/alpha in temp.zw.
        // temp.zw = masked packed old color/alpha.
        system_constants_used_ |= 1ull << kSysConst_EdramRTKeepMask_Index;
        a_.OpAnd(dxbc::Dest::R(temp, 0b1100), dxbc::Src::R(temp),
                 keep_mask_vec_src.Swizzle(keep_mask_swizzle << 4));
        // Invert the keep mask into color_temp.xy.
        // color_temp.xy = inverted keep mask (write mask).
        system_constants_used_ |= 1ull << kSysConst_EdramRTKeepMask_Index;
        a_.OpNot(dxbc::Dest::R(color_temp, 0b0011),
                 keep_mask_vec_src.Swizzle(keep_mask_swizzle));
        // Release color_temp.
        PopSystemTemp();
        // Apply the write mask to the new color/alpha in temp.xy.
        // temp.xy = masked packed new color/alpha.
        a_.OpAnd(dxbc::Dest::R(temp, 0b0011), dxbc::Src::R(temp),
                 dxbc::Src::R(color_temp));
        // Combine the masked colors into temp.xy.
        // temp.xy = packed resulting color/alpha.
        // temp.zw = free.
        a_.OpOr(dxbc::Dest::R(temp, 0b0011), dxbc::Src::R(temp),
                dxbc::Src::R(temp, 0b1110));
      }
      // Close the previous color load check.
      a_.OpEndIf();

      // ***********************************************************************
      // Writing the color
      // ***********************************************************************

      // Get if the format is 64bpp to temp.z.
      // temp.z = whether the render target is 64bpp.
      system_constants_used_ |= 1ull << kSysConst_EdramRTFormatFlags_Index;
      a_.OpAnd(temp_z_dest, rt_format_flags_src,
               dxbc::Src::LU(kRTFormatFlag_64bpp));
      // Check if the format is 64bpp.
      // temp.z = free.
      a_.OpIf(true, temp_z_src);
      {
        // Store the lower 32 bits of the 64bpp color.
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(
            dxbc::Dest::U(uav_index_edram_, uint32_t(UAVRegister::kEdram)),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW), 1,
            temp_x_src);
        // Get the address of the upper 32 bits of the color to temp.z (can't
        // use temp.x because components when not blending, packing is done once
        // for all samples, so it has to be preserved).
        a_.OpIAdd(temp_z_dest,
                  dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kWWWW),
                  dxbc::Src::LU(1));
        // Store the upper 32 bits of the 64bpp color.
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(
            dxbc::Dest::U(uav_index_edram_, uint32_t(UAVRegister::kEdram)),
            temp_z_src, 1, temp_y_src);
      }
      // The color is 32bpp.
      a_.OpElse();
      {
        // Store the 32bpp color.
        if (uav_index_edram_ == kBindingIndexUnallocated) {
          uav_index_edram_ = uav_count_++;
        }
        a_.OpStoreUAVTyped(
            dxbc::Dest::U(uav_index_edram_, uint32_t(UAVRegister::kEdram)),
            dxbc::Src::R(system_temp_rov_params_, dxbc::Src::kZZZZ), 1,
            temp_x_src);
      }
      // Close the 64bpp/32bpp conditional.
      a_.OpEndIf();

      // ***********************************************************************
      // End of color sample raster operation.
      // ***********************************************************************

      // Close the sample covered check.
      a_.OpEndIf();

      // Go to the next sample (samples are at +0, +80, +1, +81, so need to do
      // +80, -79, +80 and -81 after each sample).
      system_constants_used_ |= 1ull
                                << kSysConst_EdramResolutionSquareScale_Index;
      a_.OpIMAd(dxbc::Dest::R(system_temp_rov_params_, 0b1100),
                dxbc::Src::LI(0, 0, (j & 1) ? -78 - j : 80,
                              ((j & 1) ? -78 - j : 80) * 2),
                dxbc::Src::CB(cbuffer_index_system_constants_,
                              uint32_t(CbufferRegister::kSystemConstants),
                              kSysConst_EdramResolutionSquareScale_Vec)
                    .Select(kSysConst_EdramResolutionSquareScale_Comp),
                dxbc::Src::R(system_temp_rov_params_));
    }

    // Revert adding the EDRAM bases of the render target to
    // system_temp_rov_params_.zw.
    system_constants_used_ |= 1ull << kSysConst_EdramRTBaseDwordsScaled_Index;
    a_.OpIAdd(dxbc::Dest::R(system_temp_rov_params_, 0b1100),
              dxbc::Src::R(system_temp_rov_params_),
              -dxbc::Src::CB(cbuffer_index_system_constants_,
                             uint32_t(CbufferRegister::kSystemConstants),
                             kSysConst_EdramRTBaseDwordsScaled_Vec)
                   .Select(i));
    // Close the render target write check.
    a_.OpEndIf();
  }

  // Release temp.
  PopSystemTemp();
}

void DxbcShaderTranslator::CompletePixelShader() {
  if (is_depth_only_pixel_shader_) {
    // The depth-only shader only needs to do the depth test and to write the
    // depth to the ROV.
    if (edram_rov_used_) {
      CompletePixelShader_WriteToROV();
    }
    return;
  }

  if (current_shader().writes_color_target(0)) {
    // Alpha test.
    // X - mask, then masked result (SGPR for loading, VGPR for masking).
    // Y - operation result (SGPR for mask operations, VGPR for alpha
    //     operations).
    uint32_t alpha_test_temp = PushSystemTemp();
    dxbc::Dest alpha_test_mask_dest(dxbc::Dest::R(alpha_test_temp, 0b0001));
    dxbc::Src alpha_test_mask_src(
        dxbc::Src::R(alpha_test_temp, dxbc::Src::kXXXX));
    dxbc::Dest alpha_test_op_dest(dxbc::Dest::R(alpha_test_temp, 0b0010));
    dxbc::Src alpha_test_op_src(
        dxbc::Src::R(alpha_test_temp, dxbc::Src::kYYYY));
    // Extract the comparison mask to check if the test needs to be done at all.
    // Don't care about flow control being somewhat dynamic - early Z is forced
    // using a special version of the shader anyway.
    system_constants_used_ |= 1ull << kSysConst_Flags_Index;
    a_.OpUBFE(alpha_test_mask_dest, dxbc::Src::LU(3),
              dxbc::Src::LU(kSysFlag_AlphaPassIfLess_Shift),
              dxbc::Src::CB(cbuffer_index_system_constants_,
                            uint32_t(CbufferRegister::kSystemConstants),
                            kSysConst_Flags_Vec)
                  .Select(kSysConst_Flags_Comp));
    // Compare the mask to ALWAYS to check if the test shouldn't be done (will
    // pass even for NaNs, though the expected behavior in this case hasn't been
    // checked, but let's assume this means "always", not "less, equal or
    // greater".
    // TODO(Triang3l): Check how alpha test works with NaN on Direct3D 9.
    a_.OpINE(alpha_test_op_dest, alpha_test_mask_src, dxbc::Src::LU(0b111));
    // Don't do the test if the mode is "always".
    a_.OpIf(true, alpha_test_op_src);
    {
      // Do the test. Can't use subtraction and sign because of float specials.
      dxbc::Src alpha_src(
          dxbc::Src::R(system_temps_color_[0], dxbc::Src::kWWWW));
      system_constants_used_ |= 1ull << kSysConst_AlphaTestReference_Index;
      dxbc::Src alpha_test_reference_src(
          dxbc::Src::CB(cbuffer_index_system_constants_,
                        uint32_t(CbufferRegister::kSystemConstants),
                        kSysConst_AlphaTestReference_Vec)
              .Select(kSysConst_AlphaTestReference_Comp));
      // Less than.
      a_.OpLT(alpha_test_op_dest, alpha_src, alpha_test_reference_src);
      a_.OpOr(alpha_test_op_dest, alpha_test_op_src,
              dxbc::Src::LU(~uint32_t(1 << 0)));
      a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
      // Equals to.
      a_.OpEq(alpha_test_op_dest, alpha_src, alpha_test_reference_src);
      a_.OpOr(alpha_test_op_dest, alpha_test_op_src,
              dxbc::Src::LU(~uint32_t(1 << 1)));
      a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
      // Greater than.
      a_.OpLT(alpha_test_op_dest, alpha_test_reference_src, alpha_src);
      a_.OpOr(alpha_test_op_dest, alpha_test_op_src,
              dxbc::Src::LU(~uint32_t(1 << 2)));
      a_.OpAnd(alpha_test_mask_dest, alpha_test_mask_src, alpha_test_op_src);
      // Discard the pixel if it has failed the test.
      if (edram_rov_used_) {
        a_.OpRetC(false, alpha_test_mask_src);
      } else {
        a_.OpDiscard(false, alpha_test_mask_src);
      }
    }
    // Close the "not always" check.
    a_.OpEndIf();
    // Release alpha_test_temp.
    PopSystemTemp();
  }

  // Write the values to the render targets. Not applying the exponent bias yet
  // because the original 0 to 1 alpha value is needed for alpha to coverage,
  // which is done differently for ROV and RTV/DSV.
  if (edram_rov_used_) {
    CompletePixelShader_WriteToROV();
  } else {
    CompletePixelShader_WriteToRTVs();
    CompletePixelShader_DSV_DepthTo24Bit();
  }
}

void DxbcShaderTranslator::PreClampedDepthTo20e4(uint32_t d24_temp,
                                                 uint32_t d24_temp_component,
                                                 uint32_t d32_temp,
                                                 uint32_t d32_temp_component,
                                                 uint32_t temp_temp,
                                                 uint32_t temp_temp_component) {
  assert_true(temp_temp != d24_temp ||
              temp_temp_component != d24_temp_component);
  assert_true(temp_temp != d32_temp ||
              temp_temp_component != d32_temp_component);
  // Source and destination may be the same.
  dxbc::Dest d24_dest(dxbc::Dest::R(d24_temp, 1 << d24_temp_component));
  dxbc::Src d24_src(dxbc::Src::R(d24_temp).Select(d24_temp_component));
  dxbc::Src d32_src(dxbc::Src::R(d32_temp).Select(d32_temp_component));
  dxbc::Dest temp_dest(dxbc::Dest::R(temp_temp, 1 << temp_temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp_temp).Select(temp_temp_component));

  // CFloat24 from d3dref9.dll.
  // Assuming the depth is already clamped to [0, 2) (in all places, the depth
  // is written with the saturate flag set).

  // Check if the number is too small to be represented as normalized 20e4.
  // temp = f32 < 2^-14
  a_.OpULT(temp_dest, d32_src, dxbc::Src::LU(0x38800000));
  // Handle denormalized numbers separately.
  a_.OpIf(true, temp_src);
  {
    // temp = f32 >> 23
    a_.OpUShR(temp_dest, d32_src, dxbc::Src::LU(23));
    // temp = 113 - (f32 >> 23)
    a_.OpIAdd(temp_dest, dxbc::Src::LI(113), -temp_src);
    // Don't allow the shift to overflow, since in DXBC the lower 5 bits of the
    // shift amount are used (otherwise 0 becomes 8).
    // temp = min(113 - (f32 >> 23), 24)
    a_.OpUMin(temp_dest, temp_src, dxbc::Src::LU(24));
    // biased_f32 = (f32 & 0x7FFFFF) | 0x800000
    a_.OpBFI(d24_dest, dxbc::Src::LU(9), dxbc::Src::LU(23), dxbc::Src::LU(1),
             d32_src);
    // biased_f32 = ((f32 & 0x7FFFFF) | 0x800000) >> min(113 - (f32 >> 23), 24)
    a_.OpUShR(d24_dest, d24_src, temp_src);
  }
  // Not denormalized?
  a_.OpElse();
  {
    // Bias the exponent.
    // biased_f32 = f32 + (-112 << 23)
    // (left shift of a negative value is undefined behavior)
    a_.OpIAdd(d24_dest, d32_src, dxbc::Src::LU(0xC8000000u));
  }
  // Close the denormal check.
  a_.OpEndIf();
  // Build the 20e4 number.
  // temp = (biased_f32 >> 3) & 1
  a_.OpUBFE(temp_dest, dxbc::Src::LU(1), dxbc::Src::LU(3), d24_src);
  // f24 = biased_f32 + 3
  a_.OpIAdd(d24_dest, d24_src, dxbc::Src::LU(3));
  // f24 = biased_f32 + 3 + ((biased_f32 >> 3) & 1)
  a_.OpIAdd(d24_dest, d24_src, temp_src);
  // f24 = ((biased_f32 + 3 + ((biased_f32 >> 3) & 1)) >> 3) & 0xFFFFFF
  a_.OpUBFE(d24_dest, dxbc::Src::LU(24), dxbc::Src::LU(3), d24_src);
}

void DxbcShaderTranslator::ROV_DepthTo24Bit(uint32_t d24_temp,
                                            uint32_t d24_temp_component,
                                            uint32_t d32_temp,
                                            uint32_t d32_temp_component,
                                            uint32_t temp_temp,
                                            uint32_t temp_temp_component) {
  assert_true(temp_temp != d32_temp ||
              temp_temp_component != d32_temp_component);
  // Source and destination may be the same.

  system_constants_used_ |= 1ull << kSysConst_Flags_Index;
  a_.OpAnd(dxbc::Dest::R(temp_temp, 1 << temp_temp_component),
           dxbc::Src::CB(cbuffer_index_system_constants_,
                         uint32_t(CbufferRegister::kSystemConstants),
                         kSysConst_Flags_Vec)
               .Select(kSysConst_Flags_Comp),
           dxbc::Src::LU(kSysFlag_ROVDepthFloat24));
  // Convert according to the format.
  a_.OpIf(true, dxbc::Src::R(temp_temp).Select(temp_temp_component));
  {
    // 20e4 conversion.
    PreClampedDepthTo20e4(d24_temp, d24_temp_component, d32_temp,
                          d32_temp_component, temp_temp, temp_temp_component);
  }
  a_.OpElse();
  {
    // Unorm24 conversion.
    dxbc::Dest d24_dest(dxbc::Dest::R(d24_temp, 1 << d24_temp_component));
    dxbc::Src d24_src(dxbc::Src::R(d24_temp).Select(d24_temp_component));
    // Multiply by float(0xFFFFFF).
    a_.OpMul(d24_dest, dxbc::Src::R(d32_temp).Select(d32_temp_component),
             dxbc::Src::LF(16777215.0f));
    // Round to the nearest even integer. This seems to be the correct way:
    // rounding towards zero gives 0xFF instead of 0x100 in clear shaders in,
    // for instance, Halo 3, but other clear shaders in it are also broken if
    // 0.5 is added before ftou instead of round_ne.
    a_.OpRoundNE(d24_dest, d24_src);
    // Convert to fixed-point.
    a_.OpFToU(d24_dest, d24_src);
  }
  a_.OpEndIf();
}

}  // namespace gpu
}  // namespace xe
