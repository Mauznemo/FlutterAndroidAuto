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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Transport/ITransport.hpp>
#include <aasdk/USB/IAOAPDevice.hpp>

#include "service_discovery.h"

namespace aa {

class ProtocolSession;

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

  // `strand` must outlive every ProtocolSession built on it. aasdk's Channel base holds
  // it by reference and posts to it from promise handlers that can run after the
  // session is gone, so a strand owned by the session is a use after free waiting to
  // happen. The owner keeps one for the life of the process.
  static std::shared_ptr<ProtocolSession> Create(boost::asio::io_context& io_context,
                                                 aasdk::Strand& strand,
                                                 HeadUnitDescription description,
                                                 StateHandler on_state);

  ProtocolSession(boost::asio::io_context& io_context, aasdk::Strand& strand,
                  HeadUnitDescription description, StateHandler on_state);
  ~ProtocolSession();

  // Begins the handshake with a device that has already reached accessory mode.
  void Start(aasdk::usb::IAOAPDevice::Pointer device);

  // Asks the phone to end the session, then lets Stop() finish the job once the reply
  // arrives (or the caller gives up waiting). Always prefer this over Stop() when the
  // link is still healthy.
  void Shutdown();

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
  void SendHandshakeStep();
  void Listen();
  void ReportState(int state, const std::string& message);
  // Sends a message and reports, but does not tear down, if it fails. Used for the
  // replies that keep the phone happy rather than the ones that gate progress.
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  HeadUnitDescription description_;
  StateHandler on_state_;

  aasdk::usb::IAOAPDevice::Pointer device_;
  aasdk::transport::ITransport::Pointer transport_;
  aasdk::messenger::ICryptor::Pointer cryptor_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  aasdk::channel::control::IControlServiceChannel::Pointer control_channel_;
  std::shared_ptr<ControlEventRelay> relay_;

  std::vector<std::string> opened_channels_;
  bool stopped_ = false;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_PROTOCOL_SESSION_H_
