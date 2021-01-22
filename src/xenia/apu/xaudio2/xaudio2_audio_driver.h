/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_XAUDIO2_XAUDIO2_AUDIO_DRIVER_H_
#define XENIA_APU_XAUDIO2_XAUDIO2_AUDIO_DRIVER_H_

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/xaudio2/xaudio2_api.h"
#include "xenia/base/threading.h"

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace xe {
namespace apu {
namespace xaudio2 {

class XAudio2AudioDriver : public AudioDriver {
 public:
  XAudio2AudioDriver(Memory* memory, xe::threading::Semaphore* semaphore);
  ~XAudio2AudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 private:
  template <typename Objects>
  bool InitializeObjects(Objects& objects);
  template <typename Objects>
  void ShutdownObjects(Objects& objects);

  void* xaudio2_module_ = nullptr;
  uint32_t api_minor_version_ = 7;
  union {
    struct {
      api::IXAudio2_7* audio;
      api::IXAudio2_7MasteringVoice* mastering_voice;
      api::IXAudio2_7SourceVoice* pcm_voice;
    } api_2_7;
    struct {
      api::IXAudio2_8* audio;
      api::IXAudio2_8MasteringVoice* mastering_voice;
      api::IXAudio2_8SourceVoice* pcm_voice;
    } api_2_8;
  } objects_ = {};
  xe::threading::Semaphore* semaphore_ = nullptr;

  class VoiceCallback;
  VoiceCallback* voice_callback_ = nullptr;

  static const uint32_t frame_count_ = api::XE_XAUDIO2_MAX_QUEUED_BUFFERS;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  float frames_[frame_count_][frame_samples_];
  uint32_t current_frame_ = 0;
};

}  // namespace xaudio2
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XAUDIO2_XAUDIO2_AUDIO_DRIVER_H_
