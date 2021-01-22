/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_TRACE_VIEWER_H_
#define XENIA_GPU_TRACE_VIEWER_H_

#include <string>

#include "xenia/emulator.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/trace_player.h"
#include "xenia/gpu/trace_protocol.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

namespace xe {
namespace ui {
class Loop;
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace gpu {

struct SamplerInfo;
struct TextureInfo;

class TraceViewer {
 public:
  virtual ~TraceViewer();

  int Main(const std::vector<std::string>& args);

 protected:
  TraceViewer();

  virtual std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem() = 0;

  void DrawMultilineString(const std::string_view str);

  virtual uintptr_t GetColorRenderTarget(
      uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
      xenos::ColorRenderTargetFormat format) = 0;
  virtual uintptr_t GetDepthRenderTarget(
      uint32_t pitch, xenos::MsaaSamples samples, uint32_t base,
      xenos::DepthRenderTargetFormat format) = 0;
  virtual uintptr_t GetTextureEntry(const TextureInfo& texture_info,
                                    const SamplerInfo& sampler_info) = 0;

  virtual size_t QueryVSOutputSize() { return 0; }
  virtual size_t QueryVSOutputElementSize() { return 0; }
  virtual bool QueryVSOutput(void* buffer, size_t size) { return false; }

  virtual bool Setup();

  std::unique_ptr<xe::ui::Loop> loop_;
  std::unique_ptr<xe::ui::Window> window_;
  std::unique_ptr<Emulator> emulator_;
  Memory* memory_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  std::unique_ptr<TracePlayer> player_;

 private:
  enum class ShaderDisplayType : int {
    kUcode,
    kTranslated,
    kHostDisasm,
  };

  bool Load(const std::filesystem::path& trace_file_path);
  void Run();

  void DrawUI();
  void DrawControllerUI();
  void DrawPacketDisassemblerUI();
  int RecursiveDrawCommandBufferUI(const TraceReader::Frame* frame,
                                   TraceReader::CommandBuffer* buffer);
  void DrawCommandListUI();
  void DrawStateUI();

  ShaderDisplayType DrawShaderTypeUI();
  void DrawShaderUI(Shader* shader, ShaderDisplayType display_type);

  void DrawBlendMode(uint32_t src_blend, uint32_t dest_blend,
                     uint32_t blend_op);

  void DrawTextureInfo(const Shader::TextureBinding& texture_binding);
  void DrawFailedTextureInfo(const Shader::TextureBinding& texture_binding,
                             const char* message);

  void DrawVertexFetcher(Shader* shader,
                         const Shader::VertexBinding& vertex_binding,
                         const xenos::xe_gpu_vertex_fetch_t* fetch);
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_TRACE_VIEWER_H_
