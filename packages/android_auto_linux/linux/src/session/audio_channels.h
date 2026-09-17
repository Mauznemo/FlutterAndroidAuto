// SPDX-License-Identifier: GPL-3.0-or-later
// The three MEDIA_SINK audio channels: media, system and speech.
//
// The exchange is the same on all three, and the same one video runs:
//
//   channel open request   ->  channel open response
//   media setup request    ->  config, "ready", one configuration, ten unacked buffers
//   start indication       ->  a session id every acknowledgement has to carry
//   media data + timestamp ->  one PCM buffer per message, queued and acknowledged
//   stop indication        ->  what is queued is played out, the stream stays open
//
// What the phone sends is raw signed 16 bit little endian PCM, at the rate and channel
// count service discovery advertised for that sink. There is no codec and no container,
// so there is nothing to decode: the bytes go straight to AudioOutput, which owns the
// threads that actually wait on the speakers.
//
// Everything here runs on the io_context, and nothing here blocks. The moment a buffer
// is handed to AudioOutput it is acknowledged, which lets the phone send the next one;
// the pacing is done by the writer threads instead, where waiting is free.

#ifndef ANDROID_AUTO_LINUX_SESSION_AUDIO_CHANNELS_H_
#define ANDROID_AUTO_LINUX_SESSION_AUDIO_CHANNELS_H_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Audio/AudioMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkServiceEventHandler.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "../audio/audio_output.h"
#include "service_discovery.h"

namespace aa {

class AudioChannels;

// Forwards one audio channel's events without keeping the owner alive.
//
// The reason ControlEventRelay exists, for the same channel machinery: receive() binds
// the handler into a promise the messenger owns, so a handler that owns the channel
// makes a cycle nothing can break, and a session that never dies never releases the USB
// interface. One relay per channel, because the event handler interface carries no
// channel id of its own.
class AudioSinkRelay
    : public aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler {
 public:
  AudioSinkRelay(std::weak_ptr<AudioChannels> owner, aasdk::messenger::ChannelId channel);

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
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<AudioChannels> owner_;
  aasdk::messenger::ChannelId channel_;
};

class AudioChannels : public std::enable_shared_from_this<AudioChannels> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. `description` decides which sinks exist at all: a channel
  // service discovery did not advertise must not be created here either. `output`
  // outlives the session, the way the video decoder does.
  static std::shared_ptr<AudioChannels> Create(
      boost::asio::io_context& io_context, aasdk::Strand& strand,
      aasdk::messenger::IMessenger::Pointer messenger,
      const HeadUnitDescription& description, std::shared_ptr<AudioOutput> output,
      LogHandler log);

  AudioChannels(boost::asio::io_context& io_context, aasdk::Strand& strand,
                aasdk::messenger::IMessenger::Pointer messenger,
                const HeadUnitDescription& description,
                std::shared_ptr<AudioOutput> output, LogHandler log);
  ~AudioChannels();

  // Creates the advertised sinks and arms a receive on each.
  void Start();
  void Stop();

  // Called by AudioSinkRelay, never by aasdk directly.
  void OnOpen(aasdk::messenger::ChannelId channel);
  void OnSetup(aasdk::messenger::ChannelId channel);
  void OnStart(aasdk::messenger::ChannelId channel, int32_t session_id);
  void OnStop(aasdk::messenger::ChannelId channel);
  void OnData(aasdk::messenger::ChannelId channel,
              const aasdk::common::DataConstBuffer& buffer);
  void OnCodecConfig(aasdk::messenger::ChannelId channel,
                     const aasdk::common::DataConstBuffer& buffer);
  void OnChannelError(aasdk::messenger::ChannelId channel,
                      const aasdk::error::Error& error);

 private:
  struct Sink {
    aasdk::channel::mediasink::audio::IAudioMediaSinkService::Pointer channel;
    std::shared_ptr<AudioSinkRelay> relay;
    AudioStream stream = AudioStream::kMedia;
    // The session id the phone gave in its start indication, or -1 while stopped. Every
    // acknowledgement has to carry it, so a buffer that arrives before the start
    // indication cannot be acknowledged and is not played either.
    int32_t session_id = -1;
    bool heard = false;
  };

  // A copy of one sink, or one with a null channel if there is no such sink or the
  // session has stopped.
  //
  // The same reasoning as VideoChannel::Channel(): every handler here runs on the
  // io_context, but Stop() is reached from aa_session_stop on Flutter's platform thread
  // as well, so a buffer can be halfway through being acknowledged while the channels
  // are being dropped. A caller works from its own copy, so a teardown mid handler
  // drops the channel when the last reference goes rather than out from under whoever
  // is using it.
  Sink Get(aasdk::messenger::ChannelId channel) const;
  void SetSession(aasdk::messenger::ChannelId channel, int32_t session_id);
  void MarkHeard(aasdk::messenger::ChannelId channel);

  void Add(aasdk::messenger::ChannelId channel, AudioStream stream);
  void Listen(aasdk::messenger::ChannelId channel);
  void Acknowledge(const Sink& sink);
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  HeadUnitDescription description_;
  std::shared_ptr<AudioOutput> output_;
  LogHandler log_;

  // Guards sinks_ and messenger_. Held for the length of a pointer copy, never across
  // a send.
  mutable std::mutex sinks_mutex_;
  std::map<aasdk::messenger::ChannelId, Sink> sinks_;

  std::atomic<bool> stopped_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_AUDIO_CHANNELS_H_
