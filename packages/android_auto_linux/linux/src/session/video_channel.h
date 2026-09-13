// The MEDIA_SINK_VIDEO channel: what the phone talks to when it wants to project.
//
// The exchange, in order, and every step waits on the one before:
//
//   channel open request   ->  channel open response
//   media setup request    ->  config, "ready", one configuration, one unacked frame
//   video focus            ->  we tell the phone the projection is on screen
//   start indication       ->  a session id that every acknowledgement has to carry
//   codec config           ->  SPS and PPS, out of band, before any picture
//   media data + timestamp ->  one Annex-B access unit per message, decoded and shown
//
// Everything here runs on the io_context. The decoding does not: the bytes are copied
// into the VideoDecoder's queue and handed to its own thread, because a decode on an
// io_context thread stalls the USB transport and a stalled transport is what makes a
// phone drop the session.

#ifndef ANDROID_AUTO_LINUX_SESSION_VIDEO_CHANNEL_H_
#define ANDROID_AUTO_LINUX_SESSION_VIDEO_CHANNEL_H_

#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSink/Video/VideoMediaSinkService.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "../video/video_decoder.h"

namespace aa {

class VideoChannel;

// Forwards video channel events to a VideoChannel without keeping it alive.
//
// Exactly the reason ControlEventRelay exists, for exactly the same channel machinery:
// receive() binds the handler into a promise the messenger owns, so handing it the
// channel itself would make channel -> promise -> channel and nothing would ever be
// destroyed. A session that is never destroyed never releases the USB interface.
class VideoEventRelay
    : public aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler {
 public:
  explicit VideoEventRelay(std::weak_ptr<VideoChannel> channel);

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override;
  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup& request) override;
  void onMediaChannelStartIndication(
      const aap_protobuf::service::media::shared::message::Start& indication) override;
  void onMediaChannelStopIndication(
      const aap_protobuf::service::media::shared::message::Stop& indication) override;
  void onMediaWithTimestampIndication(
      aasdk::messenger::Timestamp::ValueType timestamp,
      const aasdk::common::DataConstBuffer& buffer) override;
  void onMediaIndication(const aasdk::common::DataConstBuffer& buffer) override;
  void onVideoFocusRequest(
      const aap_protobuf::service::media::video::message::VideoFocusRequestNotification&
          request) override;
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<VideoChannel> channel_;
};

class VideoChannel : public std::enable_shared_from_this<VideoChannel> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. The decoder is held by shared_ptr rather than a raw
  // pointer because an aasdk promise can deliver an event after the owning session has
  // let go, and a decoder that is merely kept alive one moment too long is harmless
  // where a dangling one is not.
  static std::shared_ptr<VideoChannel> Create(boost::asio::io_context& io_context,
                                              aasdk::Strand& strand,
                                              aasdk::messenger::IMessenger::Pointer messenger,
                                              std::shared_ptr<VideoDecoder> decoder,
                                              LogHandler log);

  VideoChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
               aasdk::messenger::IMessenger::Pointer messenger,
               std::shared_ptr<VideoDecoder> decoder, LogHandler log);
  ~VideoChannel();

  // Arms the first receive. Call as soon as the messenger exists: a message that
  // arrives for a channel with no outstanding receive is buffered, but arming early
  // keeps the ordering obvious.
  void Start();

  // Drops the channel. The decoder is left running, since it belongs to the session
  // rather than to one connection.
  void Stop();

  // Whether any frame has arrived on this channel yet.
  bool streaming() const { return streaming_; }

  // Called by VideoEventRelay, never by aasdk directly.
  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request);
  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup& request);
  void onMediaChannelStartIndication(
      const aap_protobuf::service::media::shared::message::Start& indication);
  void onMediaChannelStopIndication(
      const aap_protobuf::service::media::shared::message::Stop& indication);
  void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                      const aasdk::common::DataConstBuffer& buffer);
  void onMediaIndication(const aasdk::common::DataConstBuffer& buffer);
  void onVideoFocusRequest(
      const aap_protobuf::service::media::video::message::VideoFocusRequestNotification&
          request);
  void onChannelError(const aasdk::error::Error& error);

 private:
  void Listen();
  // Tells the phone the projection is on screen. Sent unsolicited after setup, because
  // the phone will not start encoding until it believes the head unit is showing it.
  void SendVideoFocus(bool projected, bool unsolicited);
  void AcknowledgeFrame();
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  std::shared_ptr<VideoDecoder> decoder_;
  LogHandler log_;

  aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer channel_;
  std::shared_ptr<VideoEventRelay> relay_;

  int32_t session_id_ = -1;
  bool streaming_ = false;
  bool stopped_ = false;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_VIDEO_CHANNEL_H_
