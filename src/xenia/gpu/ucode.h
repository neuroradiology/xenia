/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_UCODE_H_
#define XENIA_GPU_UCODE_H_

#include <cstdint>

#include "xenia/base/assert.h"
#include "xenia/base/platform.h"
#include "xenia/gpu/xenos.h"

// Closest AMD doc:
// https://developer.amd.com/wordpress/media/2012/10/R600_Instruction_Set_Architecture.pdf
// Microcode format differs, but most fields/enums are the same.

// This code comes from the freedreno project:
// https://github.com/freedreno/freedreno/blob/master/includes/instr-a2xx.h
/*
 * Copyright (c) 2012 Rob Clark <robdclark@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

namespace xe {
namespace gpu {
namespace ucode {

// Defines control flow opcodes used to schedule instructions.
enum class ControlFlowOpcode : uint32_t {
  // No-op - used to fill space.
  kNop = 0,
  // Executes fetch or ALU instructions.
  kExec = 1,
  // Executes fetch or ALU instructions then ends execution.
  kExecEnd = 2,
  // Conditionally executes based on a bool const.
  kCondExec = 3,
  // Conditionally executes based on a bool const then ends execution.
  kCondExecEnd = 4,
  // Conditionally executes based on the current predicate.
  kCondExecPred = 5,
  // Conditionally executes based on the current predicate then ends execution.
  kCondExecPredEnd = 6,
  // Starts a loop that must be terminated with kLoopEnd.
  kLoopStart = 7,
  // Continues or breaks out of a loop started with kLoopStart.
  kLoopEnd = 8,
  // Conditionally calls a function.
  // A return address is pushed to the stack to be used by a kReturn.
  kCondCall = 9,
  // Returns from the current function as called by kCondCall.
  // This is a no-op if not in a function.
  kReturn = 10,
  // Conditionally jumps to an arbitrary address based on a bool const.
  kCondJmp = 11,
  // Allocates output values.
  kAlloc = 12,
  // Conditionally executes based on the current predicate.
  // Optionally resets the predicate value.
  kCondExecPredClean = 13,
  // Conditionally executes based on the current predicate then ends execution.
  // Optionally resets the predicate value.
  kCondExecPredCleanEnd = 14,
  // Hints that no more vertex fetches will be performed.
  kMarkVsFetchDone = 15,
};

// Returns true if the given control flow opcode executes ALU or fetch
// instructions.
constexpr bool IsControlFlowOpcodeExec(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kExec ||
         opcode == ControlFlowOpcode::kExecEnd ||
         opcode == ControlFlowOpcode::kCondExec ||
         opcode == ControlFlowOpcode::kCondExecEnd ||
         opcode == ControlFlowOpcode::kCondExecPred ||
         opcode == ControlFlowOpcode::kCondExecPredEnd ||
         opcode == ControlFlowOpcode::kCondExecPredClean ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

// Returns true if the given control flow opcode terminates the shader after
// executing.
constexpr bool DoesControlFlowOpcodeEndShader(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kExecEnd ||
         opcode == ControlFlowOpcode::kCondExecEnd ||
         opcode == ControlFlowOpcode::kCondExecPredEnd ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

// Returns true if the given control flow opcode resets the predicate prior to
// execution.
constexpr bool DoesControlFlowOpcodeCleanPredicate(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kCondExecPredClean ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

// Determines whether addressing is based on a0 or aL.
enum class AddressingMode : uint32_t {
  // Indexing into register sets is done based on aL.
  // This allows forms like c[aL + 5].
  kRelative = 0,
  // Indexing into register sets is done based on a0.
  // This allows forms like c[a0 + 5].
  kAbsolute = 1,
};

// Defines the type of a ControlFlowOpcode::kAlloc instruction.
// The allocation is just a size reservation and there may be multiple in a
// shader.
enum class AllocType : uint32_t {
  // ?
  kNone = 0,
  // Vertex shader exports a position.
  kVsPosition = 1,
  // Vertex shader exports interpolators.
  kVsInterpolators = 2,
  // Pixel shader exports colors.
  kPsColors = 2,
  // MEMEXPORT?
  kMemory = 3,
};

// Instruction data for ControlFlowOpcode::kExec and kExecEnd.
struct ControlFlowExecInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Address of the instructions to execute.
  uint32_t address() const { return address_; }
  // Number of instructions being executed.
  uint32_t count() const { return count_; }
  // Sequence bits, 2 per instruction, indicating whether ALU or fetch.
  uint32_t sequence() const { return serialize_; }
  // Whether to reset the current predicate.
  bool clean() const { return clean_ == 1; }
  // ?
  bool is_yield() const { return is_yeild_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 12;
  uint32_t count_ : 3;
  uint32_t is_yeild_ : 1;
  uint32_t serialize_ : 12;
  uint32_t vc_hi_ : 4;  // Vertex cache?

  // Word 1: (16 bits)
  uint32_t vc_lo_ : 2;
  uint32_t : 7;
  uint32_t clean_ : 1;
  uint32_t : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowExecInstruction, 8);

// Instruction data for ControlFlowOpcode::kCondExec and kCondExecEnd.
struct ControlFlowCondExecInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Address of the instructions to execute.
  uint32_t address() const { return address_; }
  // Number of instructions being executed.
  uint32_t count() const { return count_; }
  // Sequence bits, 2 per instruction, indicating whether ALU or fetch.
  uint32_t sequence() const { return serialize_; }
  // Constant index used as the conditional.
  uint32_t bool_address() const { return bool_address_; }
  // Required condition value of the comparision (true or false).
  bool condition() const { return condition_ == 1; }
  // ?
  bool is_yield() const { return is_yeild_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 12;
  uint32_t count_ : 3;
  uint32_t is_yeild_ : 1;
  uint32_t serialize_ : 12;
  uint32_t vc_hi_ : 4;  // Vertex cache?

  // Word 1: (16 bits)
  uint32_t vc_lo_ : 2;
  uint32_t bool_address_ : 8;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowCondExecInstruction, 8);

// Instruction data for ControlFlowOpcode::kCondExecPred, kCondExecPredEnd,
// kCondExecPredClean, kCondExecPredCleanEnd.
struct ControlFlowCondExecPredInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Address of the instructions to execute.
  uint32_t address() const { return address_; }
  // Number of instructions being executed.
  uint32_t count() const { return count_; }
  // Sequence bits, 2 per instruction, indicating whether ALU or fetch.
  uint32_t sequence() const { return serialize_; }
  // Whether to reset the current predicate.
  bool clean() const { return clean_ == 1; }
  // Required condition value of the comparision (true or false).
  bool condition() const { return condition_ == 1; }
  // ?
  bool is_yield() const { return is_yeild_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 12;
  uint32_t count_ : 3;
  uint32_t is_yeild_ : 1;
  uint32_t serialize_ : 12;
  uint32_t vc_hi_ : 4;  // Vertex cache?

  // Word 1: (16 bits)
  uint32_t vc_lo_ : 2;
  uint32_t : 7;
  uint32_t clean_ : 1;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowCondExecPredInstruction, 8);

// Instruction data for ControlFlowOpcode::kLoopStart.
struct ControlFlowLoopStartInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Target address to jump to when skipping the loop.
  uint32_t address() const { return address_; }
  // Whether to reuse the current aL instead of reset it to loop start.
  bool is_repeat() const { return is_repeat_; }
  // Integer constant register that holds the loop parameters.
  // 0:7 - uint8 loop count, 8:15 - uint8 start aL, 16:23 - int8 aL step.
  uint32_t loop_id() const { return loop_id_; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 13;
  uint32_t is_repeat_ : 1;
  uint32_t : 2;
  uint32_t loop_id_ : 5;
  uint32_t : 11;

  // Word 1: (16 bits)
  uint32_t : 11;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowLoopStartInstruction, 8);

// Instruction data for ControlFlowOpcode::kLoopEnd.
struct ControlFlowLoopEndInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Target address of the start of the loop body.
  uint32_t address() const { return address_; }
  // Integer constant register that holds the loop parameters.
  // 0:7 - uint8 loop count, 8:15 - uint8 start aL, 16:23 - int8 aL step.
  uint32_t loop_id() const { return loop_id_; }
  // Break from the loop if the predicate matches the expected value.
  bool is_predicated_break() const { return is_predicated_break_; }
  // Required condition value of the comparision (true or false).
  bool condition() const { return condition_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 13;
  uint32_t : 3;
  uint32_t loop_id_ : 5;
  uint32_t is_predicated_break_ : 1;
  uint32_t : 10;

  // Word 1: (16 bits)
  uint32_t : 10;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowLoopEndInstruction, 8);

// Instruction data for ControlFlowOpcode::kCondCall.
struct ControlFlowCondCallInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Target address.
  uint32_t address() const { return address_; }
  // Unconditional call - ignores condition/predication.
  bool is_unconditional() const { return is_unconditional_; }
  // Whether the call is predicated (or conditional).
  bool is_predicated() const { return is_predicated_; }
  // Constant index used as the conditional.
  uint32_t bool_address() const { return bool_address_; }
  // Required condition value of the comparision (true or false).
  bool condition() const { return condition_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 13;
  uint32_t is_unconditional_ : 1;
  uint32_t is_predicated_ : 1;
  uint32_t : 17;

  // Word 1: (16 bits)
  uint32_t : 2;
  uint32_t bool_address_ : 8;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowCondCallInstruction, 8);

// Instruction data for ControlFlowOpcode::kReturn.
struct ControlFlowReturnInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }

 private:
  // Word 0: (32 bits)
  uint32_t : 32;

  // Word 1: (16 bits)
  uint32_t : 11;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowReturnInstruction, 8);

// Instruction data for ControlFlowOpcode::kCondJmp.
struct ControlFlowCondJmpInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  // Target address.
  uint32_t address() const { return address_; }
  // Unconditional jump - ignores condition/predication.
  bool is_unconditional() const { return is_unconditional_; }
  // Whether the jump is predicated (or conditional).
  bool is_predicated() const { return is_predicated_; }
  // Constant index used as the conditional.
  uint32_t bool_address() const { return bool_address_; }
  // Required condition value of the comparision (true or false).
  bool condition() const { return condition_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 13;
  uint32_t is_unconditional_ : 1;
  uint32_t is_predicated_ : 1;
  uint32_t : 17;

  // Word 1: (16 bits)
  uint32_t : 1;
  uint32_t direction_ : 1;
  uint32_t bool_address_ : 8;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowCondJmpInstruction, 8);

// Instruction data for ControlFlowOpcode::kAlloc.
struct ControlFlowAllocInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  // The total number of the given type allocated by this instruction.
  uint32_t size() const { return size_; }
  // Unconditional jump - ignores condition/predication.
  AllocType alloc_type() const { return alloc_type_; }

 private:
  // Word 0: (32 bits)
  uint32_t size_ : 3;
  uint32_t : 29;

  // Word 1: (16 bits)
  uint32_t : 8;
  uint32_t is_unserialized_ : 1;
  AllocType alloc_type_ : 2;
  uint32_t : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert_size(ControlFlowAllocInstruction, 8);

XEPACKEDUNION(ControlFlowInstruction, {
  ControlFlowOpcode opcode() const { return opcode_value; }

  ControlFlowExecInstruction exec;                    // kExec*
  ControlFlowCondExecInstruction cond_exec;           // kCondExec*
  ControlFlowCondExecPredInstruction cond_exec_pred;  // kCondExecPred*
  ControlFlowLoopStartInstruction loop_start;         // kLoopStart
  ControlFlowLoopEndInstruction loop_end;             // kLoopEnd
  ControlFlowCondCallInstruction cond_call;           // kCondCall
  ControlFlowReturnInstruction ret;                   // kReturn
  ControlFlowCondJmpInstruction cond_jmp;             // kCondJmp
  ControlFlowAllocInstruction alloc;                  // kAlloc

  XEPACKEDSTRUCTANONYMOUS({
    uint32_t unused_0 : 32;
    uint32_t unused_1 : 12;
    ControlFlowOpcode opcode_value : 4;
  });
  XEPACKEDSTRUCTANONYMOUS({
    uint32_t dword_0;
    uint32_t dword_1;
  });
});
static_assert_size(ControlFlowInstruction, 8);

inline void UnpackControlFlowInstructions(const uint32_t* dwords,
                                          ControlFlowInstruction* out_ab) {
  uint32_t dword_0 = dwords[0];
  uint32_t dword_1 = dwords[1];
  uint32_t dword_2 = dwords[2];
  out_ab[0].dword_0 = dword_0;
  out_ab[0].dword_1 = dword_1 & 0xFFFF;
  out_ab[1].dword_0 = (dword_1 >> 16) | (dword_2 << 16);
  out_ab[1].dword_1 = dword_2 >> 16;
}

enum class FetchOpcode : uint32_t {
  kVertexFetch = 0,

  // http://web.archive.org/web/20090514012026/http://msdn.microsoft.com/en-us/library/bb313957.aspx
  //
  // Parameters:
  // - UnnormalizedTextureCoords = false (default) / true.
  //   Only taken into account if AddressU (1D) / AddressU/V (2D) / AddressU/V/W
  //   (3D/cube) are all set to a non-wrapping mode. To access 1D textures that
  //   are wider than 8192 texels, unnormalized texture coordinates must be
  //   used.
  // - MagFilter = point / linear / keep (default).
  // - MinFilter = point / linear / keep (default).
  // - MipFilter = point / linear / basemap (undocumented, but assembled) / keep
  //   (default).
  // - VolMagFilter (3D only) - "filter used when the volume is magnified"
  //   ("volume" as opposed to "texture") - point / linear / keep (default).
  // - VolMinFilter (3D only) - point / linear / keep (default).
  // - AnisoFilter = disabled / max1to1 / max2to1 / max4to1 / max8to1 /
  //   max16to1 / keep (default).
  // - UseComputedLOD = false / true (default).
  // - UseRegisterLOD = false (default) / true.
  // - LODBias = -4.0...3.9375, in 1/16 increments. the default is 0.
  // - OffsetX - value added to the x-component of the texel address right
  //   before sampling = -8.0...7.5, in 1/2 increments, the default is 0.
  // - OffsetY (2D, 3D and cube only) - similar to OffsetX.
  // - OffsetZ (3D and cube only) - similar to OffsetX.
  // - FetchValidOnly - performance booster, whether the data should be fetched
  //   only for pixels inside the current primitive in a 2x2 quad (must be set
  //   to false if the result itself is used to calculate gradients) = false /
  //   true (default).
  //
  // Coordinates:
  // - 1D: U (normalized or unnormalized)
  // - 2D: U, V (normalized or unnormalized)
  // - 3D (used for both 3D and stacked 2D texture): U, V, W (normalized or
  //   unnormalized - same for both 3D W and stack layer; also VolMagFilter /
  //   VolMinFilter between stack layers is supported, used for color correction
  //   in Burnout Revenge).
  // - Cube: SC, TC (between 1 and 2 for normalized), face ID (0.0 to 5.0), the
  //   cube vector ALU instruction is used to calculate them.
  // https://gpuopen.com/learn/fetching-from-cubes-and-octahedrons/
  // "The 1.5 constant is designed such that the output face coordinate (v4 and
  //  v5 in the above example) range is {1.0 <= x < 2.0} which has an advantage
  //  in bit encoding compared to {0.0 <= x < 1.0} in that the upper mantissa
  //  bits are constant throughout the entire output range."
  //
  // The total LOD for a sample is additive and is based on what is enabled.
  //
  // For cube maps, according to what texCUBEgrad compiles to in a modified
  // HLSL shader of Brave: A Warrior's Tale and to XNA assembler output for PC
  // SM3 texldd, register gradients are in cube space (not in SC/TC space,
  // unlike the coordinates themselves). This isn't true for the GCN, however.
  //
  // TODO(Triang3l): Find if gradients are unnormalized for cube maps if
  // coordinates are unnormalized. Since texldd doesn't perform any
  // transformation for gradients (unlike for the coordinates themselves),
  // gradients are probably in cube space, which is -MA...MA, and LOD
  // calculation involves gradients in this space, so probably gradients
  // shouldn't be unnormalized.
  //
  // Adreno has only been supporting seamless cube map sampling since 3xx, so
  // the Xenos likely doesn't support seamless sampling:
  // https://developer.qualcomm.com/qfile/28557/80-nu141-1_b_adreno_opengl_es_developer_guide.pdf
  //
  // Offsets are likely applied at the LOD at which the texture is sampled (not
  // sure if to the higher-quality or to both - though "right before sampling"
  // probably means to both - in Direct3D 10, it's recommended to only use
  // offsets at integer mip levels, otherwise "you may get results that do not
  // translate well to hardware".
  kTextureFetch = 1,

  // Gets the fraction of border color that would be blended into the texture
  // data at the specified coordinates into the X component of the destination.
  // http://web.archive.org/web/20090512001222/http://msdn.microsoft.com/en-us/library/bb313945.aspx
  //
  // According to MSDN, this may take all the parameters that tfetch takes.
  kGetTextureBorderColorFrac = 16,

  // Gets the LOD for all of the pixels in the quad at the specified coordinates
  // into the X component of the destination.
  // http://web.archive.org/web/20090511233056/http://msdn.microsoft.com/en-us/library/bb313949.aspx
  //
  // According to MSDN, the only valid parameters for this are
  // UnnormalizedTextureCoords, AnisoFilter, VolMagFilter and VolMinFilter.
  // However, while XNA assembler rejects LODBias, it assembles UseComputedLOD /
  // UseRegisterLOD / UseRegisterGradients for it. It's unlikely that it takes
  // the LOD bias into account, because a getCompTexLOD + tfetch combination
  // with biases in both would result in double biasing (though not sure whether
  // grad_exp_adjust_h/v apply - in a getCompTexLOD + tfetch with
  // UseComputedLOD=false pair, gradient exponent adjustment is more logical to
  // be applied here). MipFilter also can't be overriden, the XNA assembler does
  // not assemble this instruction at all with MipFilter, so it's possible that
  // the mip filtering mode has no effect on the result (possibly should be
  // treated as linear - so fractional biasing can be done before rounding).
  //
  // Valid only in pixel shaders. Since the documentation says "for all of the
  // pixels in the quad" (with explicit gradients, this may diverge), and this
  // instruction doesn't assemble at all in vertex shaders, even with
  // UseRegisterGradients=true, it's possible that only implicit gradients may
  // be used in this instruction.
  //
  // Used with AnisoFilter=max16to1 in one place in the Source Engine.
  //
  // Not sure if the LOD should be clamped - probably not, considering an
  // out-of-range LOD passed from getCompTexLOD to setTexLOD may be biased back
  // into the range later.
  kGetTextureComputedLod = 17,

  // Source is 2-component. XZ = ddx(source.xy), YW = ddy(source.xy).
  // TODO(Triang3l): Verify whether it's coarse or fine (on Adreno 200, for
  // instance). This is using the texture unit, where the LOD is computed for
  // the whole quad (according to the Direct3D 11.3 specification), so likely
  // coarse; ddx / ddy from the Shader Model 4 era is also compiled by FXC to
  // deriv_rtx/rty_coarse when targeting Shader Model 5, and on TeraScale,
  // coarse / fine selection only appeared on Direct3D 11 GPUs.
  kGetTextureGradients = 18,

  // Gets the weights used in a bilinear fetch.
  // http://web.archive.org/web/20090511230938/http://msdn.microsoft.com/en-us/library/bb313953.aspx
  // X - horizontal lerp factor.
  // Y - vertical lerp factor.
  // Z - depth lerp factor.
  // W - mip lerp factor.
  //
  // According to MSDN, this may take all the parameters that tfetch takes.
  //
  // Takes filtering mode into account - in some games, used explicitly with
  // MagFilter=linear, MinFilter=linear. Source Engine explicitly uses this with
  // UseComputedLOD=false while only using XY of the result. Offsets and LOD
  // biasing also apply. Likely the factors are at the higher-quality LOD of the
  // pair used for filtering, though not checked.
  //
  // For cube maps, the factors are probably in SC/TC space (not sure what the
  // depth lerp factor means in case of them), since apparently there's no
  // seamless cube map sampling on the Xenos.
  kGetTextureWeights = 19,

  // Source is 1-component.
  kSetTextureLod = 24,
  // Source is 3-component.
  kSetTextureGradientsHorz = 25,
  // Source is 3-component.
  kSetTextureGradientsVert = 26,
};

struct VertexFetchInstruction {
  FetchOpcode opcode() const { return data_.opcode_value; }

  // Whether the jump is predicated (or conditional).
  bool is_predicated() const { return data_.is_predicated; }
  // Required condition value of the comparision (true or false).
  bool predicate_condition() const { return data_.pred_condition == 1; }
  // Vertex fetch constant index [0-95].
  uint32_t fetch_constant_index() const {
    return data_.const_index * 3 + data_.const_index_sel;
  }

  uint32_t dest() const { return data_.dst_reg; }
  uint32_t dest_swizzle() const { return data_.dst_swiz; }
  bool is_dest_relative() const { return data_.dst_reg_am; }
  uint32_t src() const { return data_.src_reg; }
  uint32_t src_swizzle() const { return data_.src_swiz; }
  bool is_src_relative() const { return data_.src_reg_am; }

  // Returns true if the fetch actually fetches data.
  // This may be false if it's used only to populate constants.
  bool fetches_any_data() const {
    uint32_t dst_swiz = data_.dst_swiz;
    bool fetches_any_data = false;
    for (int i = 0; i < 4; i++) {
      if ((dst_swiz & 0x7) == 4) {
        // 0.0
      } else if ((dst_swiz & 0x7) == 5) {
        // 1.0
      } else if ((dst_swiz & 0x7) == 6) {
        // ?
      } else if ((dst_swiz & 0x7) == 7) {
        // Previous register value.
      } else {
        fetches_any_data = true;
        break;
      }
      dst_swiz >>= 3;
    }
    return fetches_any_data;
  }

  uint32_t prefetch_count() const { return data_.prefetch_count; }
  bool is_mini_fetch() const { return data_.is_mini_fetch == 1; }

  xenos::VertexFormat data_format() const { return data_.format; }
  // [-32, 31]
  int exp_adjust() const { return data_.exp_adjust; }
  bool is_signed() const { return data_.fomat_comp_all == 1; }
  bool is_normalized() const { return data_.num_format_all == 0; }
  xenos::SignedRepeatingFractionMode signed_rf_mode() const {
    return data_.signed_rf_mode_all;
  }
  bool is_index_rounded() const { return data_.is_index_rounded == 1; }
  // Dword stride, [0, 255].
  uint32_t stride() const { return data_.stride; }
  // Dword offset, [-4194304, 4194303].
  int32_t offset() const { return data_.offset; }

  void AssignFromFull(const VertexFetchInstruction& full) {
    data_.stride = full.data_.stride;
    data_.const_index = full.data_.const_index;
    data_.const_index_sel = full.data_.const_index_sel;
  }

 private:
  XEPACKEDSTRUCT(Data, {
    XEPACKEDSTRUCTANONYMOUS({
      FetchOpcode opcode_value : 5;
      uint32_t src_reg : 6;
      uint32_t src_reg_am : 1;
      uint32_t dst_reg : 6;
      uint32_t dst_reg_am : 1;
      uint32_t must_be_one : 1;
      uint32_t const_index : 5;
      uint32_t const_index_sel : 2;
      // Prefetch count minus 1.
      uint32_t prefetch_count : 3;
      uint32_t src_swiz : 2;
    });
    XEPACKEDSTRUCTANONYMOUS({
      uint32_t dst_swiz : 12;
      uint32_t fomat_comp_all : 1;
      uint32_t num_format_all : 1;
      xenos::SignedRepeatingFractionMode signed_rf_mode_all : 1;
      uint32_t is_index_rounded : 1;
      xenos::VertexFormat format : 6;
      uint32_t reserved2 : 2;
      int32_t exp_adjust : 6;
      uint32_t is_mini_fetch : 1;
      uint32_t is_predicated : 1;
    });
    XEPACKEDSTRUCTANONYMOUS({
      uint32_t stride : 8;
      int32_t offset : 23;
      uint32_t pred_condition : 1;
    });
  });
  Data data_;
};

struct TextureFetchInstruction {
  FetchOpcode opcode() const { return data_.opcode_value; }

  // Whether the jump is predicated (or conditional).
  bool is_predicated() const { return data_.is_predicated; }
  // Required condition value of the comparision (true or false).
  bool predicate_condition() const { return data_.pred_condition == 1; }
  // Texture fetch constant index [0-31].
  uint32_t fetch_constant_index() const { return data_.const_index; }

  uint32_t dest() const { return data_.dst_reg; }
  uint32_t dest_swizzle() const { return data_.dst_swiz; }
  bool is_dest_relative() const { return data_.dst_reg_am; }
  uint32_t src() const { return data_.src_reg; }
  uint32_t src_swizzle() const { return data_.src_swiz; }
  bool is_src_relative() const { return data_.src_reg_am; }

  xenos::FetchOpDimension dimension() const { return data_.dimension; }
  bool fetch_valid_only() const { return data_.fetch_valid_only == 1; }
  bool unnormalized_coordinates() const { return data_.tx_coord_denorm == 1; }
  bool has_mag_filter() const {
    return data_.mag_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter mag_filter() const { return data_.mag_filter; }
  bool has_min_filter() const {
    return data_.min_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter min_filter() const { return data_.min_filter; }
  bool has_mip_filter() const {
    return data_.mip_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter mip_filter() const { return data_.mip_filter; }
  bool has_aniso_filter() const {
    return data_.aniso_filter != xenos::AnisoFilter::kUseFetchConst;
  }
  xenos::AnisoFilter aniso_filter() const { return data_.aniso_filter; }
  bool has_vol_mag_filter() const {
    return data_.vol_mag_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter vol_mag_filter() const { return data_.vol_mag_filter; }
  bool has_vol_min_filter() const {
    return data_.vol_min_filter != xenos::TextureFilter::kUseFetchConst;
  }
  xenos::TextureFilter vol_min_filter() const { return data_.vol_min_filter; }
  bool use_computed_lod() const { return data_.use_comp_lod == 1; }
  bool use_register_lod() const { return data_.use_reg_lod == 1; }
  bool use_register_gradients() const { return data_.use_reg_gradients == 1; }
  xenos::SampleLocation sample_location() const {
    return data_.sample_location;
  }
  float lod_bias() const {
    // http://web.archive.org/web/20090514012026/http://msdn.microsoft.com:80/en-us/library/bb313957.aspx
    return data_.lod_bias * (1.0f / 16.0f);
  }
  float offset_x() const { return data_.offset_x * 0.5f; }
  float offset_y() const { return data_.offset_y * 0.5f; }
  float offset_z() const { return data_.offset_z * 0.5f; }

 private:
  XEPACKEDSTRUCT(Data, {
    XEPACKEDSTRUCTANONYMOUS({
      FetchOpcode opcode_value : 5;
      uint32_t src_reg : 6;
      uint32_t src_reg_am : 1;
      uint32_t dst_reg : 6;
      uint32_t dst_reg_am : 1;
      uint32_t fetch_valid_only : 1;
      uint32_t const_index : 5;
      uint32_t tx_coord_denorm : 1;
      uint32_t src_swiz : 6;  // xyz
    });
    XEPACKEDSTRUCTANONYMOUS({
      uint32_t dst_swiz : 12;  // xyzw
      xenos::TextureFilter mag_filter : 2;
      xenos::TextureFilter min_filter : 2;
      xenos::TextureFilter mip_filter : 2;
      xenos::AnisoFilter aniso_filter : 3;
      xenos::ArbitraryFilter arbitrary_filter : 3;
      xenos::TextureFilter vol_mag_filter : 2;
      xenos::TextureFilter vol_min_filter : 2;
      uint32_t use_comp_lod : 1;
      uint32_t use_reg_lod : 1;
      uint32_t unk : 1;
      uint32_t is_predicated : 1;
    });
    XEPACKEDSTRUCTANONYMOUS({
      uint32_t use_reg_gradients : 1;
      xenos::SampleLocation sample_location : 1;
      int32_t lod_bias : 7;
      uint32_t unused : 5;
      xenos::FetchOpDimension dimension : 2;
      int32_t offset_x : 5;
      int32_t offset_y : 5;
      int32_t offset_z : 5;
      uint32_t pred_condition : 1;
    });
  });
  Data data_;
};
static_assert_size(TextureFetchInstruction, 12);

// What follows is largely a mash up of the microcode assembly naming and the
// R600 docs that have a near 1:1 with the instructions available in the xenos
// GPU, and Adreno 2xx instruction names found in Freedreno. Some of the
// behavior has been experimentally verified. Some has been guessed.
// Docs: https://www.x.org/docs/AMD/old/r600isa.pdf
//
// Conventions:
// - All temporary registers are vec4s.
// - Scalar ops swizzle out a single component of their source registers denoted
//   by 'a' or 'b'. src0.a means 'the first component specified for src0' and
//   src0.ab means 'two components specified for src0, in order'.
// - Scalar ops write the result to the entire destination register.
// - pv and ps are the previous results of a vector or scalar ALU operation.
//   Both are valid only within the current ALU clause. They are not modified
//   when the instruction that would write them fails its predication check.
// - Direct3D 9 rules (like in GCN v_*_legacy_f32 instructions) for
//   multiplication (+-0 or denormal * anything = +0) wherever it's present
//   (mul, mad, dp, etc.) and for NaN in min/max. It's very important to respect
//   this rule for multiplication, as games often rely on it in vector
//   normalization (rcp and mul), Infinity * 0 resulting in NaN breaks a lot of
//   things in games - causes white screen in Halo 3, white specular on
//   characters in GTA IV. The result is always positive zero in this case, no
//   matter what the signs of the other operands are, according to R5xx
//   Acceleration section 8.7.5 "Legacy multiply behavior" and testing on
//   Adreno 200. This means that the following need to be taken into account
//   (according to 8.7.2 "ALU Non-Transcendental Floating Point"):
//   - +0 * -0 is -0 with IEEE conformance, however, with this legacy SM3
//     handling, it should result in +0.
//   - +0 + -0 is +0, so multiply-add should not be replaced with conditional
//     move of the third operand in case of zero multiplicands, because the term
//     may be -0, while the result should be +0 in this case.
//   http://developer.amd.com/wordpress/media/2013/10/R5xx_Acceleration_v1.5.pdf
//   Multiply-add also appears to be not fused; the SM3 behavior instruction on
//   GCN is called v_mad_legacy_f32, not v_fma_legacy_f32 (in 2012-2020, before
//   RDNA 2, which removed v_mad_f32 as well) - shader translators should not
//   use instructions that may be interpreted by the host GPU as fused
//   multiply-add.

enum class AluScalarOpcode : uint32_t {
  // Floating-Point Add
  // adds/ADDs dest, src0.ab
  //     dest.xyzw = src0.a + src0.b;
  kAdds = 0,

  // Floating-Point Add (with Previous)
  // adds_prev/ADD_PREVs dest, src0.a
  //     dest.xyzw = src0.a + ps;
  kAddsPrev = 1,

  // Floating-Point Multiply
  // muls/MULs dest, src0.ab
  //     dest.xyzw = src0.a * src0.b;
  kMuls = 2,

  // Floating-Point Multiply (with Previous)
  // muls_prev/MUL_PREVs dest, src0.a
  //     dest.xyzw = src0.a * ps;
  kMulsPrev = 3,

  // Scalar Multiply Emulating LIT Operation
  // muls_prev2/MUL_PREV2s dest, src0.ab
  //    dest.xyzw =
  //        ps == -FLT_MAX || !isfinite(ps) || !isfinite(src0.b) || src0.b <= 0
  //        ? -FLT_MAX : src0.a * ps;
  kMulsPrev2 = 4,

  // Floating-Point Maximum
  // maxs/MAXs dest, src0.ab
  //     dest.xyzw = src0.a >= src0.b ? src0.a : src0.b;
  kMaxs = 5,

  // Floating-Point Minimum
  // mins/MINs dest, src0.ab
  //     dest.xyzw = src0.a < src0.b ? src0.a : src0.b;
  kMins = 6,

  // Floating-Point Set If Equal
  // seqs/SETEs dest, src0.a
  //     dest.xyzw = src0.a == 0.0 ? 1.0 : 0.0;
  kSeqs = 7,

  // Floating-Point Set If Greater Than
  // sgts/SETGTs dest, src0.a
  //     dest.xyzw = src0.a > 0.0 ? 1.0 : 0.0;
  kSgts = 8,

  // Floating-Point Set If Greater Than Or Equal
  // sges/SETGTEs dest, src0.a
  //     dest.xyzw = src0.a >= 0.0 ? 1.0 : 0.0;
  kSges = 9,

  // Floating-Point Set If Not Equal
  // snes/SETNEs dest, src0.a
  //     dest.xyzw = src0.a != 0.0 ? 1.0 : 0.0;
  kSnes = 10,

  // Floating-Point Fractional
  // frcs/FRACs dest, src0.a
  //     dest.xyzw = src0.a - floor(src0.a);
  kFrcs = 11,

  // Floating-Point Truncate
  // truncs/TRUNCs dest, src0.a
  //     dest.xyzw = src0.a >= 0 ? floor(src0.a) : -floor(-src0.a);
  kTruncs = 12,

  // Floating-Point Floor
  // floors/FLOORs dest, src0.a
  //     dest.xyzw = floor(src0.a);
  kFloors = 13,

  // Scalar Base-2 Exponent, IEEE
  // exp/EXP_IEEE dest, src0.a
  //     dest.xyzw = src0.a == 0.0 ? 1.0 : pow(2, src0.a);
  kExp = 14,

  // Scalar Base-2 Log
  // logc/LOG_CLAMP dest, src0.a
  //     float t = src0.a == 1.0 ? 0.0 : log(src0.a) / log(2.0);
  //     dest.xyzw = t == -INF ? -FLT_MAX : t;
  kLogc = 15,

  // Scalar Base-2 IEEE Log
  // log/LOG_IEEE dest, src0.a
  //     dest.xyzw = src0.a == 1.0 ? 0.0 : log(src0.a) / log(2.0);
  kLog = 16,

  // Scalar Reciprocal, Clamp to Maximum
  // rcpc/RECIP_CLAMP dest, src0.a
  //     float t = src0.a == 1.0 ? 1.0 : 1.0 / src0.a;
  //     if (t == -INF) t = -FLT_MAX;
  //     else if (t == INF) t = FLT_MAX;
  //     dest.xyzw = t;
  kRcpc = 17,

  // Scalar Reciprocal, Clamp to Zero
  // Mimicking the behavior of the fixed-function pipeline.
  // rcpf/RECIP_FF dest, src0.a
  //     float t = src0.a == 1.0 ? 1.0 : 1.0 / src0.a;
  //     if (t == -INF) t = -0.0;
  //     else if (t == INF) t = 0.0;
  //     dest.xyzw = t;
  kRcpf = 18,

  // Scalar Reciprocal, IEEE Approximation
  // rcp/RECIP_IEEE dest, src0.a
  //     dest.xyzw = src0.a == 1.0 ? 1.0 : 1.0 / src0.a;
  kRcp = 19,

  // Scalar Reciprocal Square Root, Clamp to Maximum
  // rsqc/RECIPSQ_CLAMP dest, src0.a
  //     float t = src0.a == 1.0 ? 1.0 : 1.0 / sqrt(src0.a);
  //     if (t == -INF) t = -FLT_MAX;
  //     else if (t == INF) t = FLT_MAX;
  //     dest.xyzw = t;
  kRsqc = 20,

  // Scalar Reciprocal Square Root, Clamp to Zero
  // rsqf/RECIPSQ_FF dest, src0.a
  //     float t = src0.a == 1.0 ? 1.0 : 1.0 / sqrt(src0.a);
  //     if (t == -INF) t = -0.0;
  //     else if (t == INF) t = 0.0;
  //     dest.xyzw = t;
  kRsqf = 21,

  // Scalar Reciprocal Square Root, IEEE Approximation
  // rsq/RECIPSQ_IEEE dest, src0.a
  //     dest.xyzw = src0.a == 1.0 ? 1.0 : 1.0 / sqrt(src0.a);
  kRsq = 22,

  // Floating-Point Maximum with Copy To Integer in AR
  // maxas dest, src0.ab
  // movas/MOVAs dest, src0.aa
  //     a0 = (int)clamp(floor(src0.a + 0.5), -256.0, 255.0);
  //     dest.xyzw = src0.a >= src0.b ? src0.a : src0.b;
  kMaxAs = 23,

  // Floating-Point Maximum with Copy Truncated To Integer in AR
  // maxasf dest, src0.ab
  // movasf/MOVA_FLOORs dest, src0.aa
  //     a0 = (int)clamp(floor(src0.a), -256.0, 255.0);
  //     dest.xyzw = src0.a >= src0.b ? src0.a : src0.b;
  kMaxAsf = 24,

  // Floating-Point Subtract
  // subs/SUBs dest, src0.ab
  //     dest.xyzw = src0.a - src0.b;
  kSubs = 25,

  // Floating-Point Subtract (with Previous)
  // subs_prev/SUB_PREVs dest, src0.a
  //     dest.xyzw = src0.a - ps;
  kSubsPrev = 26,

  // Floating-Point Predicate Set If Equal
  // setp_eq/PRED_SETEs dest, src0.a
  //     if (src0.a == 0.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       dest.xyzw = 1.0;
  //       p0 = 0;
  //     }
  kSetpEq = 27,

  // Floating-Point Predicate Set If Not Equal
  // setp_ne/PRED_SETNEs dest, src0.a
  //     if (src0.a != 0.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       dest.xyzw = 1.0;
  //       p0 = 0;
  //     }
  kSetpNe = 28,

  // Floating-Point Predicate Set If Greater Than
  // setp_gt/PRED_SETGTs dest, src0.a
  //     if (src0.a > 0.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       dest.xyzw = 1.0;
  //       p0 = 0;
  //     }
  kSetpGt = 29,

  // Floating-Point Predicate Set If Greater Than Or Equal
  // setp_ge/PRED_SETGTEs dest, src0.a
  //     if (src0.a >= 0.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       dest.xyzw = 1.0;
  //       p0 = 0;
  //     }
  kSetpGe = 30,

  // Predicate Counter Invert
  // setp_inv/PRED_SET_INVs dest, src0.a
  //     if (src0.a == 1.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       if (src0.a == 0.0) {
  //         dest.xyzw = 1.0;
  //       } else {
  //         dest.xyzw = src0.a;
  //       }
  //       p0 = 0;
  //     }
  kSetpInv = 31,

  // Predicate Counter Pop
  // setp_pop/PRED_SET_POPs dest, src0.a
  //     if (src0.a - 1.0 <= 0.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       dest.xyzw = src0.a - 1.0;
  //       p0 = 0;
  //     }
  kSetpPop = 32,

  // Predicate Counter Clear
  // setp_clr/PRED_SET_CLRs dest
  //     dest.xyzw = FLT_MAX;
  //     p0 = 0;
  kSetpClr = 33,

  // Predicate Counter Restore
  // setp_rstr/PRED_SET_RESTOREs dest, src0.a
  //     if (src0.a == 0.0) {
  //       dest.xyzw = 0.0;
  //       p0 = 1;
  //     } else {
  //       dest.xyzw = src0.a;
  //       p0 = 0;
  //     }
  kSetpRstr = 34,

  // Floating-Point Pixel Kill If Equal
  // kills_eq/KILLEs dest, src0.a
  //     if (src0.a == 0.0) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillsEq = 35,

  // Floating-Point Pixel Kill If Greater Than
  // kills_gt/KILLGTs dest, src0.a
  //     if (src0.a > 0.0) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillsGt = 36,

  // Floating-Point Pixel Kill If Greater Than Or Equal
  // kills_ge/KILLGTEs dest, src0.a
  //     if (src0.a >= 0.0) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillsGe = 37,

  // Floating-Point Pixel Kill If Not Equal
  // kills_ne/KILLNEs dest, src0.a
  //     if (src0.a != 0.0) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillsNe = 38,

  // Floating-Point Pixel Kill If One
  // kills_one/KILLONEs dest, src0.a
  //     if (src0.a == 1.0) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillsOne = 39,

  // Scalar Square Root, IEEE Aproximation
  // sqrt/SQRT_IEEE dest, src0.a
  //     dest.xyzw = sqrt(src0.a);
  kSqrt = 40,

  // mulsc/MUL_CONST_0 dest, src0.a, src1.a
  kMulsc0 = 42,
  // mulsc/MUL_CONST_1 dest, src0.a, src1.a
  kMulsc1 = 43,
  // addsc/ADD_CONST_0 dest, src0.a, src1.a
  kAddsc0 = 44,
  // addsc/ADD_CONST_1 dest, src0.a, src1.a
  kAddsc1 = 45,
  // subsc/SUB_CONST_0 dest, src0.a, src1.a
  kSubsc0 = 46,
  // subsc/SUB_CONST_1 dest, src0.a, src1.a
  kSubsc1 = 47,

  // Scalar Sin
  // sin/SIN dest, src0.a
  //     dest.xyzw = sin(src0.a);
  kSin = 48,

  // Scalar Cos
  // cos/COS dest, src0.a
  //     dest.xyzw = cos(src0.a);
  kCos = 49,

  // retain_prev/RETAIN_PREV dest
  //     dest.xyzw = ps;
  kRetainPrev = 50,
};

constexpr bool AluScalarOpcodeIsKill(AluScalarOpcode scalar_opcode) {
  switch (scalar_opcode) {
    case AluScalarOpcode::kKillsEq:
    case AluScalarOpcode::kKillsGt:
    case AluScalarOpcode::kKillsGe:
    case AluScalarOpcode::kKillsNe:
    case AluScalarOpcode::kKillsOne:
      return true;
    default:
      return false;
  }
}

enum class AluVectorOpcode : uint32_t {
  // Per-Component Floating-Point Add
  // add/ADDv dest, src0, src1
  //     dest.x = src0.x + src1.x;
  //     dest.y = src0.y + src1.y;
  //     dest.z = src0.z + src1.z;
  //     dest.w = src0.w + src1.w;
  kAdd = 0,

  // Per-Component Floating-Point Multiply
  // mul/MULv dest, src0, src1
  //     dest.x = src0.x * src1.x;
  //     dest.y = src0.y * src1.y;
  //     dest.z = src0.z * src1.z;
  //     dest.w = src0.w * src1.w;
  kMul = 1,

  // Per-Component Floating-Point Maximum
  // max/MAXv dest, src0, src1
  //     dest.x = src0.x >= src1.x ? src0.x : src1.x;
  //     dest.y = src0.x >= src1.y ? src0.y : src1.y;
  //     dest.z = src0.x >= src1.z ? src0.z : src1.z;
  //     dest.w = src0.x >= src1.w ? src0.w : src1.w;
  kMax = 2,

  // Per-Component Floating-Point Minimum
  // min/MINv dest, src0, src1
  //     dest.x = src0.x < src1.x ? src0.x : src1.x;
  //     dest.y = src0.x < src1.y ? src0.y : src1.y;
  //     dest.z = src0.x < src1.z ? src0.z : src1.z;
  //     dest.w = src0.x < src1.w ? src0.w : src1.w;
  kMin = 3,

  // Per-Component Floating-Point Set If Equal
  // seq/SETEv dest, src0, src1
  //     dest.x = src0.x == src1.x ? 1.0 : 0.0;
  //     dest.y = src0.y == src1.y ? 1.0 : 0.0;
  //     dest.z = src0.z == src1.z ? 1.0 : 0.0;
  //     dest.w = src0.w == src1.w ? 1.0 : 0.0;
  kSeq = 4,

  // Per-Component Floating-Point Set If Greater Than
  // sgt/SETGTv dest, src0, src1
  //     dest.x = src0.x > src1.x ? 1.0 : 0.0;
  //     dest.y = src0.y > src1.y ? 1.0 : 0.0;
  //     dest.z = src0.z > src1.z ? 1.0 : 0.0;
  //     dest.w = src0.w > src1.w ? 1.0 : 0.0;
  kSgt = 5,

  // Per-Component Floating-Point Set If Greater Than Or Equal
  // sge/SETGTEv dest, src0, src1
  //     dest.x = src0.x >= src1.x ? 1.0 : 0.0;
  //     dest.y = src0.y >= src1.y ? 1.0 : 0.0;
  //     dest.z = src0.z >= src1.z ? 1.0 : 0.0;
  //     dest.w = src0.w >= src1.w ? 1.0 : 0.0;
  kSge = 6,

  // Per-Component Floating-Point Set If Not Equal
  // sne/SETNEv dest, src0, src1
  //     dest.x = src0.x != src1.x ? 1.0 : 0.0;
  //     dest.y = src0.y != src1.y ? 1.0 : 0.0;
  //     dest.z = src0.z != src1.z ? 1.0 : 0.0;
  //     dest.w = src0.w != src1.w ? 1.0 : 0.0;
  kSne = 7,

  // Per-Component Floating-Point Fractional
  // frc/FRACv dest, src0
  //     dest.x = src0.x - floor(src0.x);
  //     dest.y = src0.y - floor(src0.y);
  //     dest.z = src0.z - floor(src0.z);
  //     dest.w = src0.w - floor(src0.w);
  kFrc = 8,

  // Per-Component Floating-Point Truncate
  // trunc/TRUNCv dest, src0
  //     dest.x = src0.x >= 0 ? floor(src0.x) : -floor(-src0.x);
  //     dest.y = src0.y >= 0 ? floor(src0.y) : -floor(-src0.y);
  //     dest.z = src0.z >= 0 ? floor(src0.z) : -floor(-src0.z);
  //     dest.w = src0.w >= 0 ? floor(src0.w) : -floor(-src0.w);
  kTrunc = 9,

  // Per-Component Floating-Point Floor
  // floor/FLOORv dest, src0
  //     dest.x = floor(src0.x);
  //     dest.y = floor(src0.y);
  //     dest.z = floor(src0.z);
  //     dest.w = floor(src0.w);
  kFloor = 10,

  // Per-Component Floating-Point Multiply-Add
  // mad/MULADDv dest, src0, src1, src2
  //     dest.x = src0.x * src1.x + src2.x;
  //     dest.y = src0.y * src1.y + src2.y;
  //     dest.z = src0.z * src1.z + src2.z;
  //     dest.w = src0.w * src1.w + src2.w;
  kMad = 11,

  // Per-Component Floating-Point Conditional Move If Equal
  // cndeq/CNDEv dest, src0, src1, src2
  //     dest.x = src0.x == 0.0 ? src1.x : src2.x;
  //     dest.y = src0.y == 0.0 ? src1.y : src2.y;
  //     dest.z = src0.z == 0.0 ? src1.z : src2.z;
  //     dest.w = src0.w == 0.0 ? src1.w : src2.w;
  kCndEq = 12,

  // Per-Component Floating-Point Conditional Move If Greater Than Or Equal
  // cndge/CNDGTEv dest, src0, src1, src2
  //     dest.x = src0.x >= 0.0 ? src1.x : src2.x;
  //     dest.y = src0.y >= 0.0 ? src1.y : src2.y;
  //     dest.z = src0.z >= 0.0 ? src1.z : src2.z;
  //     dest.w = src0.w >= 0.0 ? src1.w : src2.w;
  kCndGe = 13,

  // Per-Component Floating-Point Conditional Move If Greater Than
  // cndgt/CNDGTv dest, src0, src1, src2
  //     dest.x = src0.x > 0.0 ? src1.x : src2.x;
  //     dest.y = src0.y > 0.0 ? src1.y : src2.y;
  //     dest.z = src0.z > 0.0 ? src1.z : src2.z;
  //     dest.w = src0.w > 0.0 ? src1.w : src2.w;
  kCndGt = 14,

  // Four-Element Dot Product
  // dp4/DOT4v dest, src0, src1
  //     dest.xyzw = src0.x * src1.x + src0.y * src1.y + src0.z * src1.z +
  //                 src0.w * src1.w;
  // Note: only pv.x contains the value.
  kDp4 = 15,

  // Three-Element Dot Product
  // dp3/DOT3v dest, src0, src1
  //     dest.xyzw = src0.x * src1.x + src0.y * src1.y + src0.z * src1.z;
  // Note: only pv.x contains the value.
  kDp3 = 16,

  // Two-Element Dot Product and Add
  // dp2add/DOT2ADDv dest, src0, src1, src2
  //     dest.xyzw = src0.x * src1.x + src0.y * src1.y + src2.x;
  // Note: only pv.x contains the value.
  kDp2Add = 17,

  // Cube Map
  // cube/CUBEv dest, src0, src1
  //     dest.x = T cube coordinate;
  //     dest.y = S cube coordinate;
  //     dest.z = 2.0 * major axis;
  //     dest.w = FaceID;
  // https://developer.amd.com/wordpress/media/2012/12/AMD_Southern_Islands_Instruction_Set_Architecture.pdf
  //     if (abs(z) >= abs(x) && abs(z) >= abs(y)) {
  //       tc = -y;
  //       sc = z < 0.0 ? -x : x;
  //       ma = 2.0 * z;
  //       id = z < 0.0 ? 5.0 : 4.0;
  //     } else if (abs(y) >= abs(x)) {
  //       tc = y < 0.0 ? -z : z;
  //       sc = x;
  //       ma = 2.0 * y;
  //       id = y < 0.0 ? 3.0 : 2.0;
  //     } else {
  //       tc = -y;
  //       sc = x < 0.0 ? z : -z;
  //       ma = 2.0 * x;
  //       id = x < 0.0 ? 1.0 : 0.0;
  //     }
  // Expects src0.zzxy and src1.yxzz swizzles.
  // FaceID is D3DCUBEMAP_FACES:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/bb172528(v=vs.85).aspx
  // Used like:
  //     cube r0, source.zzxy, source.yxz
  //     rcp r0.z, r0_abs.z
  //     mad r0.xy, r0, r0.zzzw, 1.5f
  //     tfetchCube r0, r0.yxw, tf0
  // http://web.archive.org/web/20100705154143/http://msdn.microsoft.com/en-us/library/bb313921.aspx
  // On GCN, the sequence is the same, so GCN documentation can be used as a
  // reference (tfetchCube doesn't accept the ST as if the texture was a 2D
  // array in XY exactly, to get texture array ST, 1 must be subtracted from its
  // XY inputs).
  // https://gpuopen.com/learn/fetching-from-cubes-and-octahedrons/
  // "The 1.5 constant is designed such that the output face coordinate (v4 and
  //  v5 in the above example) range is {1.0 <= x < 2.0} which has an advantage
  //  in bit encoding compared to {0.0 <= x < 1.0} in that the upper mantissa
  //  bits are constant throughout the entire output range."
  kCube = 18,

  // Four-Element Maximum
  // max4/MAX4v dest, src0
  //     dest.xyzw = max(src0.x, src0.y, src0.z, src0.w);
  // Note: only pv.x contains the value.
  kMax4 = 19,

  // Floating-Point Predicate Counter Increment If Equal
  // setp_eq_push/PRED_SETE_PUSHv dest, src0, src1
  //     if (src0.w == 0.0 && src1.w == 0.0) {
  //       p0 = 1;
  //     } else {
  //       p0 = 0;
  //     }
  //     if (src0.x == 0.0 && src1.x == 0.0) {
  //       dest.xyzw = 0.0;
  //     } else {
  //       dest.xyzw = src0.x + 1.0;
  //     }
  kSetpEqPush = 20,

  // Floating-Point Predicate Counter Increment If Not Equal
  // setp_ne_push/PRED_SETNE_PUSHv dest, src0, src1
  //     if (src0.w == 0.0 && src1.w != 0.0) {
  //       p0 = 1;
  //     } else {
  //       p0 = 0;
  //     }
  //     if (src0.x == 0.0 && src1.x != 0.0) {
  //       dest.xyzw = 0.0;
  //     } else {
  //       dest.xyzw = src0.x + 1.0;
  //     }
  kSetpNePush = 21,

  // Floating-Point Predicate Counter Increment If Greater Than
  // setp_gt_push/PRED_SETGT_PUSHv dest, src0, src1
  //     if (src0.w == 0.0 && src1.w > 0.0) {
  //       p0 = 1;
  //     } else {
  //       p0 = 0;
  //     }
  //     if (src0.x == 0.0 && src1.x > 0.0) {
  //       dest.xyzw = 0.0;
  //     } else {
  //       dest.xyzw = src0.x + 1.0;
  //     }
  kSetpGtPush = 22,

  // Floating-Point Predicate Counter Increment If Greater Than Or Equal
  // setp_ge_push/PRED_SETGTE_PUSHv dest, src0, src1
  //     if (src0.w == 0.0 && src1.w >= 0.0) {
  //       p0 = 1;
  //     } else {
  //       p0 = 0;
  //     }
  //     if (src0.x == 0.0 && src1.x >= 0.0) {
  //       dest.xyzw = 0.0;
  //     } else {
  //       dest.xyzw = src0.x + 1.0;
  //     }
  kSetpGePush = 23,

  // Floating-Point Pixel Kill If Equal
  // kill_eq/KILLEv dest, src0, src1
  //     if (src0.x == src1.x ||
  //         src0.y == src1.y ||
  //         src0.z == src1.z ||
  //         src0.w == src1.w) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillEq = 24,

  // Floating-Point Pixel Kill If Greater Than
  // kill_gt/KILLGTv dest, src0, src1
  //     if (src0.x > src1.x ||
  //         src0.y > src1.y ||
  //         src0.z > src1.z ||
  //         src0.w > src1.w) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillGt = 25,

  // Floating-Point Pixel Kill If Equal
  // kill_ge/KILLGTEv dest, src0, src1
  //     if (src0.x >= src1.x ||
  //         src0.y >= src1.y ||
  //         src0.z >= src1.z ||
  //         src0.w >= src1.w) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillGe = 26,

  // Floating-Point Pixel Kill If Equal
  // kill_ne/KILLNEv dest, src0, src1
  //     if (src0.x != src1.x ||
  //         src0.y != src1.y ||
  //         src0.z != src1.z ||
  //         src0.w != src1.w) {
  //       dest.xyzw = 1.0;
  //       discard;
  //     } else {
  //       dest.xyzw = 0.0;
  //     }
  kKillNe = 27,

  // dst/DSTv dest, src0, src1
  //     dest.x = 1.0;
  //     dest.y = src0.y * src1.y;
  //     dest.z = src0.z;
  //     dest.w = src1.w;
  kDst = 28,

  // Per-Component Floating-Point Maximum with Copy To Integer in AR
  // maxa dest, src0, src1
  // This is a combined max + mova/MOVAv.
  //     a0 = (int)clamp(floor(src0.w + 0.5), -256.0, 255.0);
  //     dest.x = src0.x >= src1.x ? src0.x : src1.x;
  //     dest.y = src0.x >= src1.y ? src0.y : src1.y;
  //     dest.z = src0.x >= src1.z ? src0.z : src1.z;
  //     dest.w = src0.x >= src1.w ? src0.w : src1.w;
  // The MSDN documentation specifies clamp as:
  // if (!(SQResultF >= -256.0)) {
  //   SQResultF = -256.0;
  // }
  // if (SQResultF > 255.0) {
  //   SQResultF = 255.0;
  // }
  // http://web.archive.org/web/20100705151335/http://msdn.microsoft.com:80/en-us/library/bb313931.aspx
  // However, using NaN as an address would be unusual.
  kMaxA = 29,
};

constexpr bool AluVectorOpcodeIsKill(AluVectorOpcode vector_opcode) {
  switch (vector_opcode) {
    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe:
      return true;
    default:
      return false;
  }
}

// Whether the vector instruction has side effects such as discarding a pixel or
// setting the predicate and can't be ignored even if it doesn't write to
// anywhere. Note that all scalar operations except for retain_prev have a side
// effect of modifying the previous scalar result register, so they must always
// be executed even if not writing.
constexpr bool AluVectorOpHasSideEffects(AluVectorOpcode vector_opcode) {
  if (AluVectorOpcodeIsKill(vector_opcode)) {
    return true;
  }
  switch (vector_opcode) {
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush:
    case AluVectorOpcode::kMaxA:
      return true;
    default:
      return false;
  }
}

// Whether each component of a source operand is used at all in the instruction
// (doesn't check the operand count though).
constexpr uint32_t GetAluVectorOpUsedSourceComponents(
    AluVectorOpcode vector_opcode, uint32_t src_index) {
  assert_not_zero(src_index);
  switch (vector_opcode) {
    case AluVectorOpcode::kDp3:
      return 0b0111;
    case AluVectorOpcode::kDp2Add:
      return src_index == 3 ? 0b0001 : 0b0011;
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush:
      return 0b1001;
    case AluVectorOpcode::kDst:
      return src_index == 2 ? 0b1010 : 0b0110;
    default:
      break;
  }
  return 0b1111;
}

// Whether each component of a source operand is needed for the instruction if
// executed with the specified write mask, and thus can't be thrown away or be
// undefined in translation. For per-component operations, for example, only the
// components specified in the write mask are needed, but there are instructions
// with special behavior for certain components.
constexpr uint32_t GetAluVectorOpNeededSourceComponents(
    AluVectorOpcode vector_opcode, uint32_t src_index,
    uint32_t used_result_components) {
  assert_not_zero(src_index);
  assert_zero(used_result_components & ~uint32_t(0b1111));
  uint32_t components = used_result_components;
  switch (vector_opcode) {
    case AluVectorOpcode::kDp4:
    case AluVectorOpcode::kMax4:
      components = used_result_components ? 0b1111 : 0;
      break;
    case AluVectorOpcode::kDp3:
      components = used_result_components ? 0b0111 : 0;
      break;
    case AluVectorOpcode::kDp2Add:
      components =
          used_result_components ? (src_index == 3 ? 0b0001 : 0b0011) : 0;
      break;
    case AluVectorOpcode::kCube:
      components = used_result_components ? 0b1111 : 0;
      break;
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush:
      components = used_result_components ? 0b1001 : 0b1000;
      break;
    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe:
      components = 0b1111;
      break;
    // kDst is per-component, but not all components are used -
    // GetAluVectorOpUsedSourceComponents will filter out the unused ones.
    case AluVectorOpcode::kMaxA:
      if (src_index == 1) {
        components |= 0b1000;
      }
      break;
    default:
      break;
  }
  return components &
         GetAluVectorOpUsedSourceComponents(vector_opcode, src_index);
}

enum class ExportRegister : uint32_t {
  kVSInterpolator0 = 0,
  kVSInterpolator1,
  kVSInterpolator2,
  kVSInterpolator3,
  kVSInterpolator4,
  kVSInterpolator5,
  kVSInterpolator6,
  kVSInterpolator7,
  kVSInterpolator8,
  kVSInterpolator9,
  kVSInterpolator10,
  kVSInterpolator11,
  kVSInterpolator12,
  kVSInterpolator13,
  kVSInterpolator14,
  kVSInterpolator15,

  kVSPosition = 62,

  // See R6xx/R7xx registers for details (USE_VTX_POINT_SIZE, USE_VTX_EDGE_FLAG,
  // USE_VTX_KILL_FLAG).
  // X - PSIZE (gl_PointSize).
  // Y - EDGEFLAG (glEdgeFlag) for PrimitiveType::kPolygon wireframe/point
  //     drawing.
  // Z - KILLVERTEX flag (used in Banjo-Kazooie: Nuts & Bolts for grass), set
  //     for killing primitives based on PA_CL_CLIP_CNTL::VTX_KILL_OR condition.
  kVSPointSizeEdgeFlagKillVertex = 63,

  kPSColor0 = 0,
  kPSColor1,
  kPSColor2,
  kPSColor3,

  // In X.
  kPSDepth = 61,

  // Memory export: index.?y?? * 0100 + xe_gpu_memexport_stream_t.xyzw.
  kExportAddress = 32,
  // Memory export: values for texels [index+0], [index+1], ..., [index+4].
  kExportData0 = 33,
  kExportData1,
  kExportData2,
  kExportData3,
  kExportData4,
};

struct AluInstruction {
  // Raw accessors.

  // Whether data is being exported (or written to local registers).
  bool is_export() const { return data_.export_data == 1; }
  bool export_write_mask() const { return data_.scalar_dest_rel == 1; }

  // Whether the jump is predicated (or conditional).
  bool is_predicated() const { return data_.is_predicated; }
  // Required condition value of the comparision (true or false).
  bool predicate_condition() const { return data_.pred_condition == 1; }

  bool abs_constants() const { return data_.abs_constants == 1; }
  bool is_const_0_addressed() const { return data_.const_0_rel_abs == 1; }
  bool is_const_1_addressed() const { return data_.const_1_rel_abs == 1; }
  bool is_address_relative() const { return data_.address_absolute == 1; }

  AluVectorOpcode vector_opcode() const { return data_.vector_opc; }
  uint32_t vector_write_mask() const { return data_.vector_write_mask; }
  uint32_t vector_dest() const { return data_.vector_dest; }
  bool is_vector_dest_relative() const { return data_.vector_dest_rel == 1; }
  bool vector_clamp() const { return data_.vector_clamp == 1; }

  AluScalarOpcode scalar_opcode() const { return data_.scalar_opc; }
  uint32_t scalar_write_mask() const { return data_.scalar_write_mask; }
  uint32_t scalar_dest() const { return data_.scalar_dest; }
  bool is_scalar_dest_relative() const { return data_.scalar_dest_rel == 1; }
  bool scalar_clamp() const { return data_.scalar_clamp == 1; }

  uint32_t src_reg(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_reg;
      case 2:
        return data_.src2_reg;
      case 3:
        return data_.src3_reg;
      default:
        assert_unhandled_case(i);
        return 0;
    }
  }
  bool src_is_temp(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_sel == 1;
      case 2:
        return data_.src2_sel == 1;
      case 3:
        return data_.src3_sel == 1;
      default:
        assert_unhandled_case(i);
        return 0;
    }
  }
  uint32_t src_swizzle(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_swiz;
      case 2:
        return data_.src2_swiz;
      case 3:
        return data_.src3_swiz;
      default:
        assert_unhandled_case(i);
        return 0;
    }
  }
  bool src_negate(size_t i) const {
    switch (i) {
      case 1:
        return data_.src1_reg_negate == 1;
      case 2:
        return data_.src2_reg_negate == 1;
      case 3:
        return data_.src3_reg_negate == 1;
      default:
        assert_unhandled_case(i);
        return 0;
    }
  }

  // Helpers.

  // Note that even if the export component is unused (like W of the vertex
  // shader misc register, YZW of pixel shader depth), it must still not be
  // excluded - that may make disassembly not reassemblable if there are
  // constant 0 writes in the export, like, oPts.x000 will be assembled, but
  // oPts.x00_ will not, even though W has no effect on anything.
  uint32_t GetVectorOpResultWriteMask() const {
    uint32_t mask = vector_write_mask();
    if (is_export()) {
      mask &= ~scalar_write_mask();
    }
    return mask;
  }
  uint32_t GetScalarOpResultWriteMask() const {
    uint32_t mask = scalar_write_mask();
    if (is_export()) {
      mask &= ~vector_write_mask();
    }
    return mask;
  }
  uint32_t GetConstant0WriteMask() const {
    if (!is_export() || !is_scalar_dest_relative()) {
      return 0b0000;
    }
    return 0b1111 & ~(vector_write_mask() | scalar_write_mask());
  }
  uint32_t GetConstant1WriteMask() const {
    if (!is_export()) {
      return 0b0000;
    }
    return vector_write_mask() & scalar_write_mask();
  }

 private:
  XEPACKEDSTRUCT(Data, {
    XEPACKEDSTRUCTANONYMOUS({
      // If exporting, both vector and scalar operations use the vector
      // destination (which can't be relative in this case).
      // Not very important note: If both scalar and vector operations exporting
      // something have empty write mask, the XNA assembler forces vector_dest
      // to 0 (interpolator 0 or color 0) directly in the microcode.
      uint32_t vector_dest : 6;
      uint32_t vector_dest_rel : 1;
      uint32_t abs_constants : 1;
      uint32_t scalar_dest : 6;
      uint32_t scalar_dest_rel : 1;
      // Exports have different write masking (export is done to vector_dest by
      // both the vector and the scalar operation, and exports can write
      // constant 0 and 1). For each component:
      // - vector_write_mask 0, scalar_write_mask 0:
      //   - scalar_dest_rel 0 - unchanged.
      //   - scalar_dest_rel 1 - constant 0 (all components must be written).
      // - vector_write_mask 1, scalar_write_mask 0 - from vector operation.
      // - vector_write_mask 0, scalar_write_mask 1 - from scalar operation.
      // - vector_write_mask 1, scalar_write_mask 1 - constant 1.
      uint32_t export_data : 1;
      uint32_t vector_write_mask : 4;
      uint32_t scalar_write_mask : 4;
      uint32_t vector_clamp : 1;
      uint32_t scalar_clamp : 1;
      AluScalarOpcode scalar_opc : 6;
    });
    XEPACKEDSTRUCTANONYMOUS({
      uint32_t src3_swiz : 8;
      uint32_t src2_swiz : 8;
      uint32_t src1_swiz : 8;
      uint32_t src3_reg_negate : 1;
      uint32_t src2_reg_negate : 1;
      uint32_t src1_reg_negate : 1;
      uint32_t pred_condition : 1;
      uint32_t is_predicated : 1;
      uint32_t address_absolute : 1;
      uint32_t const_1_rel_abs : 1;
      uint32_t const_0_rel_abs : 1;
    });
    XEPACKEDSTRUCTANONYMOUS({
      uint32_t src3_reg : 8;
      uint32_t src2_reg : 8;
      uint32_t src1_reg : 8;
      AluVectorOpcode vector_opc : 5;
      uint32_t src3_sel : 1;
      uint32_t src2_sel : 1;
      uint32_t src1_sel : 1;
    });
  });
  Data data_;
};
static_assert_size(AluInstruction, 12);

}  // namespace ucode
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_UCODE_H_
