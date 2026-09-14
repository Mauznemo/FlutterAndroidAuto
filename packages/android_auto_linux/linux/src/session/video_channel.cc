#include "video_channel.h"

#include <cstdio>

#include <aasdk/Common/Log.hpp>

#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusMode.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace media_pb = aap_protobuf::service::media::shared::message;
namespace source_pb = aap_protobuf::service::media::source::message;
namespace video_pb = aap_protobuf::service::media::video::message;

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

void VideoEventRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto channel = channel_.lock()) {
    channel->onChannelError(error);
  }
}

std::shared_ptr<VideoChannel> VideoChannel::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger, std::shared_ptr<VideoDecoder> decoder,
    LogHandler log) {
  return std::make_shared<VideoChannel>(io_context, strand, std::move(messenger),
                                        std::move(decoder), std::move(log));
}

VideoChannel::VideoChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                           aasdk::messenger::IMessenger::Pointer messenger,
                           std::shared_ptr<VideoDecoder> decoder, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      decoder_(std::move(decoder)),
      log_(std::move(log)) {}

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
  // right until the host app has a say in it, which is M9's problem.
  const bool projected = request.mode() == video_pb::VIDEO_FOCUS_PROJECTED ||
                         request.mode() == video_pb::VIDEO_FOCUS_PROJECTED_NO_INPUT_FOCUS;
  SendVideoFocus(projected, false);
  Listen();
}

void VideoChannel::onMediaChannelStartIndication(const media_pb::Start& indication) {
  session_id_ = indication.session_id();
  Log("The phone started the video stream.");
  Listen();
}

void VideoChannel::onMediaChannelStopIndication(const media_pb::Stop& indication) {
  session_id_ = -1;
  streaming_ = false;
  if (decoder_) {
    decoder_->Flush();
  }
  Log("The phone stopped the video stream.");
  Listen();
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
