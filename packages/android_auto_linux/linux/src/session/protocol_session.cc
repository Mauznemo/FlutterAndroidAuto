#include "protocol_session.h"

#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Transport/USBTransport.hpp>

#include "../aa_core.h"

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;

}  // namespace

ControlEventRelay::ControlEventRelay(std::weak_ptr<ProtocolSession> session)
    : session_(std::move(session)) {}

// Each of these is the same shape: if the session is still alive, forward; if it has
// already been torn down, drop the event on the floor. A macro would hide what is going
// on in a file that exists precisely to make an ownership subtlety visible.
void ControlEventRelay::onVersionResponse(uint16_t major, uint16_t minor,
                                          aap_protobuf::shared::MessageStatus status) {
  if (auto session = session_.lock()) {
    session->onVersionResponse(major, minor, status);
  }
}

void ControlEventRelay::onHandshake(const aasdk::common::DataConstBuffer& payload) {
  if (auto session = session_.lock()) {
    session->onHandshake(payload);
  }
}

void ControlEventRelay::onServiceDiscoveryRequest(
    const control_pb::ServiceDiscoveryRequest& request) {
  if (auto session = session_.lock()) {
    session->onServiceDiscoveryRequest(request);
  }
}

void ControlEventRelay::onAudioFocusRequest(const control_pb::AudioFocusRequest& request) {
  if (auto session = session_.lock()) {
    session->onAudioFocusRequest(request);
  }
}

void ControlEventRelay::onByeByeRequest(const control_pb::ByeByeRequest& request) {
  if (auto session = session_.lock()) {
    session->onByeByeRequest(request);
  }
}

void ControlEventRelay::onByeByeResponse(const control_pb::ByeByeResponse& response) {
  if (auto session = session_.lock()) {
    session->onByeByeResponse(response);
  }
}

void ControlEventRelay::onBatteryStatusNotification(
    const control_pb::BatteryStatusNotification& notification) {
  if (auto session = session_.lock()) {
    session->onBatteryStatusNotification(notification);
  }
}

void ControlEventRelay::onNavigationFocusRequest(
    const control_pb::NavFocusRequestNotification& request) {
  if (auto session = session_.lock()) {
    session->onNavigationFocusRequest(request);
  }
}

void ControlEventRelay::onVoiceSessionRequest(
    const control_pb::VoiceSessionNotification& request) {
  if (auto session = session_.lock()) {
    session->onVoiceSessionRequest(request);
  }
}

void ControlEventRelay::onPingRequest(const control_pb::PingRequest& request) {
  if (auto session = session_.lock()) {
    session->onPingRequest(request);
  }
}

void ControlEventRelay::onPingResponse(const control_pb::PingResponse& response) {
  if (auto session = session_.lock()) {
    session->onPingResponse(response);
  }
}

void ControlEventRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto session = session_.lock()) {
    session->onChannelError(error);
  }
}

std::shared_ptr<ProtocolSession> ProtocolSession::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    HeadUnitDescription description, StateHandler on_state) {
  return std::make_shared<ProtocolSession>(io_context, strand, std::move(description),
                                           std::move(on_state));
}

ProtocolSession::ProtocolSession(boost::asio::io_context& io_context,
                                 aasdk::Strand& strand, HeadUnitDescription description,
                                 StateHandler on_state)
    : io_context_(io_context),
      strand_(strand),
      description_(std::move(description)),
      on_state_(std::move(on_state)) {}

ProtocolSession::~ProtocolSession() { Stop(); }

void ProtocolSession::Start(aasdk::usb::IAOAPDevice::Pointer device) {
  device_ = std::move(device);

  transport_ = std::make_shared<aasdk::transport::USBTransport>(io_context_, device_);

  auto ssl_wrapper = std::make_shared<aasdk::transport::SSLWrapper>();
  cryptor_ = std::make_shared<aasdk::messenger::Cryptor>(std::move(ssl_wrapper));
  cryptor_->init();

  auto in_stream =
      std::make_shared<aasdk::messenger::MessageInStream>(io_context_, transport_, cryptor_);
  auto out_stream = std::make_shared<aasdk::messenger::MessageOutStream>(
      io_context_, transport_, cryptor_);
  messenger_ = std::make_shared<aasdk::messenger::Messenger>(
      io_context_, std::move(in_stream), std::move(out_stream));

  control_channel_ =
      std::make_shared<aasdk::channel::control::ControlServiceChannel>(strand_, messenger_);
  relay_ = std::make_shared<ControlEventRelay>(weak_from_this());

  ReportState(AA_STATE_HANDSHAKING, "Phone found, negotiating protocol version.");

  Listen();
  control_channel_->sendVersionRequest(MakeSendPromise("version request"));
}

void ProtocolSession::Listen() {
  if (stopped_) {
    return;
  }
  control_channel_->receive(relay_);
}

void ProtocolSession::Shutdown() {
  if (stopped_ || !control_channel_) {
    Stop();
    return;
  }
  // Tell the phone we are going before pulling the link out from under it. Without this
  // the phone keeps its side of the accessory session open, and the USB interface stays
  // claimed on its end for long enough that the next connection attempt fails.
  {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    shutdown_acknowledged_ = false;
  }
  control_pb::ByeByeRequest request;
  request.set_reason(control_pb::USER_SELECTION);
  control_channel_->sendShutdownRequest(request, MakeSendPromise("shutdown request"));
  // Make sure a receive is outstanding, otherwise the acknowledgement has nowhere to
  // land and we would wait out the whole timeout for a reply that did arrive.
  Listen();
}

bool ProtocolSession::WaitForShutdown(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(shutdown_mutex_);
  return shutdown_cv_.wait_for(lock, timeout, [this] { return shutdown_acknowledged_; });
}

void ProtocolSession::NoteShutdownAcknowledged() {
  {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    shutdown_acknowledged_ = true;
  }
  shutdown_cv_.notify_all();
}

void ProtocolSession::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;

  if (messenger_) {
    messenger_->stop();
  }
  if (transport_) {
    transport_->stop();
  }
  if (cryptor_) {
    cryptor_->deinit();
  }

  // Drop everything that reaches the USB device. The device's destructor is what
  // releases the USB interface, and until that runs the next connection attempt fails
  // with LIBUSB_ERROR_BUSY.
  control_channel_.reset();
  messenger_.reset();
  transport_.reset();
  device_.reset();
}

aasdk::channel::SendPromise::Pointer ProtocolSession::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`. A send can be rejected long after the session that started it has
  // gone, and aasdk delivers that rejection on an io_context thread with no idea that
  // the handler's owner is dead.
  std::weak_ptr<ProtocolSession> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->ReportState(AA_STATE_ERROR,
                                      "Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

void ProtocolSession::ReportState(int state, const std::string& message) {
  if (on_state_) {
    on_state_(state, message);
  }
}

void ProtocolSession::onVersionResponse(uint16_t major, uint16_t minor,
                                        aap_protobuf::shared::MessageStatus status) {
  if (status == aap_protobuf::shared::STATUS_NO_COMPATIBLE_VERSION) {
    ReportState(AA_STATE_ERROR,
                "The phone and this head unit have no protocol version in common.");
    Stop();
    return;
  }

  ReportState(AA_STATE_HANDSHAKING,
              "Protocol version " + std::to_string(major) + "." + std::to_string(minor) +
                  " agreed, starting the SSL handshake.");

  // The TLS session is tunnelled inside the protocol's own framing rather than sitting
  // underneath it, so the handshake is driven by hand: ask OpenSSL for the next chunk,
  // ship it over the control channel, feed it whatever comes back, repeat.
  try {
    cryptor_->doHandshake();
  } catch (const aasdk::error::Error& error) {
    ReportState(AA_STATE_ERROR, std::string("SSL handshake failed: ") + error.what());
    Stop();
    return;
  }
  SendHandshakeStep();
  Listen();
}

void ProtocolSession::SendHandshakeStep() {
  control_channel_->sendHandshake(cryptor_->readHandshakeBuffer(),
                                  MakeSendPromise("SSL handshake"));
}

void ProtocolSession::onHandshake(const aasdk::common::DataConstBuffer& payload) {
  cryptor_->writeHandshakeBuffer(payload);

  bool complete = false;
  try {
    complete = cryptor_->doHandshake();
  } catch (const aasdk::error::Error& error) {
    ReportState(AA_STATE_ERROR, std::string("SSL handshake failed: ") + error.what());
    Stop();
    return;
  }

  if (!complete) {
    // Another round trip to go.
    SendHandshakeStep();
  } else {
    ReportState(AA_STATE_HANDSHAKING, "SSL established, authenticating.");
    control_pb::AuthResponse response;
    response.set_status(0);
    control_channel_->sendAuthComplete(response, MakeSendPromise("auth complete"));
  }
  Listen();
}

void ProtocolSession::onServiceDiscoveryRequest(
    const control_pb::ServiceDiscoveryRequest& request) {
  ReportState(AA_STATE_HANDSHAKING,
              "Service discovery from " +
                  (request.device_name().empty() ? std::string("phone")
                                                 : request.device_name()) +
                  ", describing this head unit.");

  control_pb::ServiceDiscoveryResponse response;
  BuildServiceDiscoveryResponse(description_, &response);

  opened_channels_.clear();
  for (const auto& channel : response.channels()) {
    opened_channels_.push_back(aasdk::messenger::channelIdToString(
        static_cast<aasdk::messenger::ChannelId>(channel.id())));
  }

  control_channel_->sendServiceDiscoveryResponse(response,
                                                 MakeSendPromise("service discovery"));

  std::string channels;
  for (size_t i = 0; i < opened_channels_.size(); ++i) {
    channels += (i == 0 ? "" : ", ") + opened_channels_[i];
  }
  ReportState(AA_STATE_CONNECTED, "Projecting. Channels advertised: " + channels);
  Listen();
}

void ProtocolSession::onAudioFocusRequest(const control_pb::AudioFocusRequest& request) {
  // Audio focus is a state machine, not a boolean, and M6 gives it a real one. Until
  // then, granting what is asked for keeps the phone from tearing the session down.
  control_pb::AudioFocusNotification response;
  response.set_focus_state(
      request.audio_focus_type() == control_pb::AUDIO_FOCUS_RELEASE
          ? control_pb::AUDIO_FOCUS_STATE_LOSS
          : control_pb::AUDIO_FOCUS_STATE_GAIN);
  control_channel_->sendAudioFocusResponse(response, MakeSendPromise("audio focus"));
  Listen();
}

void ProtocolSession::onNavigationFocusRequest(
    const control_pb::NavFocusRequestNotification& request) {
  control_pb::NavFocusNotification response;
  response.set_focus_type(control_pb::NAV_FOCUS_PROJECTED);
  control_channel_->sendNavigationFocusResponse(response,
                                                MakeSendPromise("navigation focus"));
  Listen();
}

void ProtocolSession::onPingRequest(const control_pb::PingRequest& request) {
  control_pb::PingResponse response;
  response.set_timestamp(request.timestamp());
  control_channel_->sendPingResponse(response, MakeSendPromise("ping response"));
  Listen();
}

void ProtocolSession::onPingResponse(const control_pb::PingResponse& response) {
  Listen();
}

void ProtocolSession::onByeByeRequest(const control_pb::ByeByeRequest& request) {
  ReportState(AA_STATE_IDLE, "The phone ended the session.");
  control_pb::ByeByeResponse response;
  control_channel_->sendShutdownResponse(response, MakeSendPromise("shutdown response"));
  NoteShutdownAcknowledged();
}

void ProtocolSession::onByeByeResponse(const control_pb::ByeByeResponse& response) {
  // The phone has closed its side. This is what releases the USB interface, so the
  // caller waiting in WaitForShutdown can stop waiting.
  ReportState(AA_STATE_IDLE, "The phone closed its side of the session.");
  NoteShutdownAcknowledged();
}

void ProtocolSession::onBatteryStatusNotification(
    const control_pb::BatteryStatusNotification& notification) {
  Listen();
}

void ProtocolSession::onVoiceSessionRequest(
    const control_pb::VoiceSessionNotification& request) {
  Listen();
}

void ProtocolSession::onChannelError(const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  ReportState(AA_STATE_ERROR, std::string("Control channel error: ") + error.what());
  Stop();
}

}  // namespace aa
