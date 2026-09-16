#include "microphone_channel.h"

#include <algorithm>
#include <utility>

#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>

#include <aasdk/Common/Log.hpp>

#include "../video/video_decoder.h"

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace media_pb = aap_protobuf::service::media::shared::message;
namespace source_pb = aap_protobuf::service::media::source::message;

// How many buffers the head unit tells the phone it will keep in flight. Part of the
// setup response the phone waits for, and the same number every other implementation
// sends.
constexpr uint32_t kSetupMaxUnacked = 10;

}  // namespace

MicrophoneRelay::MicrophoneRelay(std::weak_ptr<MicrophoneChannel> owner)
    : owner_(std::move(owner)) {}

void MicrophoneRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto owner = owner_.lock()) {
    owner->OnOpen();
  }
}

void MicrophoneRelay::onMediaChannelSetupRequest(const media_pb::Setup&) {
  if (auto owner = owner_.lock()) {
    owner->OnSetup();
  }
}

void MicrophoneRelay::onMediaSourceOpenRequest(const source_pb::MicrophoneRequest& request) {
  if (auto owner = owner_.lock()) {
    owner->OnRequest(request);
  }
}

void MicrophoneRelay::onMediaChannelAckIndication(const source_pb::Ack& indication) {
  if (auto owner = owner_.lock()) {
    owner->OnAck(indication);
  }
}

void MicrophoneRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto owner = owner_.lock()) {
    owner->OnChannelError(error);
  }
}

std::shared_ptr<MicrophoneChannel> MicrophoneChannel::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger, std::shared_ptr<AudioInput> input,
    LogHandler log) {
  return std::make_shared<MicrophoneChannel>(io_context, strand, std::move(messenger),
                                             std::move(input), std::move(log));
}

MicrophoneChannel::MicrophoneChannel(boost::asio::io_context& io_context,
                                     aasdk::Strand& strand,
                                     aasdk::messenger::IMessenger::Pointer messenger,
                                     std::shared_ptr<AudioInput> input, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      input_(std::move(input)),
      log_(std::move(log)) {}

MicrophoneChannel::~MicrophoneChannel() { Stop(); }

void MicrophoneChannel::Start() {
  relay_ = std::make_shared<MicrophoneRelay>(weak_from_this());
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    // No messenger means the session was torn down while it was being built. A channel
    // made now would hold a null messenger and segfault on its first receive.
    if (stopped_.load() || !messenger_) {
      return;
    }
    channel_ =
        std::make_shared<aasdk::channel::mediasource::audio::MicrophoneAudioChannel>(
            strand_, messenger_);
  }
  Listen();
}

void MicrophoneChannel::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  // Before anything else. Whatever is wrong with the link, a head unit that keeps the
  // microphone open after the session has ended is the one failure here that a person
  // would be right to be angry about.
  if (input_ && capturing_.exchange(false)) {
    input_->Stop();
  }
  // Moved out and destroyed after the lock is dropped, so a send holding its own
  // reference on another thread finishes against a live object.
  aasdk::channel::mediasource::IMediaSourceService::Pointer channel;
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    channel = std::move(channel_);
    messenger_.reset();
  }
}

aasdk::channel::mediasource::IMediaSourceService::Pointer MicrophoneChannel::Channel()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channel_mutex_);
  return channel_;
}

void MicrophoneChannel::Listen() {
  if (auto channel = Channel()) {
    channel->receive(relay_);
  }
}

void MicrophoneChannel::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

aasdk::channel::SendPromise::Pointer MicrophoneChannel::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`: a rejection can arrive on an io_context thread long after the
  // connection that started it has gone.
  std::weak_ptr<MicrophoneChannel> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

aasdk::channel::SendPromise::Pointer MicrophoneChannel::MakeBufferPromise() {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  std::weak_ptr<MicrophoneChannel> weak = weak_from_this();
  auto release = [weak]() {
    if (auto self = weak.lock()) {
      --self->in_flight_;
    }
  };
  promise->then(release, [release](const aasdk::error::Error&) {
    // Not logged. A microphone buffer fails to send because the link is going away, and
    // at thirty buffers a second the log would be the thing that drowned out why.
    release();
  });
  return promise;
}

void MicrophoneChannel::OnOpen() {
  if (auto channel = Channel()) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    channel->sendChannelOpenResponse(response,
                                     MakeSendPromise("microphone channel open"));
    // Worth saying: the channel opening and the microphone opening are different
    // events, minutes apart, and only the second one is the machine listening.
    Log("The phone opened the microphone channel. Nothing is captured until it asks.");
  }
  Listen();
}

void MicrophoneChannel::OnSetup() {
  if (auto channel = Channel()) {
    media_pb::Config response;
    response.set_status(media_pb::Config::STATUS_READY);
    response.set_max_unacked(kSetupMaxUnacked);
    // Index into the audio_config service discovery sent for this source. There is one,
    // which is why the phone never has to be told the rate again: it is the rate the
    // head unit asked for, and MicrophoneFormat() knows it.
    response.add_configuration_indices(0);
    channel->sendChannelSetupResponse(response, MakeSendPromise("microphone setup"));
  }
  Listen();
}

void MicrophoneChannel::OnRequest(const source_pb::MicrophoneRequest& request) {
  auto channel = Channel();
  if (!channel) {
    Listen();
    return;
  }

  const bool open = request.open();
  // The phone asking the head unit to do its own noise and echo cancellation. Neither is
  // implemented, and neither is a promise: the fields are hints about what the phone
  // would like the hardware to have done. Logged because a poor recognition rate in a
  // noisy car is the first thing they would explain.
  AASDK_LOG(debug) << "[Microphone] request open=" << open
                   << " anc=" << request.anc_enabled() << " ec=" << request.ec_enabled()
                   << " max_unacked=" << request.max_unacked();

  if (open) {
    max_unacked_ = request.max_unacked() > 0 ? request.max_unacked() : kQueueBound;
    in_flight_ = 0;
    dropped_ = 0;
    acked_ = 0;
    reported_drops_ = false;
  }

  // Answered before capture starts, so the phone has its session id before the first
  // buffer quoting it arrives.
  source_pb::MicrophoneResponse response;
  response.set_status(0);
  response.set_session_id(open ? ++session_id_ : session_id_.load());
  channel->sendMicrophoneOpenResponse(response,
                                      MakeSendPromise("microphone open response"));

  if (input_) {
    if (open) {
      if (!capturing_.exchange(true)) {
        std::weak_ptr<MicrophoneChannel> weak = weak_from_this();
        input_->Start(MicrophoneFormat(),
                      [weak](const uint8_t* data, size_t size) {
                        if (auto self = weak.lock()) {
                          self->Publish(data, size);
                        }
                      });
        Log("The phone opened the microphone. This machine is listening.");
      }
    } else if (capturing_.exchange(false)) {
      input_->Stop();
      Log("The phone closed the microphone. This machine has stopped listening. " +
          std::to_string(acked_.load()) + " buffer(s) acknowledged, " +
          std::to_string(dropped_.load()) + " dropped.");
    }
  }
  Listen();
}

void MicrophoneChannel::OnAck(const source_pb::Ack& indication) {
  // Counted for two reasons. It is how "did the phone hear us" gets an answer without a
  // packet capture, and the count is what decides whether the phone's declared window is
  // worth honouring: see Window().
  if (indication.session_id() == session_id_.load()) {
    acked_ += indication.has_ack() ? std::max<uint32_t>(1, indication.ack()) : 1;
  }
  Listen();
}

int32_t MicrophoneChannel::Window() const {
  return acked_.load() > 0 ? max_unacked_.load() : kQueueBound;
}

void MicrophoneChannel::Publish(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || stopped_.load()) {
    return;
  }
  if (in_flight_.load() >= Window()) {
    // The link is not keeping up. Speech cannot be queued and sent late, so the buffer
    // goes rather than the head unit handing the Assistant a sentence out of the past.
    ++dropped_;
    if (!reported_drops_.exchange(true)) {
      Log("The link cannot keep up with the microphone, so buffers are being dropped.");
    }
    return;
  }
  ++in_flight_;

  // Copied on the capture thread and moved through the strand. The capture thread owns
  // the buffer it was given and reuses it for the next read, so it cannot outlive this
  // call; the strand is where everything else that touches the channel runs.
  aasdk::common::Data buffer(data, data + size);
  std::weak_ptr<MicrophoneChannel> weak = weak_from_this();
  strand_.post([weak, buffer = std::move(buffer)]() mutable {
    if (auto self = weak.lock()) {
      self->Deliver(std::move(buffer));
    }
  });
}

void MicrophoneChannel::Deliver(aasdk::common::Data data) {
  auto channel = Channel();
  if (!channel || !capturing_.load()) {
    // Stopped between the capture thread posting this and the strand reaching it. The
    // slot has to go back either way, otherwise the window closes for good.
    --in_flight_;
    return;
  }
  // Stamped here rather than at capture, because the phone reads it as when the audio
  // was put on the wire and this is the last moment before that is true.
  channel->sendMediaSourceWithTimestampIndication(
      static_cast<uint64_t>(NowMicros()), data, MakeBufferPromise());
}

void MicrophoneChannel::OnChannelError(const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  // Already stopped: see the note on VideoChannel::onChannelError.
  if (stopped_.load()) {
    return;
  }
  Log(std::string("The microphone channel failed: ") + error.what());
  // Stops the capture as well as the channel. A link that has failed is not one the
  // phone can use to close the microphone, so nothing else would.
  Stop();
}

}  // namespace aa
