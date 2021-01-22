/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GRAPHICS_CONTEXT_H_
#define XENIA_UI_GRAPHICS_CONTEXT_H_

#include <memory>
#include <vector>

namespace xe {
namespace ui {

class GraphicsProvider;
class ImmediateDrawer;
class Window;

class RawImage {
 public:
  RawImage() = default;
  ~RawImage() = default;

  size_t width = 0;
  size_t height = 0;
  size_t stride = 0;
  std::vector<uint8_t> data;
};

class GraphicsContext {
 public:
  virtual ~GraphicsContext();

  GraphicsProvider* provider() const { return provider_; }
  Window* target_window() const { return target_window_; }
  bool is_offscreen() { return immediate_drawer() == nullptr; }

  virtual ImmediateDrawer* immediate_drawer() = 0;

  virtual bool is_current();
  virtual bool MakeCurrent();
  virtual void ClearCurrent();

  // Returns true if the OS took away our context because we caused a TDR or
  // some other outstanding error. When this happens, this context, as well as
  // any other shared contexts are junk.
  // This context must be made current in order for this call to work properly.
  virtual bool WasLost() = 0;

  // Returns true if able to draw now (the target surface is available).
  virtual bool BeginSwap() = 0;
  virtual void EndSwap() = 0;

  virtual std::unique_ptr<RawImage> Capture() = 0;

 protected:
  explicit GraphicsContext(GraphicsProvider* provider, Window* target_window);

  static void GetClearColor(float* rgba);

  GraphicsProvider* provider_ = nullptr;
  Window* target_window_ = nullptr;
};

struct GraphicsContextLock {
  explicit GraphicsContextLock(GraphicsContext* context) : context_(context) {
    was_current_ = context_->is_current();
    if (!was_current_) {
      context_->MakeCurrent();
    }
  }
  ~GraphicsContextLock() {
    if (!was_current_) {
      context_->ClearCurrent();
    }
  }

 private:
  bool was_current_ = false;
  GraphicsContext* context_ = nullptr;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GRAPHICS_CONTEXT_H_
