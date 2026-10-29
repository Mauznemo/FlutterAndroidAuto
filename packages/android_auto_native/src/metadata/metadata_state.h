// SPDX-License-Identifier: GPL-3.0-or-later
// What the phone has told this head unit about itself, and nothing about the protocol.
//
// The API agnostic seam for metadata, the mirror image of sensors/sensor_state.h: no
// protobuf and no aasdk, so the host app's widgets are written against these shapes
// rather than against the wire. session/metadata_channels.cc is the only thing that
// turns wire messages into them, and the JSON at the bottom of this file is the only
// thing that turns them into something the C ABI can carry.
//
// The direction is the one thing that makes this unlike SensorState, and it changes
// two decisions:
//
//   - **This is not process lifetime.** What the car is doing outlives the cable,
//     because a parking brake does not come off when a phone is unplugged. What the
//     phone is playing does not: the music stops. Clear() runs when a connection ends,
//     so a "now playing" bar goes blank rather than showing a track that finished an
//     hour ago.
//   - **Updates are merged, not replaced.** Android Auto splits one picture across
//     several messages: the track's title arrives on MEDIA_PLAYBACK_METADATA and
//     whether it is playing on MEDIA_PLAYBACK_STATUS, a turn's shape on
//     INSTRUMENT_CLUSTER_NAVIGATION_STATE and its distance on
//     INSTRUMENT_CLUSTER_NAVIGATION_CURRENT_POSITION. A host app wants one object with
//     all of it in, so each message writes its own fields and every update publishes
//     the whole.
//
// A field the phone did not send is absent rather than zero, the same rule the sensors
// keep: a track with no album and a track on an album called "" are different
// questions. Numbers and enumerations say so with std::optional, strings by being
// empty, and both come out of the JSON as an absent key.
//
// Threading: the channels write from io_context threads, the host app reads from
// Flutter's platform thread, and the listener runs on whichever thread wrote. The mutex
// covers all of it, and is never held across the listener.

#ifndef ANDROID_AUTO_LINUX_METADATA_METADATA_STATE_H_
#define ANDROID_AUTO_LINUX_METADATA_METADATA_STATE_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aa {

// The five things the phone can tell a head unit about itself, one per channel.
// Mirrored by AaMetadata in aa_core.h and by AndroidAutoMetadata in Dart, as an index
// and as the bit `1 << index`, so the order is part of the ABI.
enum class Metadata {
  kNavigation = 0,
  kMedia = 1,
  kPhone = 2,
  kNotification = 3,
  kBrowse = 4,
};

constexpr int kMetadataCount = 5;

// A set of them, one bit each: what service discovery advertises, and what the phone
// actually opened.
using MetadataMask = uint32_t;

constexpr MetadataMask MetadataBit(Metadata which) {
  return static_cast<MetadataMask>(1u) << static_cast<int>(which);
}

// The three the phone pushes without being asked. The default advertised set, because
// they cost the head unit nothing but a channel and they are what a native "now
// playing" bar or turn card is made of.
constexpr MetadataMask kDefaultMetadata = MetadataBit(Metadata::kNavigation) |
                                          MetadataBit(Metadata::kMedia) |
                                          MetadataBit(Metadata::kPhone);

// For logging and for the name the host app sees.
const char* MetadataName(Metadata which);

// === navigation ===

// Whether the phone is guiding anyone anywhere. The one field of the navigation status
// message, and the thing to check before drawing a turn card: a stale instruction shown
// after guidance ended is worse than no card at all.
enum class NavigationStatus {
  kUnavailable,
  kActive,
  kInactive,
  kRerouting,
};

// A distance as the phone would like it shown.
//
// Both halves matter. `metres` is the number to compare against; `display_value` and
// `display_unit` are what the phone's own UI would print, already rounded and already
// converted to whatever the user's units are. A head unit that recomputes "0.4 km" from
// 412 metres will sooner or later disagree with the phone's own screen next to it.
struct NavigationDistance {
  std::optional<int32_t> metres;
  std::string display_value;
  // "meters", "kilometers", "kilometersP1", "miles", "milesP1", "feet", "yards", or
  // empty when the phone did not say. The P1 forms mean one decimal place.
  std::string display_unit;

  bool empty() const { return !metres.has_value() && display_value.empty(); }
};

// One arrow in a lane diagram.
struct NavigationLaneDirection {
  // "straight", "slightLeft", "normalRight", "uTurnLeft" and so on, or empty.
  std::string shape;
  // Whether this is a lane the driver should be in.
  bool highlighted = false;
};

// One lane of the road, with every direction it serves.
struct NavigationLane {
  std::vector<NavigationLaneDirection> directions;
};

// Where the route ends, and how far off that is.
struct NavigationDestination {
  std::string address;
  NavigationDistance distance;
  // The phone's own arrival time, already formatted and already in the phone's time
  // zone. Not a timestamp: the protocol carries a string.
  std::string eta_text;
  std::optional<int64_t> seconds_to_arrival;
};

// Everything known about the guidance in progress.
struct NavigationInfo {
  std::optional<NavigationStatus> status;
  // The shape of the next turn: "turnNormalLeft", "roundaboutEnterAndExitCw",
  // "destinationRight" and so on. Empty when the phone has not described one.
  std::string maneuver;
  std::optional<int32_t> roundabout_exit_number;
  std::optional<int32_t> roundabout_exit_angle;
  // The road the next step turns onto.
  std::string road;
  // The phone's own wording for the instruction, longest first. What a head unit shows
  // when it has room for a sentence rather than an arrow.
  std::vector<std::string> cue;
  std::vector<NavigationLane> lanes;
  // The road the car is on now, which is not the road it is turning onto.
  std::string current_road;
  NavigationDistance step_distance;
  std::optional<int64_t> seconds_to_step;
  std::vector<NavigationDestination> destinations;
  // A rendered arrow for the next turn, base64 of whatever image format the phone
  // chose. Only older phones send one; a phone on the current protocol describes the
  // maneuver instead and leaves the drawing to the head unit.
  std::string maneuver_image;
};

// === media ===

enum class PlaybackState {
  kStopped,
  kPlaying,
  kPaused,
};

// The track and what is being done with it.
struct MediaInfo {
  std::string song;
  std::string artist;
  std::string album;
  std::string playlist;
  std::optional<int64_t> duration_seconds;
  std::optional<int32_t> rating;
  // Base64 of the cover image, encoded once when it arrives.
  std::string album_art;
  std::optional<PlaybackState> state;
  // Which app is playing, as the phone names it.
  std::string source;
  std::optional<int64_t> position_seconds;
  std::optional<bool> shuffle;
  std::optional<bool> repeat;
  std::optional<bool> repeat_one;
};

// === telephony ===

// What one call is doing. The protocol's own set.
enum class CallState {
  kUnknown,
  kInCall,
  kOnHold,
  kInactive,
  kIncoming,
  kConferenced,
  kMuted,
};

// One call. There can be several: a held call and an active one, or a conference.
struct CallInfo {
  CallState state = CallState::kUnknown;
  int64_t duration_seconds = 0;
  std::string number;
  // The name from the phone's contacts, when it knows one.
  std::string caller_id;
  // "mobile", "home" and so on, as free text from the phone.
  std::string number_type;
  // Base64 of the contact's photo.
  std::string thumbnail;
};

// The phone's telephony state. Nothing here carries audio: a call's sound goes over
// Bluetooth HFP and never touches the projection link, see docs/echo-cancellation.md.
struct PhoneInfo {
  std::vector<CallInfo> calls;
  std::optional<int32_t> signal_strength;
};

// === notifications ===

// One message the phone wants shown. Unlike everything else here this is an event
// rather than a state: each one is delivered once and acknowledged.
struct NotificationInfo {
  std::string id;
  std::string text;
  // Base64 of the icon.
  std::string icon;
};

// === media browser ===

struct BrowseSong {
  std::string path;
  std::string name;
  std::string artist;
  std::string album;
  std::optional<int64_t> duration_seconds;
  std::string art;
};

struct BrowseList {
  std::string path;
  // "playlist", "album", "artist", "station", "genre", or empty.
  std::string type;
  std::string name;
  std::string art;
};

struct BrowseSource {
  std::string path;
  std::string name;
  std::string art;
};

// One answer to a browse request. Which of the collections is filled in depends on
// `kind`, because the protocol has four different reply messages rather than one.
struct BrowseNode {
  // "root", "source", "list" or "song".
  std::string kind;
  std::string path;
  std::string name;
  // For a list node: what kind of list it is.
  std::string type;
  // Which slice of a long list this is, when the phone chose to page it.
  std::optional<int32_t> start;
  std::optional<int32_t> total;
  std::vector<BrowseSource> sources;
  std::vector<BrowseList> lists;
  std::vector<BrowseSong> songs;
  // Filled in instead of `songs` when this is a single song node.
  std::optional<BrowseSong> song;
};

class MetadataState {
 public:
  // Told which kind changed and what it now looks like, on the thread that changed it.
  // Never called with the lock held.
  using Listener = std::function<void(Metadata, const std::string& json)>;

  // Each of these merges its fields into what is already known and publishes the whole.
  // `Mutate` takes the lock, hands the caller the live struct, and publishes once the
  // caller returns, which keeps "change some fields" one operation rather than a read,
  // a copy and a write with a gap in the middle.
  void MutateNavigation(const std::function<void(NavigationInfo*)>& change);
  void MutateMedia(const std::function<void(MediaInfo*)>& change);
  void MutatePhone(const std::function<void(PhoneInfo*)>& change);

  // The two that are events rather than state. Published whole, and kept only so a host
  // app that asks late sees the most recent one.
  void PublishNotification(const NotificationInfo& notification);
  void PublishBrowseNode(const BrowseNode& node);

  // The latest of one kind as JSON, or an empty string if nothing has arrived. For a
  // host app that starts listening after the phone has already said something.
  std::string Snapshot(Metadata which) const;

  // Forgets everything. Called when a connection ends: what the phone was playing stops
  // being true the moment it is unplugged.
  void Clear();

  void SetListener(Listener listener);

  // === what the phone has actually done ===

  // Which of the five channels the phone opened on the live connection. Never the same
  // question as which were advertised, exactly as with the sensors: this is the first
  // thing to look at when nothing is arriving.
  MetadataMask opened() const { return opened_.load(); }
  // Updates received on one kind since the head unit started. The "is anything coming
  // in" number.
  int64_t updates(Metadata which) const;

  void NoteOpened(Metadata which);
  void ClearOpened();

 private:
  // Builds the JSON for one kind. Called with the lock held.
  std::string Render(Metadata which) const;
  // Releases `lock` and tells the listener. Takes the rendered JSON by value because
  // the state may change the moment the lock is dropped.
  void Publish(std::unique_lock<std::mutex> lock, Metadata which, std::string json);

  mutable std::mutex mutex_;
  NavigationInfo navigation_;
  MediaInfo media_;
  PhoneInfo phone_;
  NotificationInfo notification_;
  bool has_notification_ = false;
  BrowseNode browse_;
  bool has_browse_ = false;
  Listener listener_;

  std::atomic<MetadataMask> opened_{0};
  std::array<std::atomic<int64_t>, kMetadataCount> updates_{};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_METADATA_METADATA_STATE_H_
