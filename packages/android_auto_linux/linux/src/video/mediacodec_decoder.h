// SPDX-License-Identifier: GPL-3.0-or-later
// H.264 to frames with MediaCodec. The Android VideoDecoder.
//
// Same interface as libav_decoder.h, so the channels and the session cannot tell which
// one they have. Underneath it is shaped differently: MediaCodec decodes into the
// window of an AImageReader, and each picture comes out as an AHardwareBuffer that is
// handed to the FrameRing by reference, exactly as a VA-API surface is on Linux. Nothing
// is copied and nothing leaves the GPU.
//
// Two threads are involved rather than one. The decoder thread feeds MediaCodec and
// releases its output into the reader; the reader's own thread is told when a picture
// has landed and is the one that publishes it. Neither is an io_context thread.
//
// Android's decoders take the phone's Baseline profile as it is, so there is no SPS
// rewrite here, unlike on Linux.

#ifndef ANDROID_AUTO_LINUX_VIDEO_MEDIACODEC_DECODER_H_
#define ANDROID_AUTO_LINUX_VIDEO_MEDIACODEC_DECODER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../frame_ring.h"
#include "video_margins.h"

struct AMediaCodec;
struct AImageReader;

namespace aa {

class VideoDecoder {
 public:
  // The same three as on Linux, see libav_decoder.h. The publisher is called on the
  // image reader's thread.
  using FramePublisher = std::function<void(const Frame&, std::shared_ptr<void>)>;
  using LogHandler = std::function<void(const std::string&)>;
  using LiveHandler = std::function<void(bool live)>;

  VideoDecoder(FramePublisher publish, LogHandler log, LiveHandler on_live);
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Accepted for the sake of one interface and otherwise ignored: there is no software
  // fallback to choose, every Android decoder writes hardware buffers.
  void SetDmabufProbe(std::function<bool()> probe);

  void Start();
  void Stop();
  void SubmitCodecConfig(const uint8_t* data, size_t size);
  void Submit(const uint8_t* data, size_t size, int64_t received_us);
  void Flush(bool keep_picture = false);
  void SetMargins(int32_t frame_width, int32_t frame_height, VideoMargins margins);

  // "MediaCodec" and the decoder's own name, or "none" before the first open.
  std::string backend_name() const;

  bool live() const { return live_.load(); }
  int32_t frame_width() const { return frame_width_.load(); }
  int32_t frame_height() const { return frame_height_.load(); }
  uint64_t frames_decoded() const { return frames_decoded_.load(); }

  // Holds the image reader, and the gate its listener goes through. Public only so the
  // keepalive in mediacodec_decoder.cc can name it.
  struct Reader;

 private:
  struct Packet {
    std::vector<uint8_t> bytes;
    int64_t received_us = 0;
    bool config = false;
  };

  void Enqueue(const uint8_t* data, size_t size, int64_t received_us, bool config);
  void Run();
  bool Open();
  void Close();
  void Feed(const Packet& packet);
  void DrainOutput(int64_t timeout_us);
  void EndPicture();
  VideoMargins MarginsFor(int32_t width, int32_t height) const;
  // Reader thread.
  void OnImage(const std::shared_ptr<Reader>& reader);
  void NoteLatency(int64_t received_us);
  void Log(const std::string& message);

  FramePublisher publish_;
  LogHandler log_;
  LiveHandler on_live_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Packet> queue_;
  bool flush_requested_ = false;
  // Bumped by every flush. A picture is only published while the codec is on the
  // generation that is current, so nothing of a flushed stream can come back.
  std::atomic<uint64_t> generation_{0};
  std::atomic<uint64_t> codec_generation_{0};
  bool awaiting_keyframe_ = false;
  int32_t margins_frame_width_ = 0;
  int32_t margins_frame_height_ = 0;
  VideoMargins margins_;

  // Decoder thread only, past construction.
  AMediaCodec* codec_ = nullptr;
  std::shared_ptr<Reader> reader_;
  int32_t codec_width_ = 0;
  int32_t codec_height_ = 0;

  std::mutex picture_mutex_;
  std::atomic<bool> live_{false};
  std::atomic<int32_t> frame_width_{0};
  std::atomic<int32_t> frame_height_{0};
  std::atomic<uint64_t> frames_decoded_{0};
  mutable std::mutex name_mutex_;
  std::string codec_name_;
  // Reader thread only.
  double latency_sum_ms_ = 0.0;
  uint64_t latency_count_ = 0;
  int64_t last_latency_report_us_ = 0;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_VIDEO_MEDIACODEC_DECODER_H_
