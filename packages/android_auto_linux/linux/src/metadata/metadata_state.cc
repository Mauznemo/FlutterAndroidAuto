// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata_state.h"

#include "json.h"

namespace aa {
namespace {

const char* NavigationStatusName(NavigationStatus status) {
  switch (status) {
    case NavigationStatus::kUnavailable:
      return "unavailable";
    case NavigationStatus::kActive:
      return "active";
    case NavigationStatus::kInactive:
      return "inactive";
    case NavigationStatus::kRerouting:
      return "rerouting";
  }
  return "unavailable";
}

const char* PlaybackStateName(PlaybackState state) {
  switch (state) {
    case PlaybackState::kStopped:
      return "stopped";
    case PlaybackState::kPlaying:
      return "playing";
    case PlaybackState::kPaused:
      return "paused";
  }
  return "stopped";
}

const char* CallStateName(CallState state) {
  switch (state) {
    case CallState::kUnknown:
      return "unknown";
    case CallState::kInCall:
      return "inCall";
    case CallState::kOnHold:
      return "onHold";
    case CallState::kInactive:
      return "inactive";
    case CallState::kIncoming:
      return "incoming";
    case CallState::kConferenced:
      return "conferenced";
    case CallState::kMuted:
      return "muted";
  }
  return "unknown";
}

// Every one of these writes a key only when there is something to write, which is the
// whole of the absent-is-not-zero rule as it reaches the ABI.
void AddIfSet(Json* json, const char* key, const std::string& value) {
  if (!value.empty()) {
    json->AddString(key, value);
  }
}

void AddIfSet(Json* json, const char* key, const std::optional<int32_t>& value) {
  if (value.has_value()) {
    json->AddInt(key, *value);
  }
}

void AddIfSet(Json* json, const char* key, const std::optional<int64_t>& value) {
  if (value.has_value()) {
    json->AddInt(key, *value);
  }
}

void AddIfSet(Json* json, const char* key, const std::optional<bool>& value) {
  if (value.has_value()) {
    json->AddBool(key, *value);
  }
}

// A distance is flattened into the surrounding object rather than nested, because the
// two shapes it appears in (to the next turn, to the destination) would otherwise both
// need unwrapping on the Dart side for no gain.
void AddDistance(Json* json, const char* metres_key, const char* text_key,
                 const char* unit_key, const NavigationDistance& distance) {
  AddIfSet(json, metres_key, distance.metres);
  AddIfSet(json, text_key, distance.display_value);
  AddIfSet(json, unit_key, distance.display_unit);
}

std::string RenderSong(const BrowseSong& song) {
  Json json;
  AddIfSet(&json, "path", song.path);
  AddIfSet(&json, "name", song.name);
  AddIfSet(&json, "artist", song.artist);
  AddIfSet(&json, "album", song.album);
  AddIfSet(&json, "durationSeconds", song.duration_seconds);
  AddIfSet(&json, "art", song.art);
  return json.Done();
}

std::string RenderNavigation(const NavigationInfo& navigation) {
  Json json;
  if (navigation.status.has_value()) {
    json.AddString("status", NavigationStatusName(*navigation.status));
  }
  AddIfSet(&json, "maneuver", navigation.maneuver);
  AddIfSet(&json, "roundaboutExitNumber", navigation.roundabout_exit_number);
  AddIfSet(&json, "roundaboutExitAngle", navigation.roundabout_exit_angle);
  AddIfSet(&json, "road", navigation.road);
  AddIfSet(&json, "currentRoad", navigation.current_road);
  if (!navigation.cue.empty()) {
    std::vector<std::string> lines;
    lines.reserve(navigation.cue.size());
    for (const std::string& line : navigation.cue) {
      lines.push_back(JsonString(line));
    }
    json.AddRaw("cue", JsonArray(lines));
  }
  if (!navigation.lanes.empty()) {
    std::vector<std::string> lanes;
    lanes.reserve(navigation.lanes.size());
    for (const NavigationLane& lane : navigation.lanes) {
      std::vector<std::string> directions;
      directions.reserve(lane.directions.size());
      for (const NavigationLaneDirection& direction : lane.directions) {
        Json one;
        AddIfSet(&one, "shape", direction.shape);
        one.AddBool("highlighted", direction.highlighted);
        directions.push_back(one.Done());
      }
      Json one;
      one.AddRaw("directions", JsonArray(directions));
      lanes.push_back(one.Done());
    }
    json.AddRaw("lanes", JsonArray(lanes));
  }
  AddDistance(&json, "stepDistanceMetres", "stepDistanceText", "stepDistanceUnit",
              navigation.step_distance);
  AddIfSet(&json, "secondsToStep", navigation.seconds_to_step);
  if (!navigation.destinations.empty()) {
    std::vector<std::string> destinations;
    destinations.reserve(navigation.destinations.size());
    for (const NavigationDestination& destination : navigation.destinations) {
      Json one;
      AddIfSet(&one, "address", destination.address);
      AddDistance(&one, "distanceMetres", "distanceText", "distanceUnit",
                  destination.distance);
      AddIfSet(&one, "etaText", destination.eta_text);
      AddIfSet(&one, "secondsToArrival", destination.seconds_to_arrival);
      destinations.push_back(one.Done());
    }
    json.AddRaw("destinations", JsonArray(destinations));
  }
  AddIfSet(&json, "maneuverImage", navigation.maneuver_image);
  return json.Done();
}

std::string RenderMedia(const MediaInfo& media) {
  Json json;
  AddIfSet(&json, "song", media.song);
  AddIfSet(&json, "artist", media.artist);
  AddIfSet(&json, "album", media.album);
  AddIfSet(&json, "playlist", media.playlist);
  AddIfSet(&json, "durationSeconds", media.duration_seconds);
  AddIfSet(&json, "rating", media.rating);
  AddIfSet(&json, "albumArt", media.album_art);
  if (media.state.has_value()) {
    json.AddString("state", PlaybackStateName(*media.state));
  }
  AddIfSet(&json, "source", media.source);
  AddIfSet(&json, "positionSeconds", media.position_seconds);
  AddIfSet(&json, "shuffle", media.shuffle);
  AddIfSet(&json, "repeat", media.repeat);
  AddIfSet(&json, "repeatOne", media.repeat_one);
  return json.Done();
}

std::string RenderPhone(const PhoneInfo& phone) {
  Json json;
  std::vector<std::string> calls;
  calls.reserve(phone.calls.size());
  for (const CallInfo& call : phone.calls) {
    Json one;
    one.AddString("state", CallStateName(call.state));
    one.AddInt("durationSeconds", call.duration_seconds);
    AddIfSet(&one, "number", call.number);
    AddIfSet(&one, "callerId", call.caller_id);
    AddIfSet(&one, "numberType", call.number_type);
    AddIfSet(&one, "thumbnail", call.thumbnail);
    calls.push_back(one.Done());
  }
  // Always written, empty included: "no calls" is the answer a host app needs in order
  // to take a call banner away, and an absent key would be indistinguishable from a
  // phone that has said nothing yet.
  json.AddRaw("calls", JsonArray(calls));
  AddIfSet(&json, "signalStrength", phone.signal_strength);
  return json.Done();
}

std::string RenderNotification(const NotificationInfo& notification) {
  Json json;
  AddIfSet(&json, "id", notification.id);
  AddIfSet(&json, "text", notification.text);
  AddIfSet(&json, "icon", notification.icon);
  return json.Done();
}

std::string RenderBrowse(const BrowseNode& node) {
  Json json;
  AddIfSet(&json, "kind", node.kind);
  AddIfSet(&json, "path", node.path);
  AddIfSet(&json, "name", node.name);
  AddIfSet(&json, "type", node.type);
  AddIfSet(&json, "start", node.start);
  AddIfSet(&json, "total", node.total);
  if (!node.sources.empty()) {
    std::vector<std::string> sources;
    sources.reserve(node.sources.size());
    for (const BrowseSource& source : node.sources) {
      Json one;
      AddIfSet(&one, "path", source.path);
      AddIfSet(&one, "name", source.name);
      AddIfSet(&one, "art", source.art);
      sources.push_back(one.Done());
    }
    json.AddRaw("sources", JsonArray(sources));
  }
  if (!node.lists.empty()) {
    std::vector<std::string> lists;
    lists.reserve(node.lists.size());
    for (const BrowseList& list : node.lists) {
      Json one;
      AddIfSet(&one, "path", list.path);
      AddIfSet(&one, "type", list.type);
      AddIfSet(&one, "name", list.name);
      AddIfSet(&one, "art", list.art);
      lists.push_back(one.Done());
    }
    json.AddRaw("lists", JsonArray(lists));
  }
  if (!node.songs.empty()) {
    std::vector<std::string> songs;
    songs.reserve(node.songs.size());
    for (const BrowseSong& song : node.songs) {
      songs.push_back(RenderSong(song));
    }
    json.AddRaw("songs", JsonArray(songs));
  }
  if (node.song.has_value()) {
    json.AddRaw("song", RenderSong(*node.song));
  }
  return json.Done();
}

}  // namespace

const char* MetadataName(Metadata which) {
  switch (which) {
    case Metadata::kNavigation:
      return "navigation";
    case Metadata::kMedia:
      return "media";
    case Metadata::kPhone:
      return "phone";
    case Metadata::kNotification:
      return "notification";
    case Metadata::kBrowse:
      return "browse";
  }
  return "unknown";
}

void MetadataState::MutateNavigation(const std::function<void(NavigationInfo*)>& change) {
  std::unique_lock<std::mutex> lock(mutex_);
  change(&navigation_);
  std::string json = RenderNavigation(navigation_);
  Publish(std::move(lock), Metadata::kNavigation, std::move(json));
}

void MetadataState::MutateMedia(const std::function<void(MediaInfo*)>& change) {
  std::unique_lock<std::mutex> lock(mutex_);
  change(&media_);
  std::string json = RenderMedia(media_);
  Publish(std::move(lock), Metadata::kMedia, std::move(json));
}

void MetadataState::MutatePhone(const std::function<void(PhoneInfo*)>& change) {
  std::unique_lock<std::mutex> lock(mutex_);
  change(&phone_);
  std::string json = RenderPhone(phone_);
  Publish(std::move(lock), Metadata::kPhone, std::move(json));
}

void MetadataState::PublishNotification(const NotificationInfo& notification) {
  std::unique_lock<std::mutex> lock(mutex_);
  notification_ = notification;
  has_notification_ = true;
  std::string json = RenderNotification(notification_);
  Publish(std::move(lock), Metadata::kNotification, std::move(json));
}

void MetadataState::PublishBrowseNode(const BrowseNode& node) {
  std::unique_lock<std::mutex> lock(mutex_);
  browse_ = node;
  has_browse_ = true;
  std::string json = RenderBrowse(browse_);
  Publish(std::move(lock), Metadata::kBrowse, std::move(json));
}

void MetadataState::Publish(std::unique_lock<std::mutex> lock, Metadata which,
                            std::string json) {
  updates_[static_cast<int>(which)].fetch_add(1);
  Listener listener = listener_;
  // Outside the lock. The listener reaches the C ABI callback, which reaches the Dart
  // isolate, and holding a mutex across that is how a deadlock with the host app starts.
  lock.unlock();
  if (listener) {
    listener(which, json);
  }
}

std::string MetadataState::Render(Metadata which) const {
  switch (which) {
    case Metadata::kNavigation:
      return RenderNavigation(navigation_);
    case Metadata::kMedia:
      return RenderMedia(media_);
    case Metadata::kPhone:
      return RenderPhone(phone_);
    case Metadata::kNotification:
      return has_notification_ ? RenderNotification(notification_) : std::string();
    case Metadata::kBrowse:
      return has_browse_ ? RenderBrowse(browse_) : std::string();
  }
  return std::string();
}

std::string MetadataState::Snapshot(Metadata which) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Render(which);
}

void MetadataState::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  navigation_ = NavigationInfo();
  media_ = MediaInfo();
  phone_ = PhoneInfo();
  notification_ = NotificationInfo();
  has_notification_ = false;
  browse_ = BrowseNode();
  has_browse_ = false;
  // Deliberately without publishing. Clear() runs as a connection comes down, beside
  // the channels being dropped, and the host app is already being told the session
  // ended; five more updates saying "nothing" in the middle of that is noise. What a
  // late reader sees is what matters, and that is empty.
}

void MetadataState::SetListener(Listener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = std::move(listener);
}

int64_t MetadataState::updates(Metadata which) const {
  return updates_[static_cast<int>(which)].load();
}

void MetadataState::NoteOpened(Metadata which) {
  opened_.fetch_or(MetadataBit(which));
}

void MetadataState::ClearOpened() { opened_.store(0); }

}  // namespace aa
