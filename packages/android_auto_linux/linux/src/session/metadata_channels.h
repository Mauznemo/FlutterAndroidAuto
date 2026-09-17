// The five channels that carry what the phone is doing, rather than what it looks like.
//
// This is the point of the whole project. Every other channel gets a picture of the
// phone's screen onto a texture; these five let the host app draw its own screen from
// the same information the phone is drawing from. A native turn card, a native now
// playing bar, a call banner in the car's own typeface, all fed from here.
//
//   NAVIGATION_STATUS      the turn ahead, its distance, the lanes, the destination
//   MEDIA_PLAYBACK_STATUS  the track, the artwork, playing or paused, the position
//   PHONE_STATUS           the calls in progress and who they are with
//   GENERIC_NOTIFICATION   a message the phone wants shown, and an acknowledgement
//   MEDIA_BROWSER          the phone's media library, asked for a node at a time
//
// Four of them the phone pushes unprompted: open the channel, answer the open, and the
// messages arrive. The media browser is the exception and runs the other way round, so
// nothing arrives on it until the host app asks for a node.
//
// Everything decoded here is merged into MetadataState rather than emitted raw, because
// the phone splits one picture across several messages. See the note there.
//
// Threading: every handler runs on the channel strand, Browse() arrives on Flutter's
// platform thread, and Stop() can come from either. Hence the mutex around the channel
// array and the same Get()-a-reference-of-your-own rule the other channels follow.

#ifndef ANDROID_AUTO_LINUX_SESSION_METADATA_CHANNELS_H_
#define ANDROID_AUTO_LINUX_SESSION_METADATA_CHANNELS_H_

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Common/Data.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "../metadata/metadata_state.h"
#include "metadata_channel.h"

namespace aa {

class MetadataChannels : public std::enable_shared_from_this<MetadataChannels> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. `advertised` is the set service discovery offered, and
  // only those channels are created: a channel the phone was not told about is one it
  // will never open.
  //
  // `state` does *not* outlive the session, unlike the sensors and the audio output.
  // What the phone is playing stops being true when the phone is unplugged, so Stop()
  // clears it.
  static std::shared_ptr<MetadataChannels> Create(
      boost::asio::io_context& io_context, aasdk::Strand& strand,
      aasdk::messenger::IMessenger::Pointer messenger, MetadataMask advertised,
      std::shared_ptr<MetadataState> state, LogHandler log);

  MetadataChannels(boost::asio::io_context& io_context, aasdk::Strand& strand,
                   aasdk::messenger::IMessenger::Pointer messenger,
                   MetadataMask advertised, std::shared_ptr<MetadataState> state,
                   LogHandler log);
  ~MetadataChannels();

  // Creates the advertised channels and arms a receive on each.
  void Start();
  // Drops them and clears the state. Safe from any thread.
  void Stop();

  // Asks the phone for one node of its media library. `path` is empty for the root and
  // otherwise a path out of a previous answer; `start` is the offset into a long list.
  // Returns false when the browser channel is not open, which is the normal answer
  // whenever no phone is connected or the host app did not advertise the channel.
  //
  // The answer comes back as a Metadata::kBrowse update, not as a return value: it is
  // a round trip to the phone and everything else in this plugin that waits on it
  // reports rather than blocks.
  bool Browse(const std::string& path, int32_t start);

  // Tells the phone the user picked `path` in the browser, which is what makes it play.
  // Returns false on the same terms as Browse.
  bool BrowseSelect(const std::string& path);

 private:
  // A reference to one live channel, or nullptr once stopped. The rule
  // InputChannel::Channel() explains, for the same pair of threads.
  std::shared_ptr<MetadataChannel> Get(Metadata which) const;
  // The same, but nullptr unless the phone has actually opened the channel. What
  // anything sending unprompted has to use.
  std::shared_ptr<MetadataChannel> GetOpen(Metadata which) const;

  void OnOpen(Metadata which);
  void OnNavigation(uint16_t id, const aasdk::common::DataConstBuffer& payload);
  void OnMedia(uint16_t id, const aasdk::common::DataConstBuffer& payload);
  void OnPhone(uint16_t id, const aasdk::common::DataConstBuffer& payload);
  void OnNotification(uint16_t id, const aasdk::common::DataConstBuffer& payload);
  void OnBrowse(uint16_t id, const aasdk::common::DataConstBuffer& payload);
  // Logs a message id nothing here decodes. Debug only: the schema has ids this head
  // unit has no use for, and a phone sending one is not a fault.
  void LogUnhandled(Metadata which, uint16_t id, size_t size);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  MetadataMask advertised_;
  std::shared_ptr<MetadataState> state_;
  LogHandler log_;

  // Guards channels_ and messenger_. Held for the length of a pointer copy, never
  // across a send.
  mutable std::mutex mutex_;
  std::array<std::shared_ptr<MetadataChannel>, kMetadataCount> channels_;
  std::atomic<bool> stopped_{false};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_METADATA_CHANNELS_H_
