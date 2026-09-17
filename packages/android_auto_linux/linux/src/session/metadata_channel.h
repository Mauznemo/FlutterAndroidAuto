// A channel aasdk has a name for but no messages for.
//
// The five metadata channels are in aasdk's ChannelId list and four of them have a
// service class, but only NavigationStatus and MediaPlaybackStatus parse anything, and
// even those two stop at the message ids openauto's phones were sending in 2018: the
// turn and distance events, both marked deprecated in the schema, and not the
// navigation state a current phone actually sends. PhoneStatus, MediaBrowser and
// GenericNotification parse nothing at all beyond the channel open, and log an error
// for everything else.
//
// Extending aasdk would mean five more classes in the vendored submodule and a much
// larger patch to carry, for messages this plugin is the only consumer of. aasdk's
// Channel base is public and does everything that is actually hard (the strand, the
// send path, the framing), so instead there is one channel class here that answers the
// open request and hands every other message to a decoder, and one decoder per channel
// in metadata_channels.cc. See docs/aasdk-port-notes.md for the rule this follows:
// extend around aasdk rather than inside it.
//
// Lifetime: the receive promise binds shared_from_this, exactly as aasdk's own channels
// do, so the channel outlives the handlers that are in flight against it. What it must
// not do is keep its owner alive, so the decoder a caller supplies has to hold a weak
// reference; MetadataChannels does.

#ifndef ANDROID_AUTO_LINUX_SESSION_METADATA_CHANNEL_H_
#define ANDROID_AUTO_LINUX_SESSION_METADATA_CHANNEL_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include <google/protobuf/message.h>

#include <aasdk/Channel/Channel.hpp>
#include <aasdk/Common/Data.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

namespace aa {

class MetadataChannel : public aasdk::channel::Channel,
                        public std::enable_shared_from_this<MetadataChannel> {
 public:
  // One message from the phone: the protocol's message id and the payload with the id
  // already stripped off. Runs on the channel strand.
  using MessageHandler =
      std::function<void(uint16_t id, const aasdk::common::DataConstBuffer& payload)>;
  // The phone opened the channel. Raised after the open response has been queued, so a
  // decoder that wants to send something first thing can.
  using OpenHandler = std::function<void()>;
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. `name` is only for logging.
  static std::shared_ptr<MetadataChannel> Create(
      boost::asio::io_context& io_context, aasdk::Strand& strand,
      aasdk::messenger::IMessenger::Pointer messenger,
      aasdk::messenger::ChannelId channel_id, std::string name, OpenHandler on_open,
      MessageHandler on_message, LogHandler log);

  MetadataChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                  aasdk::messenger::IMessenger::Pointer messenger,
                  aasdk::messenger::ChannelId channel_id, std::string name,
                  OpenHandler on_open, MessageHandler on_message, LogHandler log);
  ~MetadataChannel() override = default;

  // Arms the first receive. Nothing happens until the phone opens the channel.
  void Start();
  // Stops answering. Safe from any thread, and idempotent: a receive already in flight
  // is left to be rejected by the messenger going away, and its handler sees this.
  void Stop();

  // Sends one protobuf on this channel. `what` names it in a failure message. Does
  // nothing once stopped, which is the normal answer when the phone has gone.
  void Send(uint16_t message_id, const google::protobuf::Message& payload,
            const char* what);

  const std::string& name() const { return name_; }

 private:
  void Listen();
  void OnMessage(aasdk::messenger::Message::Pointer message);
  void OnError(const aasdk::error::Error& error);
  void SendOpenResponse();
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  std::string name_;
  OpenHandler on_open_;
  MessageHandler on_message_;
  LogHandler log_;
  std::atomic<bool> stopped_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_METADATA_CHANNEL_H_
