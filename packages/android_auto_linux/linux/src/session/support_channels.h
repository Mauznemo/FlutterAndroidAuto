// The channels that have to answer before the phone will send a single video frame.
//
// Android Auto does not treat service discovery as a menu. A head unit that offers only
// video, input and audio is not a head unit it is willing to project to: it reads the
// response, sends nothing at all, and drops out of accessory mode a second later, with
// no error anywhere. Offering the microphone and the sensors as well is what makes it
// open every channel and start encoding. That was measured, not guessed; see PLAN.md
// under M4.
//
// So these exist to keep the phone happy, not to do their jobs:
//
//   microphone    accept the channel, never actually capture anything          (M7)
//   sensors       answer the start request, then report "parked" and "day"     (M8)
//
// Input and the three audio sinks used to be on that list. M5 and M6 gave them real
// implementations, so they moved out to session/input_channel.cc and
// session/audio_channels.cc, which is the shape both of these are headed for.
//
// The sensor one is not merely polite. Android Auto locks most of its UI until the head
// unit has told it the driving status, so without it the projection is a phone screen
// saying the car is not ready.
//
// Each of these becomes a real service in its own milestone. Nothing here is meant to
// survive that.

#ifndef ANDROID_AUTO_LINUX_SESSION_SUPPORT_CHANNELS_H_
#define ANDROID_AUTO_LINUX_SESSION_SUPPORT_CHANNELS_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>
#include <aasdk/Channel/MediaSource/MediaSourceService.hpp>
#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "service_discovery.h"

namespace aa {

class SupportChannels;

// Both relays below exist for the reason spelled out on ControlEventRelay: a channel
// binds its event handler into a promise the messenger owns, so a handler that owns the
// channel makes a cycle nothing can break, and a session that never dies never releases
// the USB interface. Both hold a weak reference.

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
  void OnMicrophoneOpen();
  void OnMicrophoneSetup();
  void OnMicrophoneRequest(bool open);
  void OnMicrophoneAck();
  void OnSensorOpen();
  void OnSensorStartRequest(
      const aap_protobuf::service::sensorsource::message::SensorRequest& request);
  void OnChannelError(const std::string& what, const aasdk::error::Error& error);

 private:
  // The live microphone channel, or nullptr if there is none or the session has
  // stopped. Sensor() is the same idea.
  //
  // The same reasoning as InputChannel::Channel(), for a different pair of threads.
  // Every handler here runs on the io_context, but Stop() does not: it is reached from
  // aa_session_stop() on Flutter's platform thread as well as from io threads, so a
  // message can be halfway through being answered while the channels are being dropped.
  // A caller works from its own copy, so a teardown mid handler drops the channel when
  // the last reference goes rather than out from under whoever is using it.
  aasdk::channel::mediasource::IMediaSourceService::Pointer Microphone() const;
  aasdk::channel::sensorsource::ISensorSourceService::Pointer Sensor() const;

  void ListenMicrophone();
  void ListenSensor();
  // Each of these builds its channel under the lock and arms the receive outside it,
  // which is the only ordering that neither races Stop() nor takes the lock twice.
  void AddMicrophone();
  void AddSensor();
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  HeadUnitDescription description_;
  LogHandler log_;

  // Guards microphone_, sensor_ and messenger_. Held for the length of a pointer copy,
  // never across a send.
  mutable std::mutex channels_mutex_;
  aasdk::channel::mediasource::IMediaSourceService::Pointer microphone_;
  std::shared_ptr<MicrophoneRelay> microphone_relay_;
  aasdk::channel::sensorsource::ISensorSourceService::Pointer sensor_;
  std::shared_ptr<SensorRelay> sensor_relay_;

  std::atomic<int32_t> microphone_session_{0};
  std::atomic<bool> stopped_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_SUPPORT_CHANNELS_H_
