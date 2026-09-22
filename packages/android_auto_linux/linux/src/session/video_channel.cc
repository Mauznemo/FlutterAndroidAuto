// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_channel.h"

#include <cstdio>

#include <aasdk/Common/Log.hpp>

#include <aap_protobuf/service/control/message/UpdateUiConfigRequest.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusMode.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace media_pb = aap_protobuf::service::media::shared::message;
namespace source_pb = aap_protobuf::service::media::source::message;
namespace video_pb = aap_protobuf::service::media::video::message;

// How long the view has to hold still before the phone is asked to follow it. A window
// being dragged reports a new size every frame, and every change costs a stream restart.
constexpr std::chrono::milliseconds kResizeSettle{400};
// How long each step of a resize waits on the phone before carrying on without it.
constexpr std::chrono::milliseconds kResizeStepTimeout{1500};

// The first bytes of a buffer, as hex. Only ever used at debug level, and only for the
// first buffer of each kind: what the phone puts on this channel is the one thing that
// cannot be checked by reading a proto file, and the difference between Annex-B and a
// length prefixed stream is visible in the first four bytes.
std::string HexPrefix(const aasdk::common::DataConstBuffer& buffer, size_t count) {
  std::string text;
  char byte[4];
  for (size_t i = 0; i < count && i < buffer.size; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x ", buffer.cdata[i]);
    text += byte;
  }
  return text;
}

}  // namespace

VideoEventRelay::VideoEventRelay(std::weak_ptr<VideoChannel> channel)
    : channel_(std::move(channel)) {}

void VideoEventRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest& request) {
  if (auto channel = channel_.lock()) {
    channel->onChannelOpenRequest(request);
  }
}

void VideoEventRelay::onMediaChannelSetupRequest(const media_pb::Setup& request) {
  if (auto channel = channel_.lock()) {
    channel->onMediaChannelSetupRequest(request);
  }
}

void VideoEventRelay::onMediaChannelStartIndication(const media_pb::Start& indication) {
  if (auto channel = channel_.lock()) {
    channel->onMediaChannelStartIndication(indication);
  }
}

void VideoEventRelay::onMediaChannelStopIndication(const media_pb::Stop& indication) {
  if (auto channel = channel_.lock()) {
    channel->onMediaChannelStopIndication(indication);
  }
}

void VideoEventRelay::onMediaWithTimestampIndication(
    aasdk::messenger::Timestamp::ValueType timestamp,
    const aasdk::common::DataConstBuffer& buffer) {
  if (auto channel = channel_.lock()) {
    channel->onMediaWithTimestampIndication(timestamp, buffer);
  }
}

void VideoEventRelay::onMediaIndication(const aasdk::common::DataConstBuffer& buffer) {
  if (auto channel = channel_.lock()) {
    channel->onMediaIndication(buffer);
  }
}

void VideoEventRelay::onVideoFocusRequest(
    const video_pb::VideoFocusRequestNotification& request) {
  if (auto channel = channel_.lock()) {
    channel->onVideoFocusRequest(request);
  }
}

void VideoEventRelay::onUpdateUiConfigReply(const control_pb::UpdateUiConfigReply& reply) {
  if (auto channel = channel_.lock()) {
    channel->onUpdateUiConfigReply(reply);
  }
}

void VideoEventRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto channel = channel_.lock()) {
    channel->onChannelError(error);
  }
}

std::shared_ptr<VideoChannel> VideoChannel::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger, std::shared_ptr<VideoDecoder> decoder,
    int32_t frame_width, int32_t frame_height, VideoMargins margins, LogHandler log) {
  return std::make_shared<VideoChannel>(io_context, strand, std::move(messenger),
                                        std::move(decoder), frame_width, frame_height,
                                        margins, std::move(log));
}

VideoChannel::VideoChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                           aasdk::messenger::IMessenger::Pointer messenger,
                           std::shared_ptr<VideoDecoder> decoder, int32_t frame_width,
                           int32_t frame_height, VideoMargins margins, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      decoder_(std::move(decoder)),
      log_(std::move(log)),
      frame_width_(frame_width),
      frame_height_(frame_height),
      margins_(margins),
      resize_timer_(io_context) {}

VideoChannel::~VideoChannel() { Stop(); }

void VideoChannel::Start() {
  relay_ = std::make_shared<VideoEventRelay>(weak_from_this());
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    // No messenger means the session was torn down while it was being built. A channel
    // made now would hold a null messenger and segfault on its first receive.
    if (stopped_.load() || !messenger_) {
      return;
    }
    channel_ = std::make_shared<aasdk::channel::mediasink::video::VideoMediaSinkService>(
        strand_, messenger_, aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO);
  }
  // Before the phone can send a frame, since service discovery is what told it to lay
  // out inside these margins and every frame from the first is drawn for them.
  if (decoder_) {
    decoder_->SetMargins(frame_width_, frame_height_, margins_);
  }
  Listen();
}

void VideoChannel::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  streaming_ = false;
  session_id_ = -1;
  // Moved out and destroyed after the lock is dropped, so a handler holding its own
  // reference on an io thread finishes against a live object.
  aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer channel;
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    channel = std::move(channel_);
    messenger_.reset();
  }
}

aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer VideoChannel::Channel()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channel_mutex_);
  return channel_;
}

void VideoChannel::Listen() {
  if (auto channel = Channel()) {
    channel->receive(relay_);
  }
}

void VideoChannel::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

aasdk::channel::SendPromise::Pointer VideoChannel::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`. A send can be rejected on an io_context thread long after the
  // connection that started it has gone.
  std::weak_ptr<VideoChannel> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

void VideoChannel::onChannelOpenRequest(const control_pb::ChannelOpenRequest& request) {
  if (auto channel = Channel()) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    channel->sendChannelOpenResponse(response, MakeSendPromise("video channel open"));
    Log("The phone opened the video channel.");
  }
  Listen();
}

void VideoChannel::onMediaChannelSetupRequest(const media_pb::Setup& request) {
  if (auto channel = Channel()) {
    media_pb::Config response;
    response.set_status(media_pb::Config::STATUS_READY);
    // One frame in flight at a time. The phone waits for the acknowledgement before
    // sending the next, which is what stops it running ahead of a head unit that cannot
    // keep up, and it is what every other implementation asks for.
    response.set_max_unacked(1);
    // Index into the video_configs list we sent during service discovery. There is one.
    response.add_configuration_indices(0);
    channel->sendChannelSetupResponse(response, MakeSendPromise("video setup"));

    // Unsolicited, and it has to be: the phone will not start encoding until it believes
    // the head unit is actually showing the projection.
    SendVideoFocus(true, true);
  }
  Listen();
}

void VideoChannel::SendVideoFocus(bool projected, bool unsolicited) {
  auto channel = Channel();
  if (!channel) {
    return;
  }
  video_pb::VideoFocusNotification notification;
  notification.set_focus(projected ? video_pb::VIDEO_FOCUS_PROJECTED
                                   : video_pb::VIDEO_FOCUS_NATIVE);
  notification.set_unsolicited(unsolicited);
  channel->sendVideoFocusIndication(notification, MakeSendPromise("video focus"));
}

void VideoChannel::onVideoFocusRequest(
    const video_pb::VideoFocusRequestNotification& request) {
  // The phone asks for focus back when it wants to show something on its own screen,
  // and asks for projection when it is done. Answering with what was asked for is
  // right until the host app is given a say in it, which it does not have yet.
  const bool projected = request.mode() == video_pb::VIDEO_FOCUS_PROJECTED ||
                         request.mode() == video_pb::VIDEO_FOCUS_PROJECTED_NO_INPUT_FOCUS;
  SendVideoFocus(projected, false);
  Listen();
}

void VideoChannel::onMediaChannelStartIndication(const media_pb::Start& indication) {
  session_id_ = indication.session_id();
  Log("The phone started the video stream.");
  // A resize asked for while there was no stream to restart waited for this one.
  if (resize_step_ == ResizeStep::kIdle && wanted_ && *wanted_ != margins_) {
    ArmResizeTimer(kResizeSettle);
  }
  Listen();
}

void VideoChannel::onMediaChannelStopIndication(const media_pb::Stop& indication) {
  session_id_ = -1;
  streaming_ = false;
  if (decoder_) {
    decoder_->Flush();
  }
  Log("The phone stopped the video stream.");
  if (resize_step_ == ResizeStep::kReleasing) {
    SendUiConfig();
  }
  Listen();
}

void VideoChannel::onUpdateUiConfigReply(const control_pb::UpdateUiConfigReply& reply) {
  AASDK_LOG(debug) << "[Video] UI config reply: " << reply.ShortDebugString();
  if (resize_step_ == ResizeStep::kUpdating) {
    // The reply carries the margins the phone took, which is what it will draw for, so
    // that is what gets cropped. Only when they fit the frame, though: a reply that
    // makes no sense is not worth cropping the whole picture away for.
    VideoMargins accepted = requested_;
    if (reply.has_ui_config() && reply.ui_config().has_margins()) {
      const auto& insets = reply.ui_config().margins();
      VideoMargins answered;
      answered.top = static_cast<int32_t>(insets.top());
      answered.bottom = static_cast<int32_t>(insets.bottom());
      answered.left = static_cast<int32_t>(insets.left());
      answered.right = static_cast<int32_t>(insets.right());
      if (answered.horizontal() < frame_width_ && answered.vertical() < frame_height_) {
        accepted = answered;
      }
    }
    FinishResize(accepted);
  }
  Listen();
}

void VideoChannel::Resize(VideoMargins margins) {
  std::weak_ptr<VideoChannel> weak = weak_from_this();
  boost::asio::post(strand_, [weak, margins]() {
    auto self = weak.lock();
    if (!self || self->stopped_.load()) {
      return;
    }
    self->wanted_ = margins;
    // One already under way picks the new wish up when it finishes.
    if (self->resize_step_ == ResizeStep::kIdle) {
      self->ArmResizeTimer(kResizeSettle);
    }
  });
}

void VideoChannel::ArmResizeTimer(std::chrono::milliseconds delay) {
  // A generation rather than trusting cancel(): a wait that has already expired is
  // queued to run and cannot be cancelled any more, so it has to be told it is stale.
  const uint64_t generation = ++resize_generation_;
  resize_timer_.expires_after(delay);
  std::weak_ptr<VideoChannel> weak = weak_from_this();
  resize_timer_.async_wait([weak, generation](const boost::system::error_code& error) {
    if (error) {
      return;
    }
    auto self = weak.lock();
    if (!self) {
      return;
    }
    boost::asio::post(self->strand_, [self, generation]() {
      if (!self->stopped_.load() && generation == self->resize_generation_) {
        self->OnResizeTimer();
      }
    });
  });
}

void VideoChannel::OnResizeTimer() {
  switch (resize_step_) {
    case ResizeStep::kIdle:
      if (!wanted_ || *wanted_ == margins_) {
        wanted_.reset();
        return;
      }
      // With no stream there is nothing to restart and nothing drawn for the old
      // margins. The next start indication picks this up.
      if (session_id_.load() < 0) {
        return;
      }
      BeginResize();
      return;
    case ResizeStep::kReleasing:
      Log("The phone did not stop the video stream for the resize, updating anyway.");
      SendUiConfig();
      return;
    case ResizeStep::kUpdating:
      // A phone that does not know the message ignores it and keeps drawing for the
      // old margins, so those are the ones to go on cropping.
      Log("The phone did not answer the new margins, keeping " +
          DescribeMargins(frame_width_, frame_height_, margins_) + ".");
      FinishResize(std::nullopt);
      return;
  }
}

void VideoChannel::BeginResize() {
  requested_ = *wanted_;
  resize_step_ = ResizeStep::kReleasing;
  Log("The view changed shape, asking the phone for " +
      DescribeMargins(frame_width_, frame_height_, requested_) + ".");
  SendVideoFocus(false, true);
  ArmResizeTimer(kResizeStepTimeout);
}

void VideoChannel::SendUiConfig() {
  auto channel = Channel();
  if (!channel) {
    return;
  }
  resize_step_ = ResizeStep::kUpdating;
  control_pb::UpdateUiConfigRequest request;
  auto* insets = request.mutable_ui_config()->mutable_margins();
  insets->set_top(static_cast<uint32_t>(requested_.top));
  insets->set_bottom(static_cast<uint32_t>(requested_.bottom));
  insets->set_left(static_cast<uint32_t>(requested_.left));
  insets->set_right(static_cast<uint32_t>(requested_.right));
  channel->sendUpdateUiConfigRequest(request, MakeSendPromise("UI config update"));
  ArmResizeTimer(kResizeStepTimeout);
}

void VideoChannel::FinishResize(const std::optional<VideoMargins>& accepted) {
  // Whatever timer is outstanding belonged to the step that just ended.
  ++resize_generation_;
  resize_step_ = ResizeStep::kIdle;
  if (accepted) {
    margins_ = *accepted;
    // No frame is in flight: the stream stopped for this and restarts on the focus
    // below, so the first frame drawn for the new margins is the first one cropped.
    if (decoder_) {
      decoder_->SetMargins(frame_width_, frame_height_, margins_);
    }
    Log("The phone laid its interface out for " +
        DescribeMargins(frame_width_, frame_height_, margins_) + ".");
  }
  SendVideoFocus(true, true);
  // Asked again while this one was under way. Compared with what was asked for rather
  // than with what was granted, or a phone that adjusts margins would be asked forever.
  if (wanted_ && *wanted_ != requested_) {
    ArmResizeTimer(kResizeSettle);
  } else {
    wanted_.reset();
  }
}

void VideoChannel::AcknowledgeFrame() {
  const int32_t session_id = session_id_.load();
  auto channel = Channel();
  if (session_id < 0 || !channel) {
    return;
  }
  source_pb::Ack ack;
  ack.set_session_id(session_id);
  ack.set_ack(1);
  channel->sendMediaAckIndication(ack, MakeSendPromise("video ack"));
}

void VideoChannel::onMediaIndication(const aasdk::common::DataConstBuffer& buffer) {
  // The codec config: SPS and PPS, sent once before the first picture and again after
  // any reconfiguration. Not a frame, so it is not acknowledged.
  AASDK_LOG(debug) << "[Video] codec config, " << buffer.size
                   << " bytes: " << HexPrefix(buffer, 16);
  if (decoder_ != nullptr && buffer.size > 0) {
    decoder_->SubmitCodecConfig(buffer.cdata, buffer.size);
  }
  Listen();
}

void VideoChannel::onMediaWithTimestampIndication(
    aasdk::messenger::Timestamp::ValueType timestamp,
    const aasdk::common::DataConstBuffer& buffer) {
  if (!streaming_.exchange(true)) {
    AASDK_LOG(debug) << "[Video] first frame, " << buffer.size
                     << " bytes: " << HexPrefix(buffer, 16);
    Log("First video frame received.");
  } else {
    // Size alone, for every frame after the first. A static screen encodes to a few
    // hundred bytes and a screen that just changed to tens of thousands, so this is
    // what says when the phone reacted to something, which is the other half of the
    // touch latency measurement in input_channel.cc.
    AASDK_LOG(debug) << "[Video] frame, " << buffer.size << " bytes";
  }
  if (decoder_ != nullptr && buffer.size > 0) {
    // The phone's timestamp is on its own clock with no agreed epoch, so it is no use
    // for measuring how long anything took here. Our own arrival time is.
    decoder_->Submit(buffer.cdata, buffer.size, NowMicros());
  }
  // Acknowledge before listening again: with max_unacked at one, the phone is waiting
  // on this before it encodes anything else.
  AcknowledgeFrame();
  Listen();
}

void VideoChannel::onChannelError(const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  // Already stopped: the failure is this channel's own teardown coming back, and
  // reporting it would put a connected state back up after the session had ended.
  if (stopped_.load()) {
    return;
  }
  Log(std::string("Video channel error: ") + error.what());
  Stop();
}

}  // namespace aa
