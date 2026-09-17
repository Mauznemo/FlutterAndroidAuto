// SPDX-License-Identifier: GPL-3.0-or-later
// Drives one connection, from an accessory mode USB device to a projecting phone.
//
// The sequence is fixed and every step gates the next:
//
//   version request  ->  version response
//   SSL handshake    ->  (several round trips through the control channel)
//   auth complete    ->  phone accepts us as a head unit
//   service discovery request  ->  our response describing this head unit
//   channels open, phone starts projecting
//
// Everything here runs on the io_context. Nothing touches Flutter or Dart directly:
// state changes go out through the callbacks the owner installs.

#ifndef ANDROID_AUTO_LINUX_SESSION_PROTOCOL_SESSION_H_
#define ANDROID_AUTO_LINUX_SESSION_PROTOCOL_SESSION_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Transport/ITransport.hpp>
#include <aasdk/USB/IAOAPDevice.hpp>

#include "../video/video_decoder.h"
#include "service_discovery.h"

namespace aa {

class AudioChannels;
class AudioInput;
class AudioOutput;
class InputChannel;
class MetadataChannels;
class MetadataState;
class MicrophoneChannel;
class ProtocolSession;
class SensorChannel;
class VideoChannel;

// Forwards control channel events to a ProtocolSession without keeping it alive.
//
// This exists to break a reference cycle that otherwise leaks the whole connection.
// ControlServiceChannel::receive() binds the event handler into a promise it hands to
// the messenger, and the session owns the channel, so passing the session itself makes
// session -> channel -> promise -> session. Nothing is ever destroyed, which means
// AOAPDevice's destructor never runs, which means the USB interface is never released,
// which means every reconnect fails with LIBUSB_ERROR_BUSY. A weak reference here costs
// one indirection and makes the whole thing collectable.
class ControlEventRelay
    : public aasdk::channel::control::IControlServiceChannelEventHandler {
 public:
  explicit ControlEventRelay(std::weak_ptr<ProtocolSession> session);

  void onVersionResponse(uint16_t major, uint16_t minor,
                         aap_protobuf::shared::MessageStatus status) override;
  void onHandshake(const aasdk::common::DataConstBuffer& payload) override;
  void onServiceDiscoveryRequest(
      const aap_protobuf::service::control::message::ServiceDiscoveryRequest& request)
      override;
  void onAudioFocusRequest(
      const aap_protobuf::service::control::message::AudioFocusRequest& request) override;
  void onByeByeRequest(
      const aap_protobuf::service::control::message::ByeByeRequest& request) override;
  void onByeByeResponse(
      const aap_protobuf::service::control::message::ByeByeResponse& response) override;
  void onBatteryStatusNotification(
      const aap_protobuf::service::control::message::BatteryStatusNotification&
          notification) override;
  void onNavigationFocusRequest(
      const aap_protobuf::service::control::message::NavFocusRequestNotification& request)
      override;
  void onVoiceSessionRequest(
      const aap_protobuf::service::control::message::VoiceSessionNotification& request)
      override;
  void onPingRequest(
      const aap_protobuf::service::control::message::PingRequest& request) override;
  void onPingResponse(
      const aap_protobuf::service::control::message::PingResponse& response) override;
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<ProtocolSession> session_;
};

class ProtocolSession : public std::enable_shared_from_this<ProtocolSession> {
 public:
  using StateHandler = std::function<void(int, const std::string&)>;
  // Hands the owner the input channel as it comes and goes. Called with the channel
  // once it exists, and with nullptr when the session ends, both on an io_context
  // thread. The owner is what the C ABI's send calls reach, and they arrive on
  // Flutter's platform thread, so it has to be told rather than allowed to reach in.
  using InputHandler = std::function<void(std::shared_ptr<InputChannel>)>;
  // The same, for the metadata channels. Only the media browser is ever called from
  // outside, and only from Flutter's platform thread, so it needs the same treatment
  // the input channel does rather than being reached into.
  using MetadataHandler = std::function<void(std::shared_ptr<MetadataChannels>)>;

  // `strand` must outlive every ProtocolSession built on it. aasdk's Channel base holds
  // it by reference and posts to it from promise handlers that can run after the
  // session is gone, so a strand owned by the session is a use after free waiting to
  // happen. The owner keeps one for the life of the process.
  //
  // `decoder`, `audio`, `microphone` and `sensors` outlive the session: they belong to
  // the head unit, not to one connection, so a phone reconnecting does not pay for
  // opening VA-API or the audio server again, the volume and the chosen devices survive
  // the reconnect, and so does what the car is doing. A parking brake does not come off
  // because a cable was pulled out.
  //
  // `metadata` is the object that outlives the session but not its contents. What the
  // phone is playing is only true while the phone is there, so the channels clear it on
  // the way out; the object itself is kept so a host app can hold one listener across
  // reconnects.
  static std::shared_ptr<ProtocolSession> Create(boost::asio::io_context& io_context,
                                                 aasdk::Strand& strand,
                                                 HeadUnitDescription description,
                                                 std::shared_ptr<VideoDecoder> decoder,
                                                 std::shared_ptr<AudioOutput> audio,
                                                 std::shared_ptr<AudioInput> microphone,
                                                 std::shared_ptr<SensorState> sensors,
                                                 std::shared_ptr<MetadataState> metadata,
                                                 StateHandler on_state,
                                                 InputHandler on_input,
                                                 MetadataHandler on_metadata);

  ProtocolSession(boost::asio::io_context& io_context, aasdk::Strand& strand,
                  HeadUnitDescription description, std::shared_ptr<VideoDecoder> decoder,
                  std::shared_ptr<AudioOutput> audio,
                  std::shared_ptr<AudioInput> microphone,
                  std::shared_ptr<SensorState> sensors,
                  std::shared_ptr<MetadataState> metadata, StateHandler on_state,
                  InputHandler on_input, MetadataHandler on_metadata);
  ~ProtocolSession();

  // Begins the handshake with a device that has already reached accessory mode.
  void Start(aasdk::usb::IAOAPDevice::Pointer device);

  // The same, for a phone that dialled in over Wi-Fi. Everything above the transport
  // is identical: the version request, the SSL handshake, service discovery and every
  // channel are byte for byte what goes over the cable, which is why the wireless work
  // stops at producing one of these.
  void Start(aasdk::transport::ITransport::Pointer transport);

  // Asks the phone to end its side of the session. Returns immediately; the phone's
  // acknowledgement arrives asynchronously, so pair this with WaitForShutdown.
  //
  // Blocks briefly if a Start() is still building the session on an io thread, because
  // a goodbye sent before there is a control channel to send it on is no goodbye at
  // all: the phone keeps Android Auto running and the user sees it still on the screen
  // after pressing stop.
  //
  // This matters more than it looks. Android Auto on the phone keeps its session, and
  // its hold on the USB interface, until it is told the head unit is going away. Drop
  // the link without saying so and the phone stays in Android Auto with its persistent
  // notification showing, and the next connection attempt cannot claim the interface.
  void Shutdown();

  // Blocks until the phone acknowledges the shutdown or `timeout` elapses. Returns true
  // if the phone answered. Called from the platform thread, so keep the timeout short
  // enough that a user pressing stop does not think the app has hung.
  bool WaitForShutdown(std::chrono::milliseconds timeout);

  // Tears the connection down immediately. Safe from any thread.
  void Stop();

  // The channels the phone accepted, filled in once service discovery completes.
  std::vector<std::string> opened_channels() const { return opened_channels_; }

  // Called by ControlEventRelay, never by aasdk directly.
  void onVersionResponse(uint16_t major, uint16_t minor,
                         aap_protobuf::shared::MessageStatus status);
  void onHandshake(const aasdk::common::DataConstBuffer& payload);
  void onServiceDiscoveryRequest(
      const aap_protobuf::service::control::message::ServiceDiscoveryRequest& request);
  void onAudioFocusRequest(
      const aap_protobuf::service::control::message::AudioFocusRequest& request);
  void onByeByeRequest(
      const aap_protobuf::service::control::message::ByeByeRequest& request);
  void onByeByeResponse(
      const aap_protobuf::service::control::message::ByeByeResponse& response);
  void onBatteryStatusNotification(
      const aap_protobuf::service::control::message::BatteryStatusNotification&
          notification);
  void onNavigationFocusRequest(
      const aap_protobuf::service::control::message::NavFocusRequestNotification& request);
  void onVoiceSessionRequest(
      const aap_protobuf::service::control::message::VoiceSessionNotification& request);
  void onPingRequest(const aap_protobuf::service::control::message::PingRequest& request);
  void onPingResponse(
      const aap_protobuf::service::control::message::PingResponse& response);
  void onChannelError(const aasdk::error::Error& error);

 private:
  // A reference to the live control channel, or nullptr once stopped.
  //
  // The same rule as VideoChannel::Channel(), for the same reason: every handler here
  // runs on an io_context thread while Stop() drops the channel from Flutter's platform
  // thread. A caller takes its own reference and works from that.
  aasdk::channel::control::IControlServiceChannel::Pointer Control() const;

  // The common half of the two Start overloads. Called with lifecycle_mutex_ held.
  void StartLocked(aasdk::transport::ITransport::Pointer transport);
  void SendHandshakeStep();
  // Arms AA_FAULT_TRANSPORT_AFTER, see the note in the .cc file.
  void ArmFaultInjection();
  void NoteShutdownAcknowledged();
  void Listen();
  void ReportState(int state, const std::string& message);
  // Sends a message and reports, but does not tear down, if it fails. Used for the
  // replies that keep the phone happy rather than the ones that gate progress.
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  HeadUnitDescription description_;
  StateHandler on_state_;
  InputHandler on_input_;
  MetadataHandler on_metadata_;

  // Only a wired session has one. Kept because the device's destructor is what
  // releases the USB interface, and nothing else holds it once Start has returned.
  aasdk::usb::IAOAPDevice::Pointer device_;
  aasdk::transport::ITransport::Pointer transport_;
  aasdk::messenger::ICryptor::Pointer cryptor_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  // Guards control_channel_ only, and is always taken after lifecycle_mutex_ where both
  // are held.
  mutable std::mutex control_mutex_;
  aasdk::channel::control::IControlServiceChannel::Pointer control_channel_;
  std::shared_ptr<ControlEventRelay> relay_;
  std::shared_ptr<VideoDecoder> decoder_;
  std::shared_ptr<AudioOutput> audio_;
  std::shared_ptr<AudioInput> microphone_;
  std::shared_ptr<SensorState> sensors_;
  std::shared_ptr<MetadataState> metadata_;
  std::shared_ptr<VideoChannel> video_channel_;
  std::shared_ptr<InputChannel> input_channel_;
  std::shared_ptr<AudioChannels> audio_channels_;
  std::shared_ptr<MicrophoneChannel> microphone_channel_;
  std::shared_ptr<SensorChannel> sensor_channel_;
  std::shared_ptr<MetadataChannels> metadata_channels_;

  // What the phone was last granted, as an AudioFocusStateType. Read from io threads
  // only, but atomic so that exposing it to the host app while a session is running
  // stays a matter of adding a getter.
  std::atomic<int32_t> audio_focus_{0};

  std::vector<std::string> opened_channels_;

  // Serialises building the session against tearing it down.
  //
  // Start() runs on an io_context thread, when the connector hands over a phone that
  // has reached accessory mode. Stop() and Shutdown() arrive on Flutter's platform
  // thread, from aa_session_stop. Without this, a stop pressed a moment after a start
  // resets messenger_ between two of the lines in Start() that hand it to a channel,
  // and that channel dereferences a null messenger on its first receive: a segfault in
  // InputSourceService::receive that takes the whole app down.
  //
  // Stop() sets stopped_ before it waits for this, so a Start() already in flight
  // finishes and is then torn down whole rather than being taken apart halfway.
  std::mutex lifecycle_mutex_;
  std::atomic<bool> stopped_{false};
  // Set the moment the owner asks for a shutdown, which is before the goodbye goes out
  // and a second or more before Stop() runs. Everything this connection reports from
  // then on is its own teardown, see ReportState.
  std::atomic<bool> shutting_down_{false};

  // Fault injection only, see ArmFaultInjection.
  boost::asio::steady_timer fault_timer_;

  std::mutex shutdown_mutex_;
  std::condition_variable shutdown_cv_;
  bool shutdown_acknowledged_ = false;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_PROTOCOL_SESSION_H_
