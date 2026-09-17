// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol_session.h"

#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Transport/USBTransport.hpp>

#include <cstdlib>
#include <thread>

#include <libusb.h>

#include <aasdk/Common/Log.hpp>

#include "../aa_core.h"
#include "../audio/audio_input.h"
#include "../audio/audio_output.h"
#include "audio_channels.h"
#include "input_channel.h"
#include "metadata_channels.h"
#include "microphone_channel.h"
#include "sensor_channel.h"
#include "video_channel.h"

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;

// Widens the window inside Start() between the messenger existing and the channels
// being handed it, so that a stop pressed during it lands there every time.
//
// That window is microseconds wide in normal running, which is why the crash it used to
// cause took a person pressing the buttons by hand to find once and could not be
// reproduced by a script at all. Everything a stop touches is torn down in it.
//
//   AA_FAULT_SLOW_START=<milliseconds>
//
// Compiled in only when AA_ENABLE_FAULT_INJECTION is on, which a debug build does by
// default and a release build does not. Unset, nothing stalls and this costs one getenv
// per connection.
void StallStart() {
#ifdef AA_FAULT_INJECTION
  const char* after = std::getenv("AA_FAULT_SLOW_START");
  if (after == nullptr || *after == '\0') {
    return;
  }
  const int milliseconds = std::atoi(after);
  if (milliseconds <= 0) {
    return;
  }
  AASDK_LOG(info) << "[Fault] stalling inside Start() for " << milliseconds << " ms";
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
}

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
    HeadUnitDescription description, std::shared_ptr<VideoDecoder> decoder,
    std::shared_ptr<AudioOutput> audio, std::shared_ptr<AudioInput> microphone,
    std::shared_ptr<SensorState> sensors, std::shared_ptr<MetadataState> metadata,
    StateHandler on_state, InputHandler on_input, MetadataHandler on_metadata) {
  return std::make_shared<ProtocolSession>(
      io_context, strand, std::move(description), std::move(decoder), std::move(audio),
      std::move(microphone), std::move(sensors), std::move(metadata),
      std::move(on_state), std::move(on_input), std::move(on_metadata));
}

ProtocolSession::ProtocolSession(boost::asio::io_context& io_context,
                                 aasdk::Strand& strand, HeadUnitDescription description,
                                 std::shared_ptr<VideoDecoder> decoder,
                                 std::shared_ptr<AudioOutput> audio,
                                 std::shared_ptr<AudioInput> microphone,
                                 std::shared_ptr<SensorState> sensors,
                                 std::shared_ptr<MetadataState> metadata,
                                 StateHandler on_state, InputHandler on_input,
                                 MetadataHandler on_metadata)
    : io_context_(io_context),
      strand_(strand),
      description_(std::move(description)),
      on_state_(std::move(on_state)),
      on_input_(std::move(on_input)),
      on_metadata_(std::move(on_metadata)),
      decoder_(std::move(decoder)),
      audio_(std::move(audio)),
      microphone_(std::move(microphone)),
      sensors_(std::move(sensors)),
      metadata_(std::move(metadata)),
      fault_timer_(io_context) {}

ProtocolSession::~ProtocolSession() { Stop(); }

void ProtocolSession::Start(aasdk::usb::IAOAPDevice::Pointer device) {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (stopped_.load()) {
    // Stopped before this ever ran, which is what pressing start and then stop quickly
    // looks like from in here. Building the connection now would leave one that nothing
    // is going to tear down.
    return;
  }
  device_ = std::move(device);
  StartLocked(std::make_shared<aasdk::transport::USBTransport>(io_context_, device_));
}

void ProtocolSession::Start(aasdk::transport::ITransport::Pointer transport) {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (stopped_.load()) {
    return;
  }
  StartLocked(std::move(transport));
}

void ProtocolSession::StartLocked(aasdk::transport::ITransport::Pointer transport) {
  transport_ = std::move(transport);

  auto ssl_wrapper = std::make_shared<aasdk::transport::SSLWrapper>();
  cryptor_ = std::make_shared<aasdk::messenger::Cryptor>(std::move(ssl_wrapper));
  cryptor_->init();

  auto in_stream =
      std::make_shared<aasdk::messenger::MessageInStream>(io_context_, transport_, cryptor_);
  auto out_stream = std::make_shared<aasdk::messenger::MessageOutStream>(
      io_context_, transport_, cryptor_);
  messenger_ = std::make_shared<aasdk::messenger::Messenger>(
      io_context_, std::move(in_stream), std::move(out_stream));

  // Fault injection only, and placed here deliberately: this is the exact point where a
  // concurrent stop used to reset messenger_ out from under the lines below.
  StallStart();

  {
    std::lock_guard<std::mutex> control_lock(control_mutex_);
    control_channel_ = std::make_shared<aasdk::channel::control::ControlServiceChannel>(
        strand_, messenger_);
  }
  relay_ = std::make_shared<ControlEventRelay>(weak_from_this());

  // Armed now rather than after service discovery. The messenger buffers a message that
  // arrives for a channel with no outstanding receive, so either order works, but the
  // phone opens the video channel the instant it has our response and there is nothing
  // to gain by being late.
  if (description_.enable_video && decoder_) {
    video_channel_ = VideoChannel::Create(
        io_context_, strand_, messenger_, decoder_,
        [weak = weak_from_this()](const std::string& message) {
          if (auto self = weak.lock()) {
            self->ReportState(AA_STATE_CONNECTED, message);
          }
        });
    video_channel_->Start();
  }

  // Armed for the same reason as video, and it matters more here: the phone sends its
  // key binding request the moment the channel opens, and a head unit that does not
  // answer it is one that advertised a channel and then ignored it.
  if (description_.enable_input) {
    input_channel_ = InputChannel::Create(
        io_context_, strand_, messenger_, description_.width, description_.height,
        [weak = weak_from_this()](const std::string& message) {
          if (auto self = weak.lock()) {
            self->ReportState(AA_STATE_CONNECTED, message);
          }
        });
    input_channel_->Start();
    if (on_input_) {
      on_input_(input_channel_);
    }
  }

  // The three PCM sinks. Armed here for the same reason as video: the phone opens them
  // the instant it has the service discovery response.
  if (description_.enable_media_audio || description_.enable_system_audio ||
      description_.enable_speech_audio) {
    audio_channels_ = AudioChannels::Create(
        io_context_, strand_, messenger_, description_, audio_,
        [weak = weak_from_this()](const std::string& message) {
          if (auto self = weak.lock()) {
            self->ReportState(AA_STATE_CONNECTED, message);
          }
        });
    audio_channels_->Start();
  }

  // The microphone. Armed here with the rest, but unlike every other channel this one
  // is armed without anything being opened: the capture device is only touched once the
  // phone actually asks to record.
  if (description_.enable_microphone && microphone_) {
    microphone_channel_ = MicrophoneChannel::Create(
        io_context_, strand_, messenger_, microphone_,
        [weak = weak_from_this()](const std::string& message) {
          if (auto self = weak.lock()) {
            self->ReportState(AA_STATE_CONNECTED, message);
          }
        });
    microphone_channel_->Start();
  }

  // The sensors. Last of the channels and the one the phone waits on hardest: it locks
  // most of its interface until the driving status subscription has been answered, so a
  // session that gets everything else right and this wrong looks like a phone that has
  // decided the car is not ready. See sensor_channel.h.
  if (description_.sensors != 0 && sensors_) {
    sensor_channel_ = SensorChannel::Create(
        io_context_, strand_, messenger_, sensors_, description_.sensors,
        [weak = weak_from_this()](const std::string& message) {
          if (auto self = weak.lock()) {
            self->ReportState(AA_STATE_CONNECTED, message);
          }
        });
    sensor_channel_->Start();
  }

  // The five metadata channels. Armed with the rest, and the only group here that is
  // read rather than answered: four of them exist to be pushed to, and the fifth does
  // nothing until the host app asks it for a node of the phone's media library.
  if (description_.metadata != 0 && metadata_) {
    metadata_channels_ = MetadataChannels::Create(
        io_context_, strand_, messenger_, description_.metadata, metadata_,
        [weak = weak_from_this()](const std::string& message) {
          if (auto self = weak.lock()) {
            self->ReportState(AA_STATE_CONNECTED, message);
          }
        });
    metadata_channels_->Start();
    if (on_metadata_) {
      on_metadata_(metadata_channels_);
    }
  }

  // A stop that landed while this was building has been waiting on the lock ever since,
  // so do not open a conversation with a phone that is about to be said goodbye to.
  // What has been built so far is torn down by the Stop() that runs next.
  if (stopped_.load()) {
    return;
  }

  ReportState(AA_STATE_HANDSHAKING, "Phone found, negotiating protocol version.");

  ArmFaultInjection();
  Listen();
  if (auto control = Control()) {
    control->sendVersionRequest(MakeSendPromise("version request"));
  }
}

void ProtocolSession::ArmFaultInjection() {
  // Kills the transport after AA_FAULT_TRANSPORT_AFTER seconds, without touching USB.
  //
  // This reproduces the failure that matters most and is hardest to wait for: a bulk
  // transfer failing while the phone stays enumerated and still in accessory mode. The
  // recovery from it cannot be triggered by unplugging anything, because unplugging is
  // the case that already worked, and in the wild it takes minutes of use to show up
  // once and then hides again.
  //
  //   AA_FAULT_TRANSPORT_AFTER=<seconds>
  //
  // Compiled in only when AA_ENABLE_FAULT_INJECTION is on, which a debug build does by
  // default and a release build does not. Unset, nothing is armed and this costs one
  // getenv per connection.
#ifndef AA_FAULT_INJECTION
  return;
#else
  const char* after = std::getenv("AA_FAULT_TRANSPORT_AFTER");
  if (after == nullptr || *after == '\0') {
    return;
  }
  const int seconds = std::atoi(after);
  if (seconds <= 0) {
    return;
  }
  AASDK_LOG(info) << "[Fault] killing the transport in " << seconds << "s";
  fault_timer_.expires_after(std::chrono::seconds(seconds));
  fault_timer_.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
    if (ec) {
      return;
    }
    if (auto self = weak.lock()) {
      // The same error a failed bulk read delivers, so the path under test is the real
      // one rather than something that merely resembles it.
      self->onChannelError(aasdk::error::Error(aasdk::error::ErrorCode::USB_TRANSFER,
                                               LIBUSB_TRANSFER_ERROR));
    }
  });
#endif
}

void ProtocolSession::Listen() {
  if (auto control = Control()) {
    control->receive(relay_);
  }
}

aasdk::channel::control::IControlServiceChannel::Pointer ProtocolSession::Control()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(control_mutex_);
  return control_channel_;
}

void ProtocolSession::Shutdown() {
  // Before the lock, not after: a Start() being waited on here can fail while it is
  // still finishing, and that failure is part of this teardown rather than news.
  shutting_down_ = true;
  // Waits out a Start() still building on an io thread, so that a stop pressed a
  // moment after a start still has a control channel to say goodbye on.
  std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
  auto control = Control();
  if (stopped_.load() || !control) {
    // Nothing to say goodbye with. Stop() takes the same lock, so let go of it first.
    lifecycle.unlock();
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
  control->sendShutdownRequest(request, MakeSendPromise("shutdown request"));
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
  if (stopped_.exchange(true)) {
    return;
  }
  // Waits out a Start() still building on an io thread. Tearing a session down while it
  // is half built is what left a channel holding a null messenger.
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  fault_timer_.cancel();

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
  if (video_channel_) {
    video_channel_->Stop();
    video_channel_.reset();
  }
  // The owner hears first: its send calls arrive on Flutter's platform thread, and the
  // point of telling it is to stop them reaching a channel that is about to lose its
  // messenger. A report that slips through anyway is dropped by the stopped_ check.
  if (on_input_) {
    on_input_(nullptr);
  }
  if (input_channel_) {
    input_channel_->Stop();
    input_channel_.reset();
  }
  if (audio_channels_) {
    audio_channels_->Stop();
    audio_channels_.reset();
  }
  // Stops the capture as well as the channel, which is why it is not left to the
  // destructor: the link is already gone, so nothing else is ever going to close the
  // microphone.
  if (microphone_channel_) {
    microphone_channel_->Stop();
    microphone_channel_.reset();
  }
  if (sensor_channel_) {
    sensor_channel_->Stop();
    sensor_channel_.reset();
  }
  // The owner hears first, for the same reason it does about the input channel: a
  // browse request arriving from Flutter's platform thread must not reach a channel
  // that is losing its messenger. Stopping also clears what the phone had told this
  // head unit, which is the point: the music stopped when the cable came out.
  if (on_metadata_) {
    on_metadata_(nullptr);
  }
  if (metadata_channels_) {
    metadata_channels_->Stop();
    metadata_channels_.reset();
  }
  // The decoder is not stopped here. It belongs to the head unit rather than to this
  // connection, and it is flushed rather than torn down so a reconnect does not pay for
  // opening VA-API again.
  if (decoder_) {
    decoder_->Flush();
  }
  {
    std::lock_guard<std::mutex> control_lock(control_mutex_);
    control_channel_.reset();
  }
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
  // Once the owner has asked this connection to go away, nothing it has left to say is
  // news. Its channels fail one after another as the link comes down, and reporting
  // those as errors made aa_core.cc treat a deliberate stop as a phone that had gone
  // wrong: it bounced the device and re-armed discovery in the middle of the user's
  // stop, which is how a stop ended with the phone still showing Android Auto. A
  // connected report is just as wrong here, since it puts the projection back up after
  // it has ended. Everything else still goes out, including the goodbye itself.
  if ((stopped_.load() || shutting_down_.load()) &&
      (state == AA_STATE_ERROR || state == AA_STATE_CONNECTED)) {
    return;
  }
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
  if (auto control = Control()) {
    control->sendHandshake(cryptor_->readHandshakeBuffer(),
                           MakeSendPromise("SSL handshake"));
  }
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
    if (auto control = Control()) {
      control->sendAuthComplete(response, MakeSendPromise("auth complete"));
    }
  }
  Listen();
}

void ProtocolSession::onServiceDiscoveryRequest(
    const control_pb::ServiceDiscoveryRequest& request) {
  // The icons in the request are tens of kilobytes of PNG and would bury everything
  // else, so only the fields worth reading are logged.
  AASDK_LOG(debug) << "[ServiceDiscovery] request from " << request.device_name() << " ("
                   << request.label_text()
                   << "), phone info: " << request.phone_info().ShortDebugString();
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

  if (auto control = Control()) {
    control->sendServiceDiscoveryResponse(response, MakeSendPromise("service discovery"));
  }

  std::string channels;
  for (size_t i = 0; i < opened_channels_.size(); ++i) {
    channels += (i == 0 ? "" : ", ") + opened_channels_[i];
  }
  ReportState(AA_STATE_CONNECTED, "Projecting. Channels advertised: " + channels);
  Listen();
}

void ProtocolSession::onAudioFocusRequest(const control_pb::AudioFocusRequest& request) {
  // The phone asking the head unit for permission to make noise, and the head unit
  // answering with what it granted rather than with what was asked for. There is
  // nothing else in this head unit competing for the speakers yet, so everything is
  // granted; what matters is granting the *right* state, because the phone changes its
  // own behaviour according to the answer.
  //
  // Deliberately not wired to the ducking. A phone asks for GAIN_TRANSIENT_MAY_DUCK
  // before its own media as well as before a navigation prompt, so ducking on the
  // request would have the media stream duck against itself. Ducking is driven by the
  // speech stream actually carrying audio, in AudioOutput, where the question has an
  // unambiguous answer.
  control_pb::AudioFocusStateType granted = control_pb::AUDIO_FOCUS_STATE_GAIN;
  switch (request.audio_focus_type()) {
    case control_pb::AUDIO_FOCUS_RELEASE:
      granted = control_pb::AUDIO_FOCUS_STATE_LOSS;
      break;
    case control_pb::AUDIO_FOCUS_GAIN_TRANSIENT:
    case control_pb::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
      // Transient means the phone will hand it back, and it wants to be told that this
      // is what it was given: answering plain GAIN to a transient request makes some
      // builds skip the release, leaving the head unit believing the phone still holds
      // focus it has finished with.
      granted = control_pb::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
      break;
    case control_pb::AUDIO_FOCUS_GAIN:
    default:
      granted = control_pb::AUDIO_FOCUS_STATE_GAIN;
      break;
  }
  audio_focus_ = static_cast<int32_t>(granted);

  control_pb::AudioFocusNotification response;
  response.set_focus_state(granted);
  // Solicited: this is an answer, and a phone that reads it as an unprompted change of
  // its own focus will release what it has just been granted.
  response.set_unsolicited(false);
  if (auto control = Control()) {
    control->sendAudioFocusResponse(response, MakeSendPromise("audio focus"));
  }
  AASDK_LOG(debug) << "[AudioFocus] phone asked for " << request.audio_focus_type()
                   << ", granted " << granted;
  Listen();
}

void ProtocolSession::onNavigationFocusRequest(
    const control_pb::NavFocusRequestNotification& request) {
  control_pb::NavFocusNotification response;
  response.set_focus_type(control_pb::NAV_FOCUS_PROJECTED);
  if (auto control = Control()) {
    control->sendNavigationFocusResponse(response, MakeSendPromise("navigation focus"));
  }
  Listen();
}

void ProtocolSession::onPingRequest(const control_pb::PingRequest& request) {
  control_pb::PingResponse response;
  response.set_timestamp(request.timestamp());
  if (auto control = Control()) {
    control->sendPingResponse(response, MakeSendPromise("ping response"));
  }
  Listen();
}

void ProtocolSession::onPingResponse(const control_pb::PingResponse& response) {
  Listen();
}

void ProtocolSession::onByeByeRequest(const control_pb::ByeByeRequest& request) {
  ReportState(AA_STATE_IDLE, "The phone ended the session.");
  control_pb::ByeByeResponse response;
  if (auto control = Control()) {
    control->sendShutdownResponse(response, MakeSendPromise("shutdown response"));
  }
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
  // A session that has already stopped has no news to report. Its own teardown is what
  // killed the link: aa_session_stop resets the USB device the moment Stop() returns,
  // and the read that was in flight then fails with NO_DEVICE. Reporting that made
  // every deliberate stop look like a lost cable, and the recovery in aa_core.cc
  // answered it by bouncing the phone a second time, which left the next start unable
  // to enumerate it for tens of seconds.
  if (stopped_.load() || shutting_down_.load()) {
    return;
  }
  ReportState(AA_STATE_ERROR, std::string("Control channel error: ") + error.what());
  Stop();
}

}  // namespace aa
