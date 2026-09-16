#include "metadata_channel.h"

#include <aap_protobuf/service/control/ControlMessageType.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenRequest.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/shared/MessageStatus.pb.h>

#include <aasdk/Common/Log.hpp>

namespace aa {

namespace control_pb = aap_protobuf::service::control::message;

std::shared_ptr<MetadataChannel> MetadataChannel::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger,
    aasdk::messenger::ChannelId channel_id, std::string name, OpenHandler on_open,
    MessageHandler on_message, LogHandler log) {
  return std::make_shared<MetadataChannel>(io_context, strand, std::move(messenger),
                                           channel_id, std::move(name),
                                           std::move(on_open), std::move(on_message),
                                           std::move(log));
}

MetadataChannel::MetadataChannel(boost::asio::io_context& io_context,
                                 aasdk::Strand& strand,
                                 aasdk::messenger::IMessenger::Pointer messenger,
                                 aasdk::messenger::ChannelId channel_id,
                                 std::string name, OpenHandler on_open,
                                 MessageHandler on_message, LogHandler log)
    : Channel(strand, std::move(messenger), channel_id),
      io_context_(io_context),
      name_(std::move(name)),
      on_open_(std::move(on_open)),
      on_message_(std::move(on_message)),
      log_(std::move(log)) {}

void MetadataChannel::Start() { Listen(); }

void MetadataChannel::Stop() {
  // No channel pointer to drop and no mutex to take, unlike the channels that own an
  // aasdk service object: this *is* the service object, and the receive promise holds
  // a reference to it, so it cannot go out from under a handler. The flag is all that
  // is needed, and everything reads it before touching the messenger.
  stopped_.store(true);
}

void MetadataChannel::Listen() {
  if (stopped_.load()) {
    return;
  }
  auto receive = aasdk::messenger::ReceivePromise::defer(strand_);
  // shared_from_this, the way every aasdk channel arms its own receive. The cycle
  // warning in CLAUDE.md is about handing a channel an event handler that owns it; the
  // decoder this ends up in holds a weak reference for exactly that reason.
  auto self = shared_from_this();
  receive->then([self](aasdk::messenger::Message::Pointer message) {
                  self->OnMessage(std::move(message));
                },
                [self](const aasdk::error::Error& error) { self->OnError(error); });
  messenger_->enqueueReceive(channelId_, std::move(receive));
}

void MetadataChannel::OnMessage(aasdk::messenger::Message::Pointer message) {
  if (stopped_.load() || !message) {
    return;
  }
  const aasdk::messenger::MessageId message_id(message->getPayload());
  const aasdk::common::DataConstBuffer payload(message->getPayload(),
                                               aasdk::messenger::MessageId::getSizeOf());

  if (message_id.getId() == control_pb::ControlMessageType::MESSAGE_CHANNEL_OPEN_REQUEST) {
    control_pb::ChannelOpenRequest request;
    if (request.ParseFromArray(payload.cdata, payload.size)) {
      SendOpenResponse();
      if (on_open_) {
        on_open_();
      }
    } else {
      Log("The phone sent a channel open request on " + name_ + " that did not parse.");
    }
  } else if (on_message_) {
    on_message_(message_id.getId(), payload);
  }

  // Re-armed after the handler rather than before it, so a decoder that sends a reply
  // has already queued it by the time the next message can arrive. The messenger
  // buffers anything that lands with no receive outstanding, so nothing is lost either
  // way; this only keeps the ordering obvious.
  Listen();
}

void MetadataChannel::OnError(const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  // Already stopped: the link coming down is what killed this receive, and whoever
  // stopped the channel knows more about why than this does. See the same check in
  // VideoChannel::onChannelError.
  if (stopped_.load()) {
    return;
  }
  Log("The " + name_ + " channel failed: " + error.what());
}

void MetadataChannel::SendOpenResponse() {
  if (stopped_.load()) {
    return;
  }
  auto message = std::make_shared<aasdk::messenger::Message>(
      channelId_, aasdk::messenger::EncryptionType::ENCRYPTED,
      aasdk::messenger::MessageType::CONTROL);
  message->insertPayload(
      aasdk::messenger::MessageId(
          control_pb::ControlMessageType::MESSAGE_CHANNEL_OPEN_RESPONSE)
          .getData());
  control_pb::ChannelOpenResponse response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  message->insertPayload(response);
  send(std::move(message), MakeSendPromise("channel open"));
}

void MetadataChannel::Send(uint16_t message_id, const google::protobuf::Message& payload,
                           const char* what) {
  if (stopped_.load()) {
    return;
  }
  auto message = std::make_shared<aasdk::messenger::Message>(
      channelId_, aasdk::messenger::EncryptionType::ENCRYPTED,
      aasdk::messenger::MessageType::SPECIFIC);
  message->insertPayload(aasdk::messenger::MessageId(message_id).getData());
  message->insertPayload(payload);
  send(std::move(message), MakeSendPromise(what));
}

aasdk::channel::SendPromise::Pointer MetadataChannel::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not a raw this: a rejection arrives on an io_context thread and the
  // connection that started the send may be long gone by then.
  std::weak_ptr<MetadataChannel> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + " on " + self->name_ + ": " +
                              error.what());
                  }
                });
  return promise;
}

void MetadataChannel::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

}  // namespace aa
