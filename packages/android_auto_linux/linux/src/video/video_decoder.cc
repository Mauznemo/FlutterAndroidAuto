// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_decoder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <drm/drm_fourcc.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace aa {
namespace {

// How many access units may pile up before the decoder gives up on catching up. At
// 30 fps this is two seconds of video, which is far longer than any hiccup that is
// worth recovering from frame by frame.
constexpr size_t kMaxQueuedPackets = 60;

// How many RGBA buffers the software path keeps around. Three are in the ring at most,
// so four means a spare is always free and the pool never grows.
constexpr size_t kRgbaPoolSize = 4;

constexpr int64_t kLatencyReportIntervalUs = 10 * 1000 * 1000;

// Reads the NAL unit types out of an Annex-B buffer and says whether it contains an
// IDR picture. Used only to decide where to resume after the queue overflowed, so a
// cheap scan for start codes is enough; it does not have to be a real parser.
bool ContainsKeyframe(const uint8_t* data, size_t size) {
  size_t zeros = 0;
  for (size_t i = 0; i < size; ++i) {
    const uint8_t byte = data[i];
    if (byte == 0x00) {
      ++zeros;
      continue;
    }
    if (byte == 0x01 && zeros >= 2 && i + 1 < size) {
      const uint8_t nal_type = data[i + 1] & 0x1f;
      // 5 is a coded slice of an IDR picture, 7 and 8 are the SPS and PPS that always
      // precede one in this stream.
      if (nal_type == 5 || nal_type == 7) {
        return true;
      }
    }
    zeros = 0;
  }
  return false;
}

// Picks VA-API when libavcodec offers it, and whatever it offers otherwise.
//
// The fallback is the whole point. libavcodec calls this again with VA-API removed from
// the list when setting the hardware decoder up fails, which it does for a stream whose
// profile the driver will not take. Returning AV_PIX_FMT_NONE from that second call
// leaves the decoder with no output format at all, and every packet after it comes back
// as AVERROR_INVALIDDATA: a driver limitation that reads exactly like corrupt video.
// Marks a Baseline SPS as Constrained Baseline, in place. Returns true if it changed
// anything.
//
// Android Auto advertises and encodes H.264 Baseline profile, profile_idc 66 with no
// constraint flags set. No GPU implements that: Baseline allows arbitrary slice
// ordering, flexible macroblock ordering and redundant slices, features no encoder has
// emitted this century, and drivers expose only the Constrained Baseline subset that
// leaves them out. libavcodec used to paper over the difference and stopped, so the
// stream now fails hardware setup outright.
//
// Setting constraint_set1_flag says "this really is the constrained subset". For a
// stream from Android's MediaCodec encoder that is true. If a phone ever did send the
// Baseline-only tools the hardware decoder would produce a mess rather than fall back,
// so AA_VIDEO_DECODER=software is the way out.
bool ConstrainBaselineSps(uint8_t* data, size_t size) {
  bool changed = false;
  size_t zeros = 0;
  for (size_t i = 0; i < size; ++i) {
    if (data[i] == 0x00) {
      ++zeros;
      continue;
    }
    // A start code, and enough of a NAL after it to hold profile, constraints and level.
    if (data[i] == 0x01 && zeros >= 2 && i + 3 < size) {
      uint8_t* nal = data + i + 1;
      const bool is_sps = (nal[0] & 0x1f) == 7;
      const bool is_baseline = nal[1] == 66;
      if (is_sps && is_baseline && (nal[2] & 0x40) == 0) {
        nal[2] |= 0x40;
        changed = true;
      }
    }
    zeros = 0;
  }
  return changed;
}

AVPixelFormat ChooseFormat(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    if (*format == AV_PIX_FMT_VAAPI) {
      return *format;
    }
  }
  return formats[0];
}

ColorSpace ColorSpaceOf(const AVFrame* frame) {
  switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
      return ColorSpace::kBt709;
    default:
      // Android Auto encodes at 720p and below and tags BT.601, and an untagged stream
      // at those sizes is BT.601 by convention too.
      return ColorSpace::kBt601;
  }
}

void ApplyCrop(const VideoMargins& margins, Frame* frame) {
  frame->crop_top = margins.top;
  frame->crop_bottom = margins.bottom;
  frame->crop_left = margins.left;
  frame->crop_right = margins.right;
}

}  // namespace

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

VideoDecoder::VideoDecoder(FramePublisher publish, LogHandler log)
    : publish_(std::move(publish)), log_(std::move(log)) {}

VideoDecoder::~VideoDecoder() { Stop(); }

// Sends libavcodec's own diagnostics through the same log as everything else.
//
// It is the only thing that will say why a hardware decoder refused a stream, and that
// answer is otherwise invisible: the failure arrives as a generic invalid data error on
// every packet. Off unless AA_LOG_LEVEL asks for DEBUG, because libavcodec at verbose
// is several lines per frame.
void VideoDecoder::EnableLibavLogging() {
  const char* level = std::getenv("AA_LOG_LEVEL");
  if (level == nullptr) {
    return;
  }
  const std::string wanted(level);
  if (wanted == "DEBUG" || wanted == "debug" || wanted == "TRACE" || wanted == "trace") {
    av_log_set_level(AV_LOG_VERBOSE);
  }
}

void VideoDecoder::SetDmabufProbe(std::function<bool()> probe) {
  std::lock_guard<std::mutex> lock(mutex_);
  dmabuf_probe_ = std::move(probe);
}

void VideoDecoder::Start() {
  if (running_.exchange(true)) {
    return;
  }
  EnableLibavLogging();
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
}

void VideoDecoder::SubmitCodecConfig(const uint8_t* data, size_t size) {
  std::vector<uint8_t> config(data, data + size);
  if (ConstrainBaselineSps(config.data(), config.size()) && !constrained_logged_) {
    constrained_logged_ = true;
    Log("The phone's H.264 is Baseline profile, which no GPU decodes. Marking it as "
        "Constrained Baseline so the hardware decoder will take it.");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    codec_config_ = config;
  }
  // Also feed it through as an ordinary packet. In an Annex-B stream the SPS and PPS
  // are just NAL units, and the decoder is happy to see them again, and it has to be
  // the rewritten copy: an in band SPS that still says plain Baseline would undo the
  // rewrite the moment the decoder reconfigures.
  Submit(config.data(), config.size(), NowMicros());
}

void VideoDecoder::Submit(const uint8_t* data, size_t size, int64_t received_us) {
  if (size == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (awaiting_keyframe_) {
      if (!ContainsKeyframe(data, size)) {
        return;
      }
      awaiting_keyframe_ = false;
    }
    if (queue_.size() >= kMaxQueuedPackets) {
      // The decoder is hopelessly behind. Dropping individual packets from the middle
      // of a GOP corrupts everything that references them, so throw the lot away and
      // pick up at the next keyframe instead.
      queue_.clear();
      awaiting_keyframe_ = true;
      return;
    }
    Packet packet;
    packet.bytes.assign(data, data + size);
    packet.received_us = received_us;
    queue_.push_back(std::move(packet));
  }
  cv_.notify_one();
}

void VideoDecoder::Flush() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    flush_requested_ = true;
    awaiting_keyframe_ = true;
  }
  cv_.notify_one();
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
  if (!ever_opened_.load()) {
    return "none";
  }
  return backend_hardware_.load() ? "VA-API" : "software";
}

void VideoDecoder::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

void VideoDecoder::Run() {
  while (running_.load()) {
    Packet packet;
    bool flush = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() {
        return !running_.load() || flush_requested_ || !queue_.empty();
      });
      if (!running_.load()) {
        break;
      }
      if (flush_requested_) {
        flush_requested_ = false;
        flush = true;
      } else {
        packet = std::move(queue_.front());
        queue_.pop_front();
      }
    }

    if (flush) {
      if (open_ && codec_ != nullptr) {
        avcodec_flush_buffers(codec_);
      }
      continue;
    }

    // Opening is deferred to the first packet so that the dmabuf probe has had a chance
    // to run: the present adapter can only answer once Flutter has called populate at
    // least once, which happens as soon as the texture is on screen.
    const bool want_hardware = [this]() {
      // AA_VIDEO_DECODER=software forces the fallback path, which is the only way to
      // tell a driver problem from a decoder problem without a rebuild.
      const char* forced = std::getenv("AA_VIDEO_DECODER");
      if (forced != nullptr && std::string(forced) == "software") {
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      return !dmabuf_probe_ || dmabuf_probe_();
    }();

    if (open_ && hardware_ && !want_hardware) {
      Log("The present adapter cannot import a dmabuf, switching to software decode.");
      Close();
    }
    if (!open_) {
      if (!Open(want_hardware)) {
        // Nothing can be done with this packet. Wait for the next keyframe rather than
        // hammering avcodec_open2 once per frame.
        std::lock_guard<std::mutex> lock(mutex_);
        awaiting_keyframe_ = true;
        continue;
      }
    }
    DecodePacket(packet);
  }
  Close();
}

bool VideoDecoder::Open(bool hardware) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (codec == nullptr) {
    Log("This build of libavcodec has no H.264 decoder, so there is nothing to show.");
    return false;
  }

  codec_ = avcodec_alloc_context3(codec);
  if (codec_ == nullptr) {
    Log("Out of memory allocating the H.264 decoder.");
    return false;
  }

  // The phone sends one access unit per message and expects it back as soon as
  // possible. Frame threading would buy throughput we do not need at the cost of
  // several frames of latency, which is the one thing that is actually scarce here.
  codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codec_->flags2 |= AV_CODEC_FLAG2_FAST;
  codec_->thread_type = FF_THREAD_SLICE;
  codec_->thread_count = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!codec_config_.empty()) {
      // Annex-B SPS/PPS. libavcodec detects the framing, so this is the same bytes the
      // phone sent, not a repackaged avcC record.
      codec_->extradata = static_cast<uint8_t*>(
          av_mallocz(codec_config_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (codec_->extradata != nullptr) {
        std::memcpy(codec_->extradata, codec_config_.data(), codec_config_.size());
        codec_->extradata_size = static_cast<int>(codec_config_.size());
      }
    }
  }

  hardware_ = false;
  if (hardware) {
    const int result =
        av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (result < 0) {
      Log("No VA-API device, falling back to software decode.");
      hw_device_ = nullptr;
    } else {
      codec_->hw_device_ctx = av_buffer_ref(hw_device_);
      codec_->get_format = ChooseFormat;
      hardware_ = true;
    }
  }

  if (avcodec_open2(codec_, codec, nullptr) < 0) {
    if (hardware_) {
      Log("The VA-API H.264 decoder would not open, falling back to software decode.");
      Close();
      return Open(false);
    }
    Log("The H.264 decoder would not open.");
    Close();
    return false;
  }

  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (frame_ == nullptr || packet_ == nullptr) {
    Log("Out of memory setting up the H.264 decoder.");
    Close();
    return false;
  }

  open_ = true;
  backend_hardware_.store(hardware_);
  ever_opened_.store(true);
  Log(std::string("Opening the H.264 decoder on the ") +
      (hardware_ ? "VA-API" : "software") + " backend.");
  return true;
}

void VideoDecoder::Close() {
  if (scaler_ != nullptr) {
    sws_freeContext(scaler_);
    scaler_ = nullptr;
    scaler_src_format_ = -1;
    scaler_width_ = 0;
    scaler_height_ = 0;
  }
  if (packet_ != nullptr) {
    av_packet_free(&packet_);
  }
  if (frame_ != nullptr) {
    av_frame_free(&frame_);
  }
  if (codec_ != nullptr) {
    // avcodec_free_context releases extradata and the hw_device_ctx reference it took.
    avcodec_free_context(&codec_);
  }
  if (hw_device_ != nullptr) {
    av_buffer_unref(&hw_device_);
    hw_device_ = nullptr;
  }
  open_ = false;
  hardware_ = false;
}

void VideoDecoder::DecodePacket(const Packet& packet) {
  av_packet_unref(packet_);
  // A copy the decoder owns, rather than pointing into the queue entry, because
  // libavcodec may hold a packet's data past the send call.
  if (av_new_packet(packet_, static_cast<int>(packet.bytes.size())) < 0) {
    return;
  }
  std::memcpy(packet_->data, packet.bytes.data(), packet.bytes.size());
  // Not a presentation timestamp in any real sense: it is the arrival time, carried
  // through the decoder so the frame that comes out can be matched to the bytes that
  // went in and the end to end latency measured.
  packet_->pts = packet.received_us;

  const int result = avcodec_send_packet(codec_, packet_);
  if (result < 0 && result != AVERROR(EAGAIN)) {
    if (++decode_errors_ % 120 == 1) {
      char text[AV_ERROR_MAX_STRING_SIZE] = {0};
      av_strerror(result, text, sizeof(text));
      char detail[192];
      std::snprintf(detail, sizeof(detail),
                    "The H.264 decoder rejected a %zu byte packet starting %02x %02x "
                    "%02x %02x %02x: %s",
                    packet.bytes.size(), packet.bytes[0],
                    packet.bytes.size() > 1 ? packet.bytes[1] : 0,
                    packet.bytes.size() > 2 ? packet.bytes[2] : 0,
                    packet.bytes.size() > 3 ? packet.bytes[3] : 0,
                    packet.bytes.size() > 4 ? packet.bytes[4] : 0, text);
      Log(detail);
    }
    return;
  }
  DrainFrames();
}

void VideoDecoder::DrainFrames() {
  for (;;) {
    const int result = avcodec_receive_frame(codec_, frame_);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
      return;
    }
    if (result < 0) {
      if (++decode_errors_ % 120 == 1) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(result, text, sizeof(text));
        Log(std::string("The H.264 decoder failed on a frame: ") + text);
      }
      return;
    }

    const int64_t received_us = frame_->pts;
    const VideoMargins margins = MarginsFor(frame_->width, frame_->height);
    const int32_t visible_width = frame_->width - margins.horizontal();
    const int32_t visible_height = frame_->height - margins.vertical();
    // A size change mid stream is legal and does not disturb anything downstream: the
    // ring carries the size per frame and the output texture is reallocated in place,
    // so the texture id Flutter holds stays the same. Worth saying out loud, though,
    // because the host app lays its overlay out from it. New margins on a frame of the
    // same size count too, since what is shown changes shape.
    const int32_t previous_width = frame_width_.exchange(visible_width);
    const int32_t previous_height = frame_height_.exchange(visible_height);
    const bool resized =
        previous_width != visible_width || previous_height != visible_height;
    if (resized) {
      const std::string size =
          margins.empty() ? std::to_string(frame_->width) + "x" +
                                std::to_string(frame_->height)
                          : DescribeMargins(frame_->width, frame_->height, margins);
      Log("Video: " + size + ", decoded by the " + backend_name() + " backend.");
    }
    // What the frame actually is, not what the context was configured for. libavcodec
    // drops to a software format on its own when the hardware decoder will not take the
    // stream, and reporting VA-API in that case would be a lie in the one place someone
    // would look to find out why the CPU is busy.
    const bool decoded_in_hardware = frame_->format == AV_PIX_FMT_VAAPI;
    if (backend_hardware_.exchange(decoded_in_hardware) != decoded_in_hardware) {
      Log(std::string("The H.264 stream is being decoded by the ") +
          (decoded_in_hardware ? "VA-API" : "software") + " backend.");
    }
    if (decoded_in_hardware) {
      PublishHardware(frame_, margins);
    } else {
      PublishSoftware(frame_, margins);
    }
    frames_decoded_.fetch_add(1);
    NoteLatency(received_us);
    av_frame_unref(frame_);
  }
}

void VideoDecoder::PublishHardware(AVFrame* frame, const VideoMargins& margins) {
  AVFrame* drm = av_frame_alloc();
  if (drm == nullptr) {
    return;
  }
  drm->format = AV_PIX_FMT_DRM_PRIME;
  // MAP_DIRECT asks for the decoder's own surface rather than a copy. Without it
  // libavutil is free to allocate a second surface and blit, which would make this a
  // zero copy path in name only.
  const int result = av_hwframe_map(drm, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
  if (result < 0) {
    av_frame_free(&drm);
    if (++decode_errors_ % 120 == 1) {
      Log("VA-API would not export the decoded surface as a dmabuf. Frames will be "
          "dropped until the decoder is reopened on the software backend.");
    }
    return;
  }

  const auto* descriptor = reinterpret_cast<const AVDRMFrameDescriptor*>(drm->data[0]);
  Frame out;
  out.kind = FrameKind::kDmabuf;
  out.width = frame->width;
  out.height = frame->height;
  ApplyCrop(margins, &out);
  out.color_space = ColorSpaceOf(frame);
  out.full_range = frame->color_range == AVCOL_RANGE_JPEG;

  auto add_layer = [&](uint32_t fourcc, const AVDRMPlaneDescriptor& plane, int32_t width,
                       int32_t height) {
    if (out.layer_count >= Frame::kMaxLayers) {
      return;
    }
    const AVDRMObjectDescriptor& object = descriptor->objects[plane.object_index];
    FrameLayer& layer = out.layers[out.layer_count++];
    layer.fd = object.fd;
    layer.offset = static_cast<uint32_t>(plane.offset);
    layer.pitch = static_cast<uint32_t>(plane.pitch);
    layer.modifier = object.format_modifier;
    layer.fourcc = fourcc;
    layer.width = width;
    layer.height = height;
  };

  if (descriptor->nb_layers == 1 && descriptor->layers[0].nb_planes == 2) {
    // The driver composed the two halves of NV12 into one layer. Split it back out:
    // each half still imports on its own, and a single NV12 image would have to be
    // sampled through an external texture, which Flutter will not take.
    const AVDRMLayerDescriptor& layer = descriptor->layers[0];
    add_layer(DRM_FORMAT_R8, layer.planes[0], frame->width, frame->height);
    add_layer(DRM_FORMAT_GR88, layer.planes[1], (frame->width + 1) / 2,
              (frame->height + 1) / 2);
  } else {
    for (int i = 0; i < descriptor->nb_layers && i < Frame::kMaxLayers; ++i) {
      const AVDRMLayerDescriptor& layer = descriptor->layers[i];
      if (layer.nb_planes < 1) {
        continue;
      }
      const int32_t width = i == 0 ? frame->width : (frame->width + 1) / 2;
      const int32_t height = i == 0 ? frame->height : (frame->height + 1) / 2;
      add_layer(layer.format, layer.planes[0], width, height);
    }
  }

  if (out.layer_count != 2) {
    // Anything other than luma plus chroma is a format the shader does not know, and
    // guessing would paint garbage. Software decode handles it correctly instead.
    av_frame_free(&drm);
    if (++decode_errors_ % 120 == 1) {
      Log("The decoded surface is not a two layer NV12 dmabuf, which the present "
          "adapter cannot import.");
    }
    return;
  }

  // The mapped frame owns the exported file descriptors and holds a reference to the
  // surface they came from, so keeping it alive is exactly what keeps the frame valid.
  std::shared_ptr<void> keepalive(drm, [](void* pointer) {
    AVFrame* owned = static_cast<AVFrame*>(pointer);
    av_frame_free(&owned);
  });
  publish_(out, std::move(keepalive));
}

std::shared_ptr<std::vector<uint8_t>> VideoDecoder::TakeRgbaBuffer(size_t bytes) {
  for (auto& buffer : rgba_pool_) {
    // The ring is the only other holder, so a count of one means the last frame that
    // used this buffer has been superseded and it can be written again.
    if (buffer.use_count() == 1) {
      buffer->resize(bytes);
      return buffer;
    }
  }
  auto buffer = std::make_shared<std::vector<uint8_t>>(bytes);
  if (rgba_pool_.size() < kRgbaPoolSize) {
    rgba_pool_.push_back(buffer);
  }
  return buffer;
}

void VideoDecoder::PublishSoftware(AVFrame* frame, const VideoMargins& margins) {
  const auto source_format = static_cast<AVPixelFormat>(frame->format);
  if (scaler_ == nullptr || scaler_src_format_ != frame->format ||
      scaler_width_ != frame->width || scaler_height_ != frame->height) {
    if (scaler_ != nullptr) {
      sws_freeContext(scaler_);
    }
    // SWS_POINT, not bilinear: this is a straight colour conversion at the same size,
    // so there is nothing to interpolate and the cheapest filter is the right one.
    scaler_ = sws_getContext(frame->width, frame->height, source_format, frame->width,
                             frame->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr,
                             nullptr);
    scaler_src_format_ = frame->format;
    scaler_width_ = frame->width;
    scaler_height_ = frame->height;
    if (scaler_ == nullptr) {
      Log(std::string("Cannot convert ") + av_get_pix_fmt_name(source_format) +
          " frames to RGBA.");
      return;
    }
  }

  const int stride = frame->width * 4;
  auto buffer = TakeRgbaBuffer(static_cast<size_t>(stride) * frame->height);
  uint8_t* destination[4] = {buffer->data(), nullptr, nullptr, nullptr};
  const int destination_stride[4] = {stride, 0, 0, 0};
  sws_scale(scaler_, frame->data, frame->linesize, 0, frame->height, destination,
            destination_stride);

  Frame out;
  out.kind = FrameKind::kCpuRgba;
  out.width = frame->width;
  out.height = frame->height;
  ApplyCrop(margins, &out);
  out.pixels = buffer->data();
  out.stride = stride;
  publish_(out, std::shared_ptr<void>(buffer, buffer.get()));
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
  if (now - last_latency_report_us_ < kLatencyReportIntervalUs || latency_count_ == 0) {
    return;
  }
  const double average = latency_sum_ms_ / static_cast<double>(latency_count_);
  char text[160];
  std::snprintf(text, sizeof(text),
                "Video: %dx%d, %s, %llu frames, %.1f ms from wire to frame.",
                frame_width_.load(), frame_height_.load(), backend_name().c_str(),
                static_cast<unsigned long long>(frames_decoded_.load()), average);
  Log(text);
  latency_sum_ms_ = 0.0;
  latency_count_ = 0;
  last_latency_report_us_ = now;
}

}  // namespace aa
