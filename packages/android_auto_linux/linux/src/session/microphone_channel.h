// The MEDIA_SOURCE_MICROPHONE channel: the head unit's microphone, on its way to the
// phone's Assistant.
//
// The exchange looks like an audio sink's run backwards, and the differences are all in
// the last two lines:
//
//   channel open request     ->  channel open response
//   media setup request      ->  config, "ready", one configuration
//   microphone request open  ->  a session id, then capture starts
//   ... buffers flow outwards, one per 32 ms ...
//   microphone request close ->  answered, and capture stops
//
// Two things make this unlike every other channel here.
//
// **It is the only channel that turns hardware on.** Everything else answers what the
// phone sends. This opens the machine's microphone, so the rule is that the device is
// touched when the phone asks and at no other time: `AudioInput::Start` on the open
// request, `AudioInput::Stop` on the close request, on a channel error, and on teardown.
// There is no path that leaves it capturing.
//
// **Buffers are produced on a thread that is not the io_context.** The capture thread
// blocks on the microphone, which is what paces the stream to real time, so it posts
// each buffer onto the channel strand rather than sending in place, exactly as
// InputChannel does with a finger movement.
//
// This lived in support_channels.cc until M7, answering the phone and capturing nothing.

#ifndef ANDROID_AUTO_LINUX_SESSION_MICROPHONE_CHANNEL_H_
#define ANDROID_AUTO_LINUX_SESSION_MICROPHONE_CHANNEL_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSource/Audio/MicrophoneAudioChannel.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>
#include <aasdk/Common/Data.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "../audio/audio_input.h"

namespace aa {

class MicrophoneChannel;

// Forwards the channel's events without keeping the owner alive.
//
// The reason ControlEventRelay exists, for the same machinery: receive() binds the
// handler into a promise the messenger owns, so a handler that owns the channel makes a
// cycle nothing can break, and a session that never dies never releases the USB
// interface.
class MicrophoneRelay
    : public aasdk::channel::mediasource::IMediaSourceServiceEventHandler {
 public:
  explicit MicrophoneRelay(std::weak_ptr<MicrophoneChannel> owner);

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override;
  void onMediaChannelSetupRequest(
      const aap_protobuf::service::media::shared::message::Setup& request) override;
  void onMediaSourceOpenRequest(
      const aap_protobuf::service::media::source::message::MicrophoneRequest& request)
      override;
  void onMediaChannelAckIndication(
      const aap_protobuf::service::media::source::message::Ack& indication) override;
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<MicrophoneChannel> owner_;
};

class MicrophoneChannel : public std::enable_shared_from_this<MicrophoneChannel> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. `input` outlives the session, the way the video decoder and
  // the audio output do: which microphone to use belongs to the head unit rather than to
  // whichever phone is plugged in.
  static std::shared_ptr<MicrophoneChannel> Create(
      boost::asio::io_context& io_context, aasdk::Strand& strand,
      aasdk::messenger::IMessenger::Pointer messenger,
      std::shared_ptr<AudioInput> input, LogHandler log);

  MicrophoneChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                    aasdk::messenger::IMessenger::Pointer messenger,
                    std::shared_ptr<AudioInput> input, LogHandler log);
  ~MicrophoneChannel();

  // Creates the channel and arms the first receive.
  void Start();
  // Stops capture first, then drops the channel. Safe from any thread.
  void Stop();

  // Called by MicrophoneRelay, never by aasdk directly.
  void OnOpen();
  void OnSetup();
  void OnRequest(
      const aap_protobuf::service::media::source::message::MicrophoneRequest& request);
  void OnAck(const aap_protobuf::service::media::source::message::Ack& indication);
  void OnChannelError(const aasdk::error::Error& error);

 private:
  // How many buffers may be in flight before one is dropped.
  //
  // Ten of them is about a third of a second of speech: enough to ride out a stalled io
  // thread, short enough that a link which has stopped moving does not hand the
  // Assistant a third of a second of the past.
  //
  // This is a latency bound on this head unit's own send queue, which is not the same
  // quantity `max_unacked` in the phone's request describes. See Window().
  static constexpr int32_t kQueueBound = 10;

  // A reference to the live channel, or nullptr once stopped.
  //
  // The same rule as InputChannel::Channel(), and for the same reason: handlers run on
  // the io_context while buffers arrive from the capture thread and Stop() can come from
  // Flutter's platform thread, so a caller takes its own reference and works from that.
  aasdk::channel::mediasource::IMediaSourceService::Pointer Channel() const;

  void Listen();
  // How many buffers may be outstanding right now.
  //
  // The phone's `max_unacked` is its receive window, and honouring it is only meaningful
  // against a phone that actually acknowledges: the Pixel tested against asks for two
  // and then never sends a single Ack, so taking it literally would cap this head unit
  // at 64 ms of speech in flight for no reason anyone could observe. So the phone's
  // number is used once it has proved it acknowledges, and kQueueBound until then. Right
  // under both readings, and the measurement that motivated it is in PLAN.md under M7.
  int32_t Window() const;
  // Capture thread. Copies the buffer and posts the send onto the strand.
  void Publish(const uint8_t* data, size_t size);
  // Strand only. Writes one buffer out, timestamped as it leaves.
  void Deliver(aasdk::common::Data data);
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  // Like MakeSendPromise, but also releases the in flight slot the buffer took. Both
  // outcomes release it: a rejected send is a buffer that is never going to arrive.
  aasdk::channel::SendPromise::Pointer MakeBufferPromise();
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  std::shared_ptr<AudioInput> input_;
  LogHandler log_;

  // Guards channel_ and messenger_. Held for the length of a pointer copy, never across
  // a send.
  mutable std::mutex channel_mutex_;
  aasdk::channel::mediasource::IMediaSourceService::Pointer channel_;
  std::shared_ptr<MicrophoneRelay> relay_;

  // The id the head unit hands the phone in its open response. Unlike a media sink,
  // where the phone chooses it, the source side names its own session and every
  // acknowledgement quotes it back.
  std::atomic<int32_t> session_id_{0};
  std::atomic<bool> capturing_{false};
  std::atomic<bool> stopped_{false};

  // Buffers posted but not yet written to USB. This is the backpressure, and it is
  // counted against the send rather than against the phone's acknowledgements on
  // purpose: a phone that never acknowledges would otherwise stall the microphone
  // outright, while a link that has stopped moving has to drop something, and for live
  // speech the right thing to drop is the oldest.
  std::atomic<int32_t> in_flight_{0};
  // What the phone asked for in its open request, or kQueueBound if it did not say.
  std::atomic<int32_t> max_unacked_{kQueueBound};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> acked_{0};

  // Whether the "buffers are being dropped" line has already gone out for this open.
  // Once per open, not once per buffer: at thirty a second the log would be the problem.
  std::atomic<bool> reported_drops_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_MICROPHONE_CHANNEL_H_
