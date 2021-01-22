/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cinttypes>
#include <cstring>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/main.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/shader_translator.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/ui/spirv/spirv_disassembler.h"

// For D3DDisassemble:
#if XE_PLATFORM_WIN32
#include "xenia/ui/d3d12/d3d12_api.h"
#endif  // XE_PLATFORM_WIN32

DEFINE_path(shader_input, "", "Input shader binary file path.", "GPU");
DEFINE_string(shader_input_type, "",
              "'vs', 'ps', or unspecified to infer from the given filename.",
              "GPU");
DEFINE_path(shader_output, "", "Output shader file path.", "GPU");
DEFINE_string(shader_output_type, "ucode",
              "Translator to use: [ucode, spirv, spirvtext, dxbc, dxbctext].",
              "GPU");
DEFINE_string(
    vertex_shader_output_type, "",
    "Type of the host interface to produce the vertex or domain shader for: "
    "[vertex or unspecified, linedomaincp, linedomainpatch, triangledomaincp, "
    "triangledomainpatch, quaddomaincp, quaddomainpatch].",
    "GPU");
DEFINE_bool(shader_output_bindless_resources, false,
            "Output host shader with bindless resources used.", "GPU");
DEFINE_bool(shader_output_dxbc_rov, false,
            "Output ROV-based output-merger code in DXBC pixel shaders.",
            "GPU");

namespace xe {
namespace gpu {

int shader_compiler_main(const std::vector<std::string>& args) {
  xenos::ShaderType shader_type;
  if (!cvars::shader_input_type.empty()) {
    if (cvars::shader_input_type == "vs") {
      shader_type = xenos::ShaderType::kVertex;
    } else if (cvars::shader_input_type == "ps") {
      shader_type = xenos::ShaderType::kPixel;
    } else {
      XELOGE("Invalid --shader_input_type; must be 'vs' or 'ps'.");
      return 1;
    }
  } else {
    bool valid_type = false;
    if (cvars::shader_input.has_extension()) {
      auto extension = cvars::shader_input.extension();
      if (extension == ".vs") {
        shader_type = xenos::ShaderType::kVertex;
        valid_type = true;
      } else if (extension == ".ps") {
        shader_type = xenos::ShaderType::kPixel;
        valid_type = true;
      }
    }
    if (!valid_type) {
      XELOGE(
          "File type not recognized (use .vs, .ps or "
          "--shader_input_type=vs|ps).");
      return 1;
    }
  }

  auto input_file = filesystem::OpenFile(cvars::shader_input, "rb");
  if (!input_file) {
    XELOGE("Unable to open input file: {}",
           xe::path_to_utf8(cvars::shader_input));
    return 1;
  }
  fseek(input_file, 0, SEEK_END);
  size_t input_file_size = ftell(input_file);
  fseek(input_file, 0, SEEK_SET);
  std::vector<uint32_t> ucode_dwords(input_file_size / 4);
  fread(ucode_dwords.data(), 4, ucode_dwords.size(), input_file);
  fclose(input_file);

  XELOGI("Opened {} as a {} shader, {} words ({} bytes).",
         xe::path_to_utf8(cvars::shader_input),
         shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
         ucode_dwords.size(), ucode_dwords.size() * 4);

  // TODO(benvanik): hash? need to return the data to big-endian format first.
  uint64_t ucode_data_hash = 0;
  auto shader = std::make_unique<Shader>(
      shader_type, ucode_data_hash, ucode_dwords.data(), ucode_dwords.size());

  StringBuffer ucode_disasm_buffer;
  shader->AnalyzeUcode(ucode_disasm_buffer);

  std::unique_ptr<ShaderTranslator> translator;
  if (cvars::shader_output_type == "spirv" ||
      cvars::shader_output_type == "spirvtext") {
    translator = std::make_unique<SpirvShaderTranslator>();
  } else if (cvars::shader_output_type == "dxbc" ||
             cvars::shader_output_type == "dxbctext") {
    translator = std::make_unique<DxbcShaderTranslator>(
        0, cvars::shader_output_bindless_resources,
        cvars::shader_output_dxbc_rov);
  } else {
    // Just output microcode disassembly generated during microcode information
    // gathering.
    if (!cvars::shader_output.empty()) {
      auto output_file = filesystem::OpenFile(cvars::shader_output, "wb");
      fwrite(shader->ucode_disassembly().c_str(), 1,
             shader->ucode_disassembly().length(), output_file);
      fclose(output_file);
    }
    return 0;
  }

  Shader::HostVertexShaderType host_vertex_shader_type =
      Shader::HostVertexShaderType::kVertex;
  if (shader_type == xenos::ShaderType::kVertex) {
    if (cvars::vertex_shader_output_type == "linedomaincp") {
      host_vertex_shader_type =
          Shader::HostVertexShaderType::kLineDomainCPIndexed;
    } else if (cvars::vertex_shader_output_type == "linedomainpatch") {
      host_vertex_shader_type =
          Shader::HostVertexShaderType::kLineDomainPatchIndexed;
    } else if (cvars::vertex_shader_output_type == "triangledomaincp") {
      host_vertex_shader_type =
          Shader::HostVertexShaderType::kTriangleDomainCPIndexed;
    } else if (cvars::vertex_shader_output_type == "triangledomainpatch") {
      host_vertex_shader_type =
          Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
    } else if (cvars::vertex_shader_output_type == "quaddomaincp") {
      host_vertex_shader_type =
          Shader::HostVertexShaderType::kQuadDomainCPIndexed;
    } else if (cvars::vertex_shader_output_type == "quaddomainpatch") {
      host_vertex_shader_type =
          Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
    }
  }
  uint64_t modification = translator->GetDefaultModification(
      shader_type, 64, host_vertex_shader_type);

  Shader::Translation* translation =
      shader->GetOrCreateTranslation(modification);
  translator->TranslateAnalyzedShader(*translation);

  const void* source_data = translation->translated_binary().data();
  size_t source_data_size = translation->translated_binary().size();

  std::unique_ptr<xe::ui::spirv::SpirvDisassembler::Result> spirv_disasm_result;
  if (cvars::shader_output_type == "spirvtext") {
    // Disassemble SPIRV.
    spirv_disasm_result = xe::ui::spirv::SpirvDisassembler().Disassemble(
        reinterpret_cast<const uint32_t*>(source_data), source_data_size / 4);
    source_data = spirv_disasm_result->text();
    source_data_size = std::strlen(spirv_disasm_result->text()) + 1;
  }
#if XE_PLATFORM_WIN32
  ID3DBlob* dxbc_disasm_blob = nullptr;
  if (cvars::shader_output_type == "dxbctext") {
    HMODULE d3d_compiler = LoadLibraryW(L"D3DCompiler_47.dll");
    if (d3d_compiler != nullptr) {
      pD3DDisassemble d3d_disassemble =
          pD3DDisassemble(GetProcAddress(d3d_compiler, "D3DDisassemble"));
      if (d3d_disassemble != nullptr) {
        // Disassemble DXBC.
        if (SUCCEEDED(d3d_disassemble(source_data, source_data_size,
                                      D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING |
                                          D3D_DISASM_ENABLE_INSTRUCTION_OFFSET,
                                      nullptr, &dxbc_disasm_blob))) {
          source_data = dxbc_disasm_blob->GetBufferPointer();
          source_data_size = dxbc_disasm_blob->GetBufferSize();
          // Stop at the null terminator.
          for (size_t i = 0; i < source_data_size; ++i) {
            if (reinterpret_cast<const char*>(source_data)[i] == '\0') {
              source_data_size = i;
              break;
            }
          }
        }
      }
      FreeLibrary(d3d_compiler);
    }
  }
#endif  // XE_PLATFORM_WIN32

  if (!cvars::shader_output.empty()) {
    auto output_file = filesystem::OpenFile(cvars::shader_output, "wb");
    fwrite(source_data, 1, source_data_size, output_file);
    fclose(output_file);
  }

#if XE_PLATFORM_WIN32
  if (dxbc_disasm_blob != nullptr) {
    dxbc_disasm_blob->Release();
  }
#endif  // XE_PLATFORM_WIN32

  return 0;
}

}  // namespace gpu
}  // namespace xe

DEFINE_ENTRY_POINT("xenia-gpu-shader-compiler", xe::gpu::shader_compiler_main,
                   "shader.bin", "shader_input");
