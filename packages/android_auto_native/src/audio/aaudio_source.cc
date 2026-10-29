// SPDX-License-Identifier: GPL-3.0-or-later
// PcmSource on Android, through AAudio. The only file that names AAudio for capture.
//
// Opened only when the phone asks for the microphone, and closed when it lets go, the
// same rule as on Linux. Android adds one condition of its own: the host app has to hold
// RECORD_AUDIO. Without it the open fails, AudioInput falls back to silence, and the
// phone hears a head unit with no microphone rather than nothing at all.

#include <aaudio/AAudio.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "pcm_source.h"

namespace aa {
namespace {

constexpr int64_t kReadTimeoutNanos = 500 * 1000 * 1000;

class AAudioSource : public PcmSource {
 public:
  ~AAudioSource() override { Close(); }

  bool Open(const PcmFormat& format, const std::string&, const std::string&,
            std::string* error) override {
    Close();
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
      if (error != nullptr) *error = AAudio_convertResultToText(result);
      return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, format.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, format.channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    // Voice recognition is the preset that turns the platform's echo canceller and
    // noise suppressor on where it has them, which is what the Assistant wants in a car.
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
      stream_ = nullptr;
      if (error != nullptr) {
        *error = std::string(AAudio_convertResultToText(result)) +
                 " (does the app hold RECORD_AUDIO?)";
      }
      return false;
    }
    format_ = format;
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
      Close();
      if (error != nullptr) *error = "the capture stream would not start";
      return false;
    }
    return true;
  }

  bool Read(uint8_t* data, size_t size) override {
    if (stream_ == nullptr || data == nullptr) {
      return false;
    }
    const int32_t frame_bytes = format_.channels * (format_.bits / 8);
    int32_t frames = static_cast<int32_t>(size / frame_bytes);
    uint8_t* cursor = data;
    while (frames > 0) {
      const aaudio_result_t read = AAudioStream_read(stream_, cursor, frames, kReadTimeoutNanos);
      if (read <= 0) {
        return false;
      }
      frames -= read;
      cursor += static_cast<size_t>(read) * frame_bytes;
    }
    return true;
  }

  void Close() override {
    if (stream_ != nullptr) {
      AAudioStream_requestStop(stream_);
      AAudioStream_close(stream_);
      stream_ = nullptr;
    }
  }

  std::string backend_name() const override { return "AAudio"; }

 private:
  AAudioStream* stream_ = nullptr;
  PcmFormat format_;
};

class SilentSource : public PcmSource {
 public:
  bool Open(const PcmFormat& format, const std::string&, const std::string&,
            std::string*) override {
    bytes_per_second_ = format.bytes_per_second();
    return true;
  }

  bool Read(uint8_t* data, size_t size) override {
    if (data == nullptr || size == 0 || bytes_per_second_ <= 0) {
      return false;
    }
    // Slept for, not returned at once, for the same reason as on Linux: a caller reading
    // as fast as it can would flood the phone with silence.
    std::this_thread::sleep_for(std::chrono::microseconds(
        static_cast<int64_t>(size) * 1000000 / bytes_per_second_));
    std::memset(data, 0, size);
    return true;
  }

  void Close() override {}

  std::string backend_name() const override { return "none"; }

 private:
  int32_t bytes_per_second_ = 32000;
};

}  // namespace

std::unique_ptr<PcmSource> CreatePcmSource() { return std::make_unique<AAudioSource>(); }

std::unique_ptr<PcmSource> CreateSilentPcmSource() { return std::make_unique<SilentSource>(); }

std::vector<PcmDevice> ListPcmCaptureDevices() {
  PcmDevice device;
  device.description = "Android microphone";
  device.is_default = true;
  return {device};
}

}  // namespace aa
