// SPDX-License-Identifier: GPL-3.0-or-later
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
// And one exchange the head unit starts, when the host app's view changes shape mid
// session and the margins round the phone's interface have to follow. See Resize.
//
// Everything here runs on the io_context. The decoding does not: the bytes are copied
// into the VideoDecoder's queue and handed to its own thread, because a decode on an
// io_context thread stalls the transport, and a stalled transport is what makes a
// phone drop the session.

#ifndef ANDROID_AUTO_LINUX_SESSION_VIDEO_CHANNEL_H_
#define ANDROID_AUTO_LINUX_SESSION_VIDEO_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSink/Video/VideoMediaSinkService.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "../video/video_decoder.h"
#include "../video/video_margins.h"

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
  void onUpdateUiConfigReply(
      const aap_protobuf::service::control::message::UpdateUiConfigReply& reply) override;
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
  //
  // `frame_width` by `frame_height` is the frame size service discovery advertised and
  // `margins` what it asked the phone to leave clear inside it, both of which the
  // decoder is told on Start so it crops them off.
  static std::shared_ptr<VideoChannel> Create(boost::asio::io_context& io_context,
                                              aasdk::Strand& strand,
                                              aasdk::messenger::IMessenger::Pointer messenger,
                                              std::shared_ptr<VideoDecoder> decoder,
                                              int32_t frame_width, int32_t frame_height,
                                              VideoMargins margins, LogHandler log);

  VideoChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
               aasdk::messenger::IMessenger::Pointer messenger,
               std::shared_ptr<VideoDecoder> decoder, int32_t frame_width,
               int32_t frame_height, VideoMargins margins, LogHandler log);
  ~VideoChannel();

  // Arms the first receive. Call as soon as the messenger exists: a message that
  // arrives for a channel with no outstanding receive is buffered, but arming early
  // keeps the ordering obvious.
  void Start();

  // Drops the channel. The decoder is left running, since it belongs to the session
  // rather than to one connection.
  void Stop();

  // Whether any frame has arrived on this channel yet.
  bool streaming() const { return streaming_.load(); }

  // Asks the phone to lay its interface out inside new margins. Safe from any thread.
  //
  // Settled after a pause, so a window being dragged to a new size costs one change
  // rather than one per frame of the drag. Then, measured on a Pixel 8 Pro:
  //
  //   video focus native     ->  the phone stops the stream
  //   UpdateUiConfigRequest  ->  UpdateUiConfigReply, with the margins it took
  //   video focus projected  ->  a new stream, laid out for them from its first frame
  //
  // The focus round trip is not ceremony. Sent on its own, the update re-lays out the
  // phone's own launcher but leaves the app in front drawn for the old margins, and the
  // stream stops dead until something else on the screen changes. And the stream
  // restarting is what makes the crop exact: the decoder is told the new margins while
  // no frame is in flight, so no frame is ever cropped for the wrong layout.
  void Resize(VideoMargins margins);

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
  void onUpdateUiConfigReply(
      const aap_protobuf::service::control::message::UpdateUiConfigReply& reply);
  void onChannelError(const aasdk::error::Error& error);

 private:
  // Where a Resize has got to. Strand only, like everything the resize touches.
  enum class ResizeStep {
    kIdle,
    // Focus given back, waiting for the phone to stop the stream.
    kReleasing,
    // The update sent, waiting for the phone's reply.
    kUpdating,
  };

  // A reference to the live channel, or nullptr once stopped.
  //
  // The same reasoning as InputChannel::Channel(), for a different pair of threads.
  // Every handler here runs on the io_context, but Stop() does not: it is reached from
  // aa_session_stop() on Flutter's platform thread as well as from io threads, so a
  // frame can be halfway through being acknowledged while the channel is being dropped.
  // A caller takes its own reference and works from that, so a teardown mid handler
  // drops the channel when the last reference goes rather than out from under whoever
  // is using it.
  aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer Channel() const;

  void Listen();
  // Tells the phone the projection is on screen. Sent unsolicited after setup, because
  // the phone will not start encoding until it believes the head unit is showing it.
  void SendVideoFocus(bool projected, bool unsolicited);
  void AcknowledgeFrame();
  // The steps of a Resize, in order. Strand only.
  void ArmResizeTimer(std::chrono::milliseconds delay);
  void OnResizeTimer();
  void BeginResize();
  void SendUiConfig();
  void FinishResize(const std::optional<VideoMargins>& accepted);
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  std::shared_ptr<VideoDecoder> decoder_;
  LogHandler log_;

  // Guards channel_ and messenger_ only. Held for the length of a pointer copy, never
  // across a send.
  mutable std::mutex channel_mutex_;
  aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer channel_;
  std::shared_ptr<VideoEventRelay> relay_;

  std::atomic<int32_t> session_id_{-1};
  std::atomic<bool> streaming_{false};
  std::atomic<bool> stopped_{false};

  const int32_t frame_width_;
  const int32_t frame_height_;
  // Strand only, past construction. What the phone is laying out for now, what the host
  // app last asked for, and what was sent and not yet answered.
  VideoMargins margins_;
  std::optional<VideoMargins> wanted_;
  VideoMargins requested_;
  ResizeStep resize_step_ = ResizeStep::kIdle;
  // Bumped by every arm and by a finish, so a timer that has already fired can tell it
  // belongs to a step that is over.
  uint64_t resize_generation_ = 0;
  // The settling pause, and the give up if the phone does not answer a step.
  boost::asio::steady_timer resize_timer_;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_VIDEO_CHANNEL_H_
