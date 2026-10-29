// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_decoder.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "annex_b.h"

namespace aa {
namespace {

constexpr const char* kMime = "video/avc";

// Same bound as on Linux: about two seconds of video, past which the decoder is
// hopelessly behind and the queue is thrown away up to the next keyframe.
constexpr size_t kMaxQueuedPackets = 60;

// The FrameRing keeps up to three pictures alive, one more is being published, and the
// codec needs at least one to decode into. Two spare keep it from ever stalling on us.
constexpr int32_t kReaderImages = 6;

constexpr int64_t kLatencyReportIntervalUs = 10 * 1000 * 1000;

// What the codec is opened at when nothing has said how big the stream is. MediaCodec
// follows the stream's own SPS whatever it was told, so this only sizes the first
// buffers.
constexpr int32_t kDefaultWidth = 1280;
constexpr int32_t kDefaultHeight = 720;

}  // namespace

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {

// What the reader's listener is handed. Weak, because the reader owns it: a callback
// that arrives as the last picture lets go finds nothing rather than a dangling reader.
struct ListenerContext {
  std::weak_ptr<VideoDecoder::Reader> reader;
};

}  // namespace

// The image reader, kept alive by every picture taken from it. A picture can outlive the
// decoder that made it, sitting in the FrameRing while the session is torn down, and an
// AImage must not outlive its reader. The listener goes through `owner` rather than
// straight to the decoder, so a picture that lands after the decoder has let go of the
// reader reaches nobody.
struct VideoDecoder::Reader {
  AImageReader* reader = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  std::mutex gate;
  VideoDecoder* owner = nullptr;
  AImageReader_ImageListener listener{};

  ~Reader() {
    if (reader != nullptr) {
      // Stops the callbacks, so the context can go after it.
      AImageReader_delete(reader);
    }
    delete static_cast<ListenerContext*>(listener.context);
  }

  void Disown() {
    std::lock_guard<std::mutex> lock(gate);
    owner = nullptr;
  }
};

namespace {

// What a published frame holds on to: the picture, and the reader it came from.
struct HeldImage {
  std::shared_ptr<VideoDecoder::Reader> reader;
  AImage* image = nullptr;
  ~HeldImage() {
    if (image != nullptr) {
      AImage_delete(image);
    }
  }
};

}  // namespace

VideoDecoder::VideoDecoder(FramePublisher publish, LogHandler log, LiveHandler on_live)
    : publish_(std::move(publish)), log_(std::move(log)), on_live_(std::move(on_live)) {}

VideoDecoder::~VideoDecoder() {
  Stop();
  if (reader_) {
    reader_->Disown();
  }
}

void VideoDecoder::SetDmabufProbe(std::function<bool()>) {}

void VideoDecoder::Start() {
  if (running_.exchange(true)) {
    return;
  }
  thread_ = std::thread([this]() { Run(); });
}

void VideoDecoder::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }
  EndPicture();
}

void VideoDecoder::SubmitCodecConfig(const uint8_t* data, size_t size) {
  Enqueue(data, size, NowMicros(), true);
}

void VideoDecoder::Submit(const uint8_t* data, size_t size, int64_t received_us) {
  Enqueue(data, size, received_us, false);
}

void VideoDecoder::Enqueue(const uint8_t* data, size_t size, int64_t received_us,
                           bool config) {
  if (size == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (awaiting_keyframe_ && !config) {
      if (!ContainsKeyframe(data, size)) {
        return;
      }
      awaiting_keyframe_ = false;
    }
    if (queue_.size() >= kMaxQueuedPackets) {
      queue_.clear();
      awaiting_keyframe_ = true;
      return;
    }
    Packet packet;
    packet.bytes.assign(data, data + size);
    packet.received_us = received_us;
    packet.config = config;
    queue_.push_back(std::move(packet));
  }
  cv_.notify_one();
}

void VideoDecoder::Flush(bool keep_picture) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    flush_requested_ = true;
    awaiting_keyframe_ = true;
    generation_.fetch_add(1);
  }
  cv_.notify_one();
  if (!keep_picture) {
    EndPicture();
  }
}

void VideoDecoder::EndPicture() {
  bool was_live = false;
  {
    std::lock_guard<std::mutex> lock(picture_mutex_);
    was_live = live_.exchange(false);
    frame_width_.store(0);
    frame_height_.store(0);
  }
  if (was_live && on_live_) {
    on_live_(false);
  }
}

void VideoDecoder::SetMargins(int32_t frame_width, int32_t frame_height,
                              VideoMargins margins) {
  std::lock_guard<std::mutex> lock(mutex_);
  margins_frame_width_ = frame_width;
  margins_frame_height_ = frame_height;
  margins_ = margins;
}

VideoMargins VideoDecoder::MarginsFor(int32_t width, int32_t height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (width != margins_frame_width_ || height != margins_frame_height_) {
    return VideoMargins{};
  }
  return margins_;
}

std::string VideoDecoder::backend_name() const {
  std::lock_guard<std::mutex> lock(name_mutex_);
  return codec_name_.empty() ? std::string("none") : "MediaCodec " + codec_name_;
}

void VideoDecoder::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

void VideoDecoder::Run() {
  while (running_.load()) {
    Packet packet;
    bool have_packet = false;
    bool flush = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto ready = [this]() {
        return !running_.load() || flush_requested_ || !queue_.empty();
      };
      if (codec_ != nullptr) {
        // An open codec can have pictures to hand on with nothing new to feed it, so
        // the wait is short rather than indefinite.
        cv_.wait_for(lock, std::chrono::milliseconds(5), ready);
      } else {
        cv_.wait(lock, ready);
      }
      if (!running_.load()) {
        break;
      }
      if (flush_requested_) {
        flush_requested_ = false;
        flush = true;
      } else if (!queue_.empty()) {
        packet = std::move(queue_.front());
        queue_.pop_front();
        have_packet = true;
      }
    }

    if (flush) {
      if (codec_ != nullptr) {
        AMediaCodec_flush(codec_);
      }
      codec_generation_.store(generation_.load());
      continue;
    }
    if (have_packet) {
      if (codec_ == nullptr && !Open()) {
        continue;
      }
      Feed(packet);
    }
    if (codec_ != nullptr) {
      DrainOutput(0);
    }
  }
  Close();
}

bool VideoDecoder::Open() {
  int32_t width = kDefaultWidth;
  int32_t height = kDefaultHeight;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (margins_frame_width_ > 0 && margins_frame_height_ > 0) {
      width = margins_frame_width_;
      height = margins_frame_height_;
    }
  }

  if (!reader_ || reader_->width != width || reader_->height != height) {
    if (reader_) {
      reader_->Disown();
    }
    auto reader = std::make_shared<Reader>();
    reader->width = width;
    reader->height = height;
    const media_status_t status = AImageReader_newWithUsage(
        width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        kReaderImages, &reader->reader);
    if (status != AMEDIA_OK || reader->reader == nullptr) {
      Log("Could not create an image reader for decoded video (" +
          std::to_string(status) + ").");
      reader->reader = nullptr;
      return false;
    }
    reader->owner = this;
    reader->listener.context = new ListenerContext{reader};
    reader->listener.onImageAvailable = [](void* context, AImageReader*) {
      auto held = static_cast<ListenerContext*>(context)->reader.lock();
      if (!held) {
        return;
      }
      std::lock_guard<std::mutex> lock(held->gate);
      if (held->owner != nullptr) {
        held->owner->OnImage(held);
      }
    };
    AImageReader_setImageListener(reader->reader, &reader->listener);
    reader_ = std::move(reader);
  }

  ANativeWindow* window = nullptr;
  if (AImageReader_getWindow(reader_->reader, &window) != AMEDIA_OK || window == nullptr) {
    Log("The image reader for decoded video has no window.");
    return false;
  }

  codec_ = AMediaCodec_createDecoderByType(kMime);
  if (codec_ == nullptr) {
    Log("This device has no H.264 decoder.");
    return false;
  }
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, kMime);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
  // Projection is interactive: every frame the decoder holds back to reorder is a frame
  // of lag between a finger and the screen. Both keys are hints a decoder may ignore.
  AMediaFormat_setInt32(format, "low-latency", 1);
  AMediaFormat_setInt32(format, "priority", 0);
  const media_status_t configured = AMediaCodec_configure(codec_, format, window, nullptr, 0);
  AMediaFormat_delete(format);
  if (configured != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
    Log("The H.264 decoder would not start (" + std::to_string(configured) + ").");
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }
  codec_width_ = width;
  codec_height_ = height;
  codec_generation_.store(generation_.load());

  // c2.exynos.h264.decoder, say, which is the only way to tell a hardware decoder from
  // Google's software one.
  std::string name = "decoder";
  char* raw = nullptr;
  if (AMediaCodec_getName(codec_, &raw) == AMEDIA_OK && raw != nullptr) {
    name = raw;
    AMediaCodec_releaseName(codec_, raw);
  }
  {
    std::lock_guard<std::mutex> lock(name_mutex_);
    codec_name_ = name;
  }
  Log("Opened " + name + " for " + std::to_string(width) + "x" + std::to_string(height) +
      " H.264.");
  return true;
}

void VideoDecoder::Close() {
  if (codec_ != nullptr) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
}

void VideoDecoder::Feed(const Packet& packet) {
  // The codec may have every input buffer full of frames it has not finished. Taking
  // its output is what frees them, so the two alternate until there is room.
  ssize_t index = -1;
  for (int attempt = 0; attempt < 50 && running_.load(); ++attempt) {
    index = AMediaCodec_dequeueInputBuffer(codec_, 2000);
    if (index >= 0) {
      break;
    }
    DrainOutput(0);
  }
  if (index < 0) {
    Log("The H.264 decoder has no room, dropping a frame.");
    return;
  }
  size_t capacity = 0;
  uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(index), &capacity);
  if (buffer == nullptr || capacity < packet.bytes.size()) {
    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, 0, 0, 0);
    Log("A video frame does not fit the decoder's buffer, dropping it.");
    return;
  }
  std::memcpy(buffer, packet.bytes.data(), packet.bytes.size());
  const uint32_t flags = packet.config ? AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG : 0;
  AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, packet.bytes.size(),
                               static_cast<uint64_t>(packet.received_us), flags);
}

void VideoDecoder::DrainOutput(int64_t timeout_us) {
  for (;;) {
    AMediaCodecBufferInfo info{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, timeout_us);
    timeout_us = 0;
    if (index >= 0) {
      // Rendering is what sends the picture to the image reader, and a picture of a
      // stream that has since been flushed must not get there.
      const bool current = codec_generation_.load() == generation_.load();
      AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(index),
                                      current && info.size > 0);
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
      int32_t width = 0;
      int32_t height = 0;
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
      AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
      AMediaFormat_delete(format);
      Log("The H.264 decoder is producing " + std::to_string(width) + "x" +
          std::to_string(height) + ".");
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    return;
  }
}

void VideoDecoder::OnImage(const std::shared_ptr<Reader>& reader) {
  AImage* image = nullptr;
  if (AImageReader_acquireNextImage(reader->reader, &image) != AMEDIA_OK ||
      image == nullptr) {
    return;
  }
  auto held = std::make_shared<HeldImage>();
  held->reader = reader;
  held->image = image;

  AHardwareBuffer* buffer = nullptr;
  if (AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK || buffer == nullptr) {
    return;
  }
  AHardwareBuffer_Desc desc{};
  AHardwareBuffer_describe(buffer, &desc);
  AImageCropRect crop{0, 0, static_cast<int32_t>(desc.width),
                      static_cast<int32_t>(desc.height)};
  AImage_getCropRect(image, &crop);
  if (crop.right <= crop.left || crop.bottom <= crop.top) {
    crop = {0, 0, static_cast<int32_t>(desc.width), static_cast<int32_t>(desc.height)};
  }
  int64_t timestamp_ns = 0;
  AImage_getTimestamp(image, &timestamp_ns);

  // The picture the phone encoded is the crop rectangle, the rest is the decoder's own
  // padding. Margins are worked out against the picture, then both are cut off.
  const int32_t picture_width = crop.right - crop.left;
  const int32_t picture_height = crop.bottom - crop.top;
  const VideoMargins margins = MarginsFor(picture_width, picture_height);

  Frame frame;
  frame.kind = FrameKind::kHardwareBuffer;
  frame.width = static_cast<int32_t>(desc.width);
  frame.height = static_cast<int32_t>(desc.height);
  frame.crop_left = crop.left + margins.left;
  frame.crop_top = crop.top + margins.top;
  frame.crop_right = (frame.width - crop.right) + margins.right;
  frame.crop_bottom = (frame.height - crop.bottom) + margins.bottom;
  frame.hardware_buffer = buffer;
  const int32_t visible_width = frame.visible_width();
  const int32_t visible_height = frame.visible_height();

  bool published = false;
  bool resized = false;
  bool became_live = false;
  {
    std::lock_guard<std::mutex> lock(picture_mutex_);
    if (codec_generation_.load() == generation_.load()) {
      publish_(frame, held);
      published = true;
      const int32_t previous_width = frame_width_.exchange(visible_width);
      const int32_t previous_height = frame_height_.exchange(visible_height);
      resized = previous_width != visible_width || previous_height != visible_height;
      became_live = !live_.exchange(true);
    }
  }
  if (resized) {
    const std::string size =
        margins.empty()
            ? std::to_string(picture_width) + "x" + std::to_string(picture_height)
            : DescribeMargins(picture_width, picture_height, margins);
    Log("Video: " + size + ", decoded by " + backend_name() + ".");
  }
  if (became_live && on_live_) {
    on_live_(true);
  }
  if (published) {
    frames_decoded_.fetch_add(1);
    NoteLatency(timestamp_ns / 1000);
  }
}

void VideoDecoder::NoteLatency(int64_t received_us) {
  if (received_us <= 0) {
    return;
  }
  const int64_t now = NowMicros();
  latency_sum_ms_ += static_cast<double>(now - received_us) / 1000.0;
  ++latency_count_;
  if (last_latency_report_us_ == 0) {
    last_latency_report_us_ = now;
    return;
  }
  if (now - last_latency_report_us_ < kLatencyReportIntervalUs) {
    return;
  }
  const double average = latency_sum_ms_ / static_cast<double>(latency_count_);
  char text[200];
  std::snprintf(text, sizeof(text), "Video: %dx%d, %s, %llu frames, %.1f ms from wire to frame.",
                frame_width_.load(), frame_height_.load(), backend_name().c_str(),
                static_cast<unsigned long long>(frames_decoded_.load()), average);
  Log(text);
  latency_sum_ms_ = 0.0;
  latency_count_ = 0;
  last_latency_report_us_ = now;
}

}  // namespace aa
