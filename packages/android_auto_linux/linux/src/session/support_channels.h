// The channels that have to answer before the phone will send a single video frame.
//
// Android Auto does not treat service discovery as a menu. A head unit that offers
// only video, input and sensors is not a head unit it is willing to project to: it
// reads the response, sends nothing at all, and drops out of accessory mode a second
// later, with no error anywhere. Offering the three audio sinks and the microphone as
// well is what makes it open every channel and start encoding. That was measured, not
// guessed; see PLAN.md under M4.
//
// So these exist to keep the phone happy, not to do their jobs:
//
//   audio sinks   accept the stream and acknowledge it, then discard the PCM   (M6)
//   microphone    accept the channel, never actually capture anything          (M7)
//   sensors       answer the start request, then report "parked" and "day"     (M8)
//
// Input used to be on that list. M5 gave it a real implementation, so it moved out to
// session/input_channel.cc, which is the shape every one of these is headed for.
//
// The sensor one is not merely polite. Android Auto locks most of its UI until the
// head unit has told it the driving status, so without it the projection is a phone
// screen saying the car is not ready.
//
// Each of these becomes a real service in its own milestone. Nothing here is meant to
// survive that: when M6 lands, the audio sink handling moves out of this file wholesale.

#ifndef ANDROID_AUTO_LINUX_SESSION_SUPPORT_CHANNELS_H_
#define ANDROID_AUTO_LINUX_SESSION_SUPPORT_CHANNELS_H_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Audio/AudioMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSource/MediaSourceService.hpp>
#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "service_discovery.h"

namespace aa {

class SupportChannels;

// The three relays below all exist for the reason spelled out on ControlEventRelay:
// a channel binds its event handler into a promise the messenger owns, so a handler
// that owns the channel makes a cycle nothing can break, and a session that never dies
// never releases the USB interface. Every one of them holds a weak reference.

class AudioSinkRelay
    : public aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler {
 public:
  AudioSinkRelay(std::weak_ptr<SupportChannels> owner, aasdk::messenger::ChannelId channel);

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
  std::weak_ptr<SupportChannels> owner_;
  aasdk::messenger::ChannelId channel_;
};

class MicrophoneRelay
    : public aasdk::channel::mediasource::IMediaSourceServiceEventHandler {
 public:
  explicit MicrophoneRelay(std::weak_ptr<SupportChannels> owner);

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
  std::weak_ptr<SupportChannels> owner_;
};

class SensorRelay : public aasdk::channel::sensorsource::ISensorSourceServiceEventHandler {
 public:
  explicit SensorRelay(std::weak_ptr<SupportChannels> owner);

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override;
  void onSensorStartRequest(
      const aap_protobuf::service::sensorsource::message::SensorRequest& request) override;
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<SupportChannels> owner_;
};

class SupportChannels : public std::enable_shared_from_this<SupportChannels> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, for the reason on
  // ProtocolSession::Create. `description` decides which of these are created at all:
  // a channel that service discovery did not advertise must not exist here either.
  static std::shared_ptr<SupportChannels> Create(
      boost::asio::io_context& io_context, aasdk::Strand& strand,
      aasdk::messenger::IMessenger::Pointer messenger,
      const HeadUnitDescription& description, LogHandler log);

  SupportChannels(boost::asio::io_context& io_context, aasdk::Strand& strand,
                  aasdk::messenger::IMessenger::Pointer messenger,
                  const HeadUnitDescription& description, LogHandler log);
  ~SupportChannels();

  // Creates the channels and arms a receive on each.
  void Start();
  void Stop();

  // Called by the relays, never by aasdk directly.
  void OnAudioOpen(aasdk::messenger::ChannelId channel);
  void OnAudioSetup(aasdk::messenger::ChannelId channel);
  void OnAudioStart(aasdk::messenger::ChannelId channel, int32_t session_id);
  void OnAudioStop(aasdk::messenger::ChannelId channel);
  void OnAudioData(aasdk::messenger::ChannelId channel);
  void OnMicrophoneOpen();
  void OnMicrophoneSetup();
  void OnMicrophoneRequest(bool open);
  void OnMicrophoneAck();
  void OnSensorOpen();
  void OnSensorStartRequest(
      const aap_protobuf::service::sensorsource::message::SensorRequest& request);
  void OnChannelError(const std::string& what, const aasdk::error::Error& error);

 private:
  struct AudioSink {
    aasdk::channel::mediasink::audio::IAudioMediaSinkService::Pointer channel;
    std::shared_ptr<AudioSinkRelay> relay;
    int32_t session_id = -1;
  };

  // A copy of one audio sink, or one with a null channel if there is no such sink or
  // the session has stopped. Microphone() and Sensor() are the same idea.
  //
  // The same reasoning as InputChannel::Channel(), for a different pair of threads.
  // Every handler here runs on the io_context, but Stop() does not: it is reached from
  // aa_session_stop() on Flutter's platform thread as well as from io threads, so a
  // buffer can be halfway through being acknowledged while the channels are being
  // dropped. A caller works from its own copy, so a teardown mid handler drops the
  // channel when the last reference goes rather than out from under whoever is using
  // it.
  AudioSink Audio(aasdk::messenger::ChannelId channel) const;
  aasdk::channel::mediasource::IMediaSourceService::Pointer Microphone() const;
  aasdk::channel::sensorsource::ISensorSourceService::Pointer Sensor() const;
  void SetAudioSession(aasdk::messenger::ChannelId channel, int32_t session_id);

  void ListenAudio(aasdk::messenger::ChannelId channel);
  void ListenMicrophone();
  void ListenSensor();
  // Each of these builds its channel under the lock and arms the receive outside it,
  // which is the only ordering that neither races Stop() nor takes the lock twice.
  void AddAudioSink(aasdk::messenger::ChannelId channel);
  void AddMicrophone();
  void AddSensor();
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  HeadUnitDescription description_;
  LogHandler log_;

  // Guards audio_, microphone_, sensor_ and messenger_. Held for the length of a
  // pointer copy, never across a send.
  mutable std::mutex channels_mutex_;
  std::map<aasdk::messenger::ChannelId, AudioSink> audio_;
  aasdk::channel::mediasource::IMediaSourceService::Pointer microphone_;
  std::shared_ptr<MicrophoneRelay> microphone_relay_;
  aasdk::channel::sensorsource::ISensorSourceService::Pointer sensor_;
  std::shared_ptr<SensorRelay> sensor_relay_;

  std::atomic<int32_t> microphone_session_{0};
  std::atomic<bool> stopped_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_SUPPORT_CHANNELS_H_
