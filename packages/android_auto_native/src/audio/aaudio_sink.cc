// SPDX-License-Identifier: GPL-3.0-or-later
// PcmSink on Android, through AAudio. The only file that names AAudio for playback.
//
// AAudio in blocking mode is the same contract libpulse-simple gives on Linux: Write
// waits until the stream has room, which is what paces the head unit to real time, and
// it runs on AudioOutput's writer thread, never an io thread.
//
// Each of the three streams says what it is through its usage, so Android's own audio
// policy treats them as a car would want: navigation prompts duck other apps' music,
// and the system sounds follow the device's notification volume. The ducking of the
// phone's own media under its own speech still happens in AudioOutput, for the reason
// given there.

#include <aaudio/AAudio.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "pcm_sink.h"

namespace aa {
namespace {

// Long enough for a writer that has fallen behind by a buffer or two, short enough that
// a stream that has died is noticed within a second.
constexpr int64_t kWriteTimeoutNanos = 500 * 1000 * 1000;

aaudio_usage_t UsageFor(const std::string& label) {
  if (label.find("speech") != std::string::npos) {
    return AAUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE;
  }
  if (label.find("system") != std::string::npos) {
    return AAUDIO_USAGE_ASSISTANCE_SONIFICATION;
  }
  return AAUDIO_USAGE_MEDIA;
}

aaudio_content_type_t ContentFor(const std::string& label) {
  if (label.find("speech") != std::string::npos) {
    return AAUDIO_CONTENT_TYPE_SPEECH;
  }
  if (label.find("system") != std::string::npos) {
    return AAUDIO_CONTENT_TYPE_SONIFICATION;
  }
  return AAUDIO_CONTENT_TYPE_MUSIC;
}

class AAudioSink : public PcmSink {
 public:
  ~AAudioSink() override { Close(); }

  bool Open(const PcmFormat& format, const std::string&, const std::string& label,
            std::string* error) override {
    Close();
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
      if (error != nullptr) *error = AAudio_convertResultToText(result);
      return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, format.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, format.channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    // Speech and system sounds want to be heard the moment they arrive; music is
    // happier with the larger buffers that let the device sleep between them.
    AAudioStreamBuilder_setPerformanceMode(builder, UsageFor(label) == AAUDIO_USAGE_MEDIA
                                                        ? AAUDIO_PERFORMANCE_MODE_NONE
                                                        : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, UsageFor(label));
    AAudioStreamBuilder_setContentType(builder, ContentFor(label));
    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
      stream_ = nullptr;
      if (error != nullptr) *error = AAudio_convertResultToText(result);
      return false;
    }
    format_ = format;
    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
      if (error != nullptr) *error = AAudio_convertResultToText(result);
      Close();
      return false;
    }
    return true;
  }

  bool Write(const uint8_t* data, size_t size) override {
    if (stream_ == nullptr) {
      return false;
    }
    const int32_t frame_bytes = format_.channels * (format_.bits / 8);
    int32_t frames = static_cast<int32_t>(size / frame_bytes);
    const uint8_t* cursor = data;
    while (frames > 0) {
      const aaudio_result_t written =
          AAudioStream_write(stream_, cursor, frames, kWriteTimeoutNanos);
      if (written < 0) {
        return false;
      }
      if (written == 0) {
        // Timed out with no room at all: the stream is stuck, which the caller treats
        // the same as broken.
        return false;
      }
      frames -= written;
      cursor += static_cast<size_t>(written) * frame_bytes;
    }
    return true;
  }

  void Flush() override {
    if (stream_ == nullptr) {
      return;
    }
    // AAudio only flushes a paused stream.
    AAudioStream_requestPause(stream_);
    WaitForState(AAUDIO_STREAM_STATE_PAUSING, AAUDIO_STREAM_STATE_PAUSED);
    AAudioStream_requestFlush(stream_);
    WaitForState(AAUDIO_STREAM_STATE_FLUSHING, AAUDIO_STREAM_STATE_FLUSHED);
    AAudioStream_requestStart(stream_);
  }

  void Drain() override {
    if (stream_ == nullptr) {
      return;
    }
    // Until the device has read everything written, bounded at two seconds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      const int64_t pending =
          AAudioStream_getFramesWritten(stream_) - AAudioStream_getFramesRead(stream_);
      if (pending <= 0) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void Close() override {
    if (stream_ != nullptr) {
      AAudioStream_requestStop(stream_);
      AAudioStream_close(stream_);
      stream_ = nullptr;
    }
  }

  int64_t LatencyMicros() override {
    if (stream_ == nullptr || format_.sample_rate <= 0) {
      return 0;
    }
    const int64_t pending =
        AAudioStream_getFramesWritten(stream_) - AAudioStream_getFramesRead(stream_);
    return pending <= 0 ? 0 : pending * 1000000 / format_.sample_rate;
  }

  std::string backend_name() const override { return "AAudio"; }

 private:
  void WaitForState(aaudio_stream_state_t transient, aaudio_stream_state_t) {
    aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
    AAudioStream_waitForStateChange(stream_, transient, &next, 200 * 1000 * 1000);
  }

  AAudioStream* stream_ = nullptr;
  PcmFormat format_;
};

class NullSink : public PcmSink {
 public:
  bool Open(const PcmFormat&, const std::string&, const std::string&,
            std::string*) override {
    return true;
  }
  bool Write(const uint8_t*, size_t) override { return true; }
  void Flush() override {}
  void Drain() override {}
  void Close() override {}
  int64_t LatencyMicros() override { return 0; }
  std::string backend_name() const override { return "none"; }
};

}  // namespace

std::unique_ptr<PcmSink> CreatePcmSink() { return std::make_unique<AAudioSink>(); }

std::unique_ptr<PcmSink> CreateNullPcmSink() { return std::make_unique<NullSink>(); }

std::vector<PcmDevice> ListPcmDevices() {
  // Android routes by usage, not by device: the system decides between the speaker, a
  // wired headset and Bluetooth. So there is one device and it is whatever Android says.
  PcmDevice device;
  device.description = "Android audio";
  device.is_default = true;
  return {device};
}

}  // namespace aa
