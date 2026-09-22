// SPDX-License-Identifier: GPL-3.0-or-later
// H.264 to frames, on a thread of its own.
//
// The phone sends Annex-B H.264 over the video channel. This turns it into frames and
// hands them to the FrameRing, without ever naming a graphics API: the hardware path
// produces dmabuf file descriptors, which import into EGL today and Vulkan later.
//
// Two backends, picked at open time rather than configured:
//
//   VA-API    libavcodec with AV_HWDEVICE_TYPE_VAAPI. The surface never leaves the GPU;
//             it is exported as DRM prime and the present adapter imports it.
//   software  libavcodec on the CPU, converted to RGBA with libswscale. Correct
//             everywhere, and roughly ten times the cost per frame.
//
// The decoder runs on its own thread because decoding on an io_context thread would
// stall the transport, and the transport falling behind is what makes a phone drop
// the session.

#ifndef ANDROID_AUTO_LINUX_VIDEO_VIDEO_DECODER_H_
#define ANDROID_AUTO_LINUX_VIDEO_VIDEO_DECODER_H_

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

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace aa {

class VideoDecoder {
 public:
  // Hands a decoded frame on. Called on the decoder thread, never on an io_context
  // thread. `keepalive` owns whatever the frame's pointers or file descriptors refer to.
  using FramePublisher = std::function<void(const Frame&, std::shared_ptr<void>)>;
  // Human readable progress and problems, for the Dart event stream.
  using LogHandler = std::function<void(const std::string&)>;

  VideoDecoder(FramePublisher publish, LogHandler log);
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Whether the present adapter can import a dmabuf. Consulted every time the decoder
  // opens, and again whenever it changes, so a machine whose EGL will not take a
  // dmabuf quietly ends up on software decode instead of showing nothing.
  void SetDmabufProbe(std::function<bool()> probe);

  // Starts the decoder thread. Idempotent.
  void Start();

  // Stops the thread and releases the codec. Idempotent, and safe to call from the
  // platform thread: it never waits on anything but the decoder's own loop.
  void Stop();

  // The phone's SPS/PPS, which arrive out of band before the first frame. Kept so the
  // codec can be reopened mid stream without waiting for the phone to send them again.
  void SubmitCodecConfig(const uint8_t* data, size_t size);

  // One access unit of Annex-B H.264. Copied, so the aasdk buffer can go away the
  // moment this returns. `received_us` is a steady clock reading taken when the bytes
  // came off the wire, and is what the end to end latency figure is measured from.
  void Submit(const uint8_t* data, size_t size, int64_t received_us);

  // Throws away everything queued and resets the codec, so the next keyframe starts
  // clean. Called when the phone stops the stream.
  void Flush();

  // The black the phone was asked to leave round a `frame_width` by `frame_height`
  // frame, cropped off every frame of exactly that size from now on. A frame of any
  // other size is shown whole: the margins were worked out against the size advertised,
  // and a phone sending something else has not laid its interface out inside them. Safe
  // from any thread.
  void SetMargins(int32_t frame_width, int32_t frame_height, VideoMargins margins);

  // "VA-API" or "software", or "none" before the first open.
  std::string backend_name() const;

  // The size of what is shown, which is the decoded frame less its margins.
  int32_t frame_width() const { return frame_width_.load(); }
  int32_t frame_height() const { return frame_height_.load(); }
  uint64_t frames_decoded() const { return frames_decoded_.load(); }

 private:
  struct Packet {
    std::vector<uint8_t> bytes;
    int64_t received_us = 0;
  };

  static void EnableLibavLogging();
  void Run();
  bool Open(bool hardware);
  void Close();
  void DecodePacket(const Packet& packet);
  void DrainFrames();
  // The margins to crop off a frame of this size. Decoder thread.
  VideoMargins MarginsFor(int32_t width, int32_t height) const;
  void PublishHardware(AVFrame* frame, const VideoMargins& margins);
  void PublishSoftware(AVFrame* frame, const VideoMargins& margins);
  void NoteLatency(int64_t received_us);
  // Recycles one of a handful of RGBA buffers rather than allocating three megabytes
  // per frame. A buffer is free when the ring has let go of the frame that used it.
  std::shared_ptr<std::vector<uint8_t>> TakeRgbaBuffer(size_t bytes);
  void Log(const std::string& message);

  FramePublisher publish_;
  LogHandler log_;
  std::function<bool()> dmabuf_probe_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Packet> queue_;
  std::vector<uint8_t> codec_config_;
  bool flush_requested_ = false;
  // Set when the queue overflowed. Everything is discarded until a keyframe arrives,
  // because feeding a decoder half a GOP produces a screen of green blocks rather than
  // a dropped frame.
  bool awaiting_keyframe_ = false;
  // Said once, not once per codec config. See ConstrainBaselineSps.
  bool constrained_logged_ = false;
  // What SetMargins was last told.
  int32_t margins_frame_width_ = 0;
  int32_t margins_frame_height_ = 0;
  VideoMargins margins_;

  // Decoder thread only, past construction.
  AVCodecContext* codec_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  SwsContext* scaler_ = nullptr;
  int scaler_src_format_ = -1;
  int32_t scaler_width_ = 0;
  int32_t scaler_height_ = 0;
  bool hardware_ = false;
  bool open_ = false;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> rgba_pool_;

  std::atomic<int32_t> frame_width_{0};
  std::atomic<int32_t> frame_height_{0};
  std::atomic<uint64_t> frames_decoded_{0};
  std::atomic<bool> backend_hardware_{false};
  std::atomic<bool> ever_opened_{false};
  // Rolling average of wire to frame latency, reported every few seconds.
  double latency_sum_ms_ = 0.0;
  uint64_t latency_count_ = 0;
  int64_t last_latency_report_us_ = 0;
  uint64_t decode_errors_ = 0;
};

// Steady clock reading in microseconds, the one the latency figures are built on.
int64_t NowMicros();

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_VIDEO_VIDEO_DECODER_H_
