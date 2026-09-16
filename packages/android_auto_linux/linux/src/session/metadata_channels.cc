#include "metadata_channels.h"

#include <cctype>

#include <aasdk/Common/Log.hpp>

#include <aap_protobuf/service/genericnotification/GenericNotificationMessageId.pb.h>
#include <aap_protobuf/service/genericnotification/message/GenericNotificationAck.pb.h>
#include <aap_protobuf/service/genericnotification/message/GenericNotificationMessage.pb.h>
#include <aap_protobuf/service/genericnotification/message/GenericNotificationSubscribe.pb.h>
#include <aap_protobuf/service/mediabrowser/MediaBrowserMessageId.pb.h>
#include <aap_protobuf/service/mediabrowser/message/MediaBrowserInput.pb.h>
#include <aap_protobuf/service/mediabrowser/message/MediaGetNode.pb.h>
#include <aap_protobuf/service/mediabrowser/message/MediaListNode.pb.h>
#include <aap_protobuf/service/mediabrowser/message/MediaRootNode.pb.h>
#include <aap_protobuf/service/mediabrowser/message/MediaSongNode.pb.h>
#include <aap_protobuf/service/mediabrowser/message/MediaSourceNode.pb.h>
#include <aap_protobuf/service/mediaplayback/MediaPlaybackStatusMessageId.pb.h>
#include <aap_protobuf/service/mediaplayback/message/MediaPlaybackMetadata.pb.h>
#include <aap_protobuf/service/mediaplayback/message/MediaPlaybackStatus.pb.h>
#include <aap_protobuf/service/navigationstatus/NavigationStatusMessageId.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationCurrentPosition.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationNextTurnDistanceEvent.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationNextTurnEvent.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationState.pb.h>
#include <aap_protobuf/service/navigationstatus/message/NavigationStatus.pb.h>
#include <aap_protobuf/service/phonestatus/PhoneStatusMessageId.pb.h>
#include <aap_protobuf/service/phonestatus/message/PhoneStatus.pb.h>

#include "../metadata/json.h"

namespace aa {

namespace browser_pb = aap_protobuf::service::mediabrowser;
namespace media_pb = aap_protobuf::service::mediaplayback;
namespace navigation_pb = aap_protobuf::service::navigationstatus;
namespace notification_pb = aap_protobuf::service::genericnotification;
namespace phone_pb = aap_protobuf::service::phonestatus;

namespace {

// SCREAMING_SNAKE from the schema to the camelCase the rest of the API speaks.
//
// Written once rather than as five switch statements. The maneuver enum alone has
// forty three values, and a hand written table of them would be forty three chances to
// mistype a name that only ever appears in a string, in a case nobody drives through
// twice. The protobuf runtime knows every name already.
std::string CamelFromEnumName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  bool upper_next = false;
  for (const char character : name) {
    if (character == '_') {
      upper_next = true;
      continue;
    }
    const char lower = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
    out.push_back(upper_next ? static_cast<char>(
                                   std::toupper(static_cast<unsigned char>(lower)))
                             : lower);
    upper_next = false;
  }
  return out;
}

NavigationStatus StatusFrom(
    navigation_pb::message::NavigationStatus_NavigationStatusEnum status) {
  switch (status) {
    case navigation_pb::message::NavigationStatus::ACTIVE:
      return NavigationStatus::kActive;
    case navigation_pb::message::NavigationStatus::INACTIVE:
      return NavigationStatus::kInactive;
    case navigation_pb::message::NavigationStatus::REROUTING:
      return NavigationStatus::kRerouting;
    case navigation_pb::message::NavigationStatus::UNAVAILABLE:
    default:
      return NavigationStatus::kUnavailable;
  }
}

PlaybackState PlaybackFrom(media_pb::message::MediaPlaybackStatus_State state) {
  switch (state) {
    case media_pb::message::MediaPlaybackStatus::PLAYING:
      return PlaybackState::kPlaying;
    case media_pb::message::MediaPlaybackStatus::PAUSED:
      return PlaybackState::kPaused;
    case media_pb::message::MediaPlaybackStatus::STOPPED:
    default:
      return PlaybackState::kStopped;
  }
}

CallState CallFrom(phone_pb::message::PhoneStatus_State state) {
  switch (state) {
    case phone_pb::message::PhoneStatus::IN_CALL:
      return CallState::kInCall;
    case phone_pb::message::PhoneStatus::ON_HOLD:
      return CallState::kOnHold;
    case phone_pb::message::PhoneStatus::INACTIVE:
      return CallState::kInactive;
    case phone_pb::message::PhoneStatus::INCOMING:
      return CallState::kIncoming;
    case phone_pb::message::PhoneStatus::CONFERENCED:
      return CallState::kConferenced;
    case phone_pb::message::PhoneStatus::MUTED:
      return CallState::kMuted;
    case phone_pb::message::PhoneStatus::UNKNOWN:
    default:
      return CallState::kUnknown;
  }
}

std::string Base64Of(const std::string& bytes) {
  return Base64(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

// The deprecated turn event's vocabulary, translated into the current one.
//
// Without this a head unit would see "slightTurn" from an older phone and
// "turnSlightLeft" from a newer one for the same instruction, and every host app would
// have to know both lists. The side is a separate field in the old message and baked
// into the name in the new one, which is the whole of the difference.
std::string ManeuverFromTurnEvent(
    const navigation_pb::message::NavigationNextTurnEvent& turn) {
  using Event = navigation_pb::message::NavigationNextTurnEvent;
  const bool left = turn.has_turn_side() && turn.turn_side() == Event::LEFT;
  const bool right = turn.has_turn_side() && turn.turn_side() == Event::RIGHT;
  // A turn with no side is not a left turn and not a right one, and guessing would put
  // the wrong arrow on the screen at the moment it matters most.
  auto sided = [left, right](const char* on_left, const char* on_right) {
    return left ? std::string(on_left)
                : (right ? std::string(on_right) : std::string("unknown"));
  };
  switch (turn.event()) {
    case Event::DEPART:
      return "depart";
    case Event::NAME_CHANGE:
      return "nameChange";
    case Event::SLIGHT_TURN:
      return sided("turnSlightLeft", "turnSlightRight");
    case Event::TURN:
      return sided("turnNormalLeft", "turnNormalRight");
    case Event::SHARP_TURN:
      return sided("turnSharpLeft", "turnSharpRight");
    case Event::U_TURN:
      return sided("uTurnLeft", "uTurnRight");
    case Event::ON_RAMP:
      return sided("onRampNormalLeft", "onRampNormalRight");
    case Event::OFF_RAMP:
      return sided("offRampNormalLeft", "offRampNormalRight");
    case Event::FORK:
      return sided("forkLeft", "forkRight");
    case Event::MERGE:
      return left ? "mergeLeft" : (right ? "mergeRight" : "mergeSideUnspecified");
    case Event::ROUNDABOUT_ENTER:
      return "roundaboutEnter";
    case Event::ROUNDABOUT_EXIT:
      return "roundaboutExit";
    case Event::ROUNDABOUT_ENTER_AND_EXIT:
      // The old message has no field for which way round the roundabout goes, and
      // clockwise is what a right hand drive country does. Wrong half the world over,
      // and there is nothing in the message to do better with.
      return "roundaboutEnterAndExitCw";
    case Event::STRAIGHT:
      return "straight";
    case Event::FERRY_BOAT:
      return "ferryBoat";
    case Event::FERRY_TRAIN:
      return "ferryTrain";
    case Event::DESTINATION:
      return left ? "destinationLeft" : (right ? "destinationRight" : "destination");
    case Event::UNKNOWN:
    default:
      return "unknown";
  }
}

NavigationDistance DistanceFrom(
    const navigation_pb::message::NavigationDistance& distance) {
  NavigationDistance out;
  if (distance.has_meters()) {
    out.metres = distance.meters();
  }
  if (distance.has_display_value()) {
    out.display_value = distance.display_value();
  }
  if (distance.has_display_units()) {
    out.display_unit = CamelFromEnumName(
        navigation_pb::message::NavigationDistance_DistanceUnits_Name(
            distance.display_units()));
  }
  return out;
}

// The channel each kind lives on. One place, so the advertised set, the created
// channels and the ids on the wire cannot drift apart.
aasdk::messenger::ChannelId ChannelIdOf(Metadata which) {
  switch (which) {
    case Metadata::kNavigation:
      return aasdk::messenger::ChannelId::NAVIGATION_STATUS;
    case Metadata::kMedia:
      return aasdk::messenger::ChannelId::MEDIA_PLAYBACK_STATUS;
    case Metadata::kPhone:
      return aasdk::messenger::ChannelId::PHONE_STATUS;
    case Metadata::kNotification:
      return aasdk::messenger::ChannelId::GENERIC_NOTIFICATION;
    case Metadata::kBrowse:
      return aasdk::messenger::ChannelId::MEDIA_BROWSER;
  }
  return aasdk::messenger::ChannelId::NONE;
}

}  // namespace

std::shared_ptr<MetadataChannels> MetadataChannels::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger, MetadataMask advertised,
    std::shared_ptr<MetadataState> state, LogHandler log) {
  return std::make_shared<MetadataChannels>(io_context, strand, std::move(messenger),
                                            advertised, std::move(state),
                                            std::move(log));
}

MetadataChannels::MetadataChannels(boost::asio::io_context& io_context,
                                   aasdk::Strand& strand,
                                   aasdk::messenger::IMessenger::Pointer messenger,
                                   MetadataMask advertised,
                                   std::shared_ptr<MetadataState> state, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      advertised_(advertised),
      state_(std::move(state)),
      log_(std::move(log)) {}

MetadataChannels::~MetadataChannels() { Stop(); }

void MetadataChannels::Start() {
  std::array<std::shared_ptr<MetadataChannel>, kMetadataCount> created;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_.load() || !messenger_) {
      return;
    }
    for (int index = 0; index < kMetadataCount; ++index) {
      const auto which = static_cast<Metadata>(index);
      if ((advertised_ & MetadataBit(which)) == 0) {
        continue;
      }
      std::weak_ptr<MetadataChannels> weak = weak_from_this();
      // Weak in all three, for the reason ControlEventRelay exists: the channel binds
      // itself into the messenger's promise, so a handler owning this object would make
      // a cycle and the USB interface would never be released.
      auto channel = MetadataChannel::Create(
          io_context_, strand_, messenger_, ChannelIdOf(which), MetadataName(which),
          [weak, which]() {
            if (auto self = weak.lock()) {
              self->OnOpen(which);
            }
          },
          [weak, which](uint16_t id, const aasdk::common::DataConstBuffer& payload) {
            auto self = weak.lock();
            if (!self) {
              return;
            }
            switch (which) {
              case Metadata::kNavigation:
                self->OnNavigation(id, payload);
                break;
              case Metadata::kMedia:
                self->OnMedia(id, payload);
                break;
              case Metadata::kPhone:
                self->OnPhone(id, payload);
                break;
              case Metadata::kNotification:
                self->OnNotification(id, payload);
                break;
              case Metadata::kBrowse:
                self->OnBrowse(id, payload);
                break;
            }
          },
          [weak](const std::string& message) {
            if (auto self = weak.lock()) {
              self->Log(message);
            }
          });
      channels_[index] = channel;
      created[index] = std::move(channel);
    }
  }
  // Outside the lock: Start() arms a receive, which reaches the messenger.
  for (auto& channel : created) {
    if (channel) {
      channel->Start();
    }
  }
}

void MetadataChannels::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  std::array<std::shared_ptr<MetadataChannel>, kMetadataCount> channels;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    channels = std::move(channels_);
    channels_ = {};
    messenger_.reset();
  }
  for (auto& channel : channels) {
    if (channel) {
      channel->Stop();
    }
  }
  if (state_) {
    // The connection is what made any of it true. A track that was playing over a cable
    // that has been pulled is not paused, it is gone, and a now playing bar still
    // showing it is worse than an empty one.
    state_->ClearOpened();
    state_->Clear();
  }
}

std::shared_ptr<MetadataChannel> MetadataChannels::Get(Metadata which) const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return channels_[static_cast<int>(which)];
}

std::shared_ptr<MetadataChannel> MetadataChannels::GetOpen(Metadata which) const {
  // A channel exists from the moment it is advertised, and the phone opens it or does
  // not. This Pixel never opens the notification or the media browser, so a browse
  // request sent on the strength of the channel existing goes into the void exactly as
  // an early input report does, with nothing to say it went nowhere. The caller is told
  // no instead.
  if (!state_ || (state_->opened() & MetadataBit(which)) == 0) {
    return nullptr;
  }
  return Get(which);
}

void MetadataChannels::OnOpen(Metadata which) {
  if (state_) {
    state_->NoteOpened(which);
  }
  AASDK_LOG(debug) << "[Metadata] the phone opened the " << MetadataName(which)
                   << " channel";

  if (which == Metadata::kNotification) {
    // The one channel that needs asking. The phone pushes navigation, playback and
    // telephony the moment their channels are open, but a notification is addressed to
    // a head unit that wants them, so it is sent only to one that has said so.
    if (auto channel = Get(which)) {
      notification_pb::message::GenericNotificationSubscribe subscribe;
      channel->Send(notification_pb::GENERIC_NOTIFICATION_SUBSCRIBE, subscribe,
                    "notification subscribe");
    }
  }
}

// Two of the message ids below are marked deprecated in the schema, and decoding them
// is the point: a phone on an older build sends those and nothing else, so refusing to
// name them would mean no turn card at all on the phones most likely to be in an
// aftermarket head unit. Deprecated is not the same as gone. The -Wpragmas line first,
// so the compiler that does not know the second warning's name does not object to being
// told to ignore it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations-switch-case"
void MetadataChannels::OnNavigation(uint16_t id,
                                    const aasdk::common::DataConstBuffer& payload) {
  if (!state_) {
    return;
  }
  switch (id) {
    case navigation_pb::INSTRUMENT_CLUSTER_NAVIGATION_STATUS: {
      navigation_pb::message::NavigationStatus status;
      if (!status.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      const NavigationStatus value = StatusFrom(status.status());
      state_->MutateNavigation([value](NavigationInfo* navigation) {
        navigation->status = value;
        if (value != NavigationStatus::kActive &&
            value != NavigationStatus::kRerouting) {
          // Guidance ended. Everything else in here described a turn that is no longer
          // coming, and leaving it would have a head unit draw an arrow at a driver who
          // has arrived. The status itself is kept, because "inactive" is the answer.
          const auto status = navigation->status;
          *navigation = NavigationInfo();
          navigation->status = status;
        }
      });
      break;
    }
    case navigation_pb::INSTRUMENT_CLUSTER_NAVIGATION_STATE: {
      navigation_pb::message::NavigationState state;
      if (!state.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      state_->MutateNavigation([&state](NavigationInfo* navigation) {
        // Only the first step. The protocol allows a list of them, but everything a
        // head unit shows is about the one coming next, and a phone has only ever been
        // seen to send one.
        navigation->maneuver.clear();
        navigation->roundabout_exit_number.reset();
        navigation->roundabout_exit_angle.reset();
        navigation->road.clear();
        navigation->cue.clear();
        navigation->lanes.clear();
        if (state.steps_size() > 0) {
          const auto& step = state.steps(0);
          if (step.has_maneuver()) {
            const auto& maneuver = step.maneuver();
            if (maneuver.has_type()) {
              navigation->maneuver = CamelFromEnumName(
                  navigation_pb::message::NavigationManeuver_NavigationType_Name(
                      maneuver.type()));
            }
            if (maneuver.has_roundabout_exit_number()) {
              navigation->roundabout_exit_number = maneuver.roundabout_exit_number();
            }
            if (maneuver.has_roundabout_exit_angle()) {
              navigation->roundabout_exit_angle = maneuver.roundabout_exit_angle();
            }
          }
          if (step.has_road() && step.road().has_name()) {
            navigation->road = step.road().name();
          }
          if (step.has_cue()) {
            for (const auto& line : step.cue().alternate_text()) {
              navigation->cue.push_back(line);
            }
          }
          for (const auto& lane : step.lanes()) {
            NavigationLane out;
            for (const auto& direction : lane.lane_directions()) {
              NavigationLaneDirection one;
              if (direction.has_shape()) {
                one.shape = CamelFromEnumName(
                    navigation_pb::message::NavigationLane_LaneDirection_Shape_Name(
                        direction.shape()));
              }
              one.highlighted = direction.is_highlighted();
              out.directions.push_back(std::move(one));
            }
            navigation->lanes.push_back(std::move(out));
          }
        }
        navigation->destinations.clear();
        for (const auto& destination : state.destinations()) {
          NavigationDestination out;
          if (destination.has_address()) {
            out.address = destination.address();
          }
          navigation->destinations.push_back(std::move(out));
        }
      });
      break;
    }
    case navigation_pb::INSTRUMENT_CLUSTER_NAVIGATION_CURRENT_POSITION: {
      navigation_pb::message::NavigationCurrentPosition position;
      if (!position.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      state_->MutateNavigation([&position](NavigationInfo* navigation) {
        if (position.has_current_road() && position.current_road().has_name()) {
          navigation->current_road = position.current_road().name();
        }
        if (position.has_step_distance()) {
          const auto& step = position.step_distance();
          if (step.has_distance()) {
            navigation->step_distance = DistanceFrom(step.distance());
          }
          if (step.has_time_to_step_seconds()) {
            navigation->seconds_to_step = step.time_to_step_seconds();
          }
        }
        // The addresses came with the state message and the distances come with this
        // one, so they are matched by position rather than replaced. A phone that sends
        // distances before it has ever sent a state gets entries with no address, which
        // is still the right count of destinations.
        const int count = position.destination_distances_size();
        if (static_cast<int>(navigation->destinations.size()) < count) {
          navigation->destinations.resize(count);
        }
        for (int index = 0; index < count; ++index) {
          const auto& distance = position.destination_distances(index);
          NavigationDestination& out = navigation->destinations[index];
          if (distance.has_distance()) {
            out.distance = DistanceFrom(distance.distance());
          }
          if (distance.has_estimated_time_at_arrival()) {
            out.eta_text = distance.estimated_time_at_arrival();
          }
          if (distance.has_time_to_arrival_seconds()) {
            out.seconds_to_arrival = distance.time_to_arrival_seconds();
          }
        }
      });
      break;
    }
    case navigation_pb::INSTRUMENT_CLUSTER_NAVIGATION_TURN_EVENT: {
      // Deprecated in the schema and still the only turn a phone on an older build
      // sends. It carries a rendered arrow rather than a maneuver to draw, which is why
      // maneuver_image exists at all.
      navigation_pb::message::NavigationNextTurnEvent turn;
      if (!turn.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      state_->MutateNavigation([&turn](NavigationInfo* navigation) {
        navigation->road = turn.road();
        if (turn.has_event()) {
          navigation->maneuver = ManeuverFromTurnEvent(turn);
        }
        // turn_number and turn_angle only ever describe a roundabout, which is what the
        // current message spells out in the field names.
        if (turn.has_turn_number()) {
          navigation->roundabout_exit_number = turn.turn_number();
        }
        if (turn.has_turn_angle()) {
          navigation->roundabout_exit_angle = turn.turn_angle();
        }
        navigation->maneuver_image =
            turn.has_image() ? Base64Of(turn.image()) : std::string();
      });
      break;
    }
    case navigation_pb::INSTRUMENT_CLUSTER_NAVIGATION_DISTANCE_EVENT: {
      navigation_pb::message::NavigationNextTurnDistanceEvent distance;
      if (!distance.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      state_->MutateNavigation([&distance](NavigationInfo* navigation) {
        navigation->step_distance = NavigationDistance();
        navigation->step_distance.metres = distance.distance_meters();
        if (distance.has_display_distance_e3()) {
          // The only place the older pair differs in shape: the number to show comes as
          // a fixed point integer rather than as the string the phone would print, so
          // it is turned back into one here. Thousandths, as the e3 says.
          const double value = distance.display_distance_e3() / 1000.0;
          char formatted[32];
          snprintf(formatted, sizeof(formatted), "%.1f", value);
          navigation->step_distance.display_value = formatted;
        }
        if (distance.has_display_distance_unit()) {
          navigation->step_distance.display_unit = CamelFromEnumName(
              navigation_pb::message::
                  NavigationNextTurnDistanceEvent_DistanceUnits_Name(
                      distance.display_distance_unit()));
        }
        navigation->seconds_to_step = distance.time_to_turn_seconds();
      });
      break;
    }
    case navigation_pb::INSTRUMENT_CLUSTER_START:
    case navigation_pb::INSTRUMENT_CLUSTER_STOP:
      // Both carry an empty message. Nothing to decode, and the status message says the
      // same thing in a form a host app can act on.
      break;
    default:
      LogUnhandled(Metadata::kNavigation, id, payload.size);
      break;
  }
}
#pragma GCC diagnostic pop

void MetadataChannels::OnMedia(uint16_t id,
                               const aasdk::common::DataConstBuffer& payload) {
  if (!state_) {
    return;
  }
  switch (id) {
    case media_pb::MEDIA_PLAYBACK_METADATA: {
      media_pb::message::MediaPlaybackMetadata metadata;
      if (!metadata.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      state_->MutateMedia([&metadata](MediaInfo* media) {
        media->song = metadata.song();
        media->artist = metadata.artist();
        media->album = metadata.album();
        media->playlist = metadata.playlist();
        media->duration_seconds =
            metadata.has_duration_seconds()
                ? std::optional<int64_t>(metadata.duration_seconds())
                : std::nullopt;
        media->rating = metadata.has_rating() ? std::optional<int32_t>(metadata.rating())
                                              : std::nullopt;
        // Encoded here, once, rather than every time the position ticks: the artwork
        // rides along in every snapshot and re-encoding fifty kilobytes a second to say
        // a track moved on would be absurd.
        media->album_art =
            metadata.has_album_art() ? Base64Of(metadata.album_art()) : std::string();
      });
      break;
    }
    case media_pb::MEDIA_PLAYBACK_STATUS: {
      media_pb::message::MediaPlaybackStatus status;
      if (!status.ParseFromArray(payload.cdata, payload.size)) {
        break;
      }
      state_->MutateMedia([&status](MediaInfo* media) {
        if (status.has_state()) {
          media->state = PlaybackFrom(status.state());
        }
        if (status.has_media_source()) {
          media->source = status.media_source();
        }
        if (status.has_playback_seconds()) {
          media->position_seconds = status.playback_seconds();
        }
        if (status.has_shuffle()) {
          media->shuffle = status.shuffle();
        }
        if (status.has_repeat()) {
          media->repeat = status.repeat();
        }
        if (status.has_repeat_one()) {
          media->repeat_one = status.repeat_one();
        }
      });
      break;
    }
    default:
      LogUnhandled(Metadata::kMedia, id, payload.size);
      break;
  }
}

void MetadataChannels::OnPhone(uint16_t id,
                               const aasdk::common::DataConstBuffer& payload) {
  if (!state_) {
    return;
  }
  if (id != phone_pb::PHONE_STATUS) {
    LogUnhandled(Metadata::kPhone, id, payload.size);
    return;
  }
  phone_pb::message::PhoneStatus status;
  if (!status.ParseFromArray(payload.cdata, payload.size)) {
    return;
  }
  state_->MutatePhone([&status](PhoneInfo* phone) {
    // Replaced rather than merged. The message is the whole list of calls every time,
    // so a call that has ended is one that is no longer in it, and merging would leave
    // a finished call on the screen forever.
    phone->calls.clear();
    for (const auto& call : status.calls()) {
      CallInfo out;
      out.state = CallFrom(call.phone_state());
      out.duration_seconds = call.call_duration_seconds();
      out.number = call.caller_number();
      out.caller_id = call.caller_id();
      out.number_type = call.caller_number_type();
      if (call.has_caller_thumbnail()) {
        out.thumbnail = Base64Of(call.caller_thumbnail());
      }
      phone->calls.push_back(std::move(out));
    }
    if (status.has_signal_strength()) {
      phone->signal_strength = static_cast<int32_t>(status.signal_strength());
    }
  });
}

void MetadataChannels::OnNotification(uint16_t id,
                                      const aasdk::common::DataConstBuffer& payload) {
  if (!state_) {
    return;
  }
  if (id != notification_pb::GENERIC_NOTIFICATION_MESSAGE) {
    LogUnhandled(Metadata::kNotification, id, payload.size);
    return;
  }
  notification_pb::message::GenericNotificationMessage message;
  if (!message.ParseFromArray(payload.cdata, payload.size)) {
    return;
  }
  NotificationInfo notification;
  notification.id = message.id();
  notification.text = message.text();
  if (message.has_icon()) {
    notification.icon = Base64Of(message.icon());
  }
  state_->PublishNotification(notification);

  // Acknowledged as handled, unconditionally. This head unit has passed it to the host
  // app, which is the whole of what it can promise; whether a person saw it is not
  // something the protocol asks and not something this could answer. An unacknowledged
  // notification is one the phone will send again.
  if (auto channel = Get(Metadata::kNotification)) {
    notification_pb::message::GenericNotificationAck ack;
    ack.set_id(message.id());
    ack.set_handled(true);
    channel->Send(notification_pb::GENERIC_NOTIFICATION_ACK, ack, "notification ack");
  }
}

void MetadataChannels::OnBrowse(uint16_t id,
                                const aasdk::common::DataConstBuffer& payload) {
  if (!state_) {
    return;
  }
  BrowseNode node;
  switch (id) {
    case browser_pb::MEDIA_ROOT_NODE: {
      browser_pb::message::MediaRootNode root;
      if (!root.ParseFromArray(payload.cdata, payload.size)) {
        return;
      }
      node.kind = "root";
      node.path = root.path();
      for (const auto& source : root.media_sources()) {
        BrowseSource out;
        out.path = source.path();
        out.name = source.name();
        if (source.has_album_art()) {
          out.art = Base64Of(source.album_art());
        }
        node.sources.push_back(std::move(out));
      }
      break;
    }
    case browser_pb::MEDIA_SOURCE_NODE: {
      browser_pb::message::MediaSourceNode source;
      if (!source.ParseFromArray(payload.cdata, payload.size)) {
        return;
      }
      node.kind = "source";
      node.path = source.source().path();
      node.name = source.source().name();
      if (source.has_start()) {
        node.start = source.start();
      }
      if (source.has_total()) {
        node.total = source.total();
      }
      for (const auto& list : source.lists()) {
        BrowseList out;
        out.path = list.path();
        out.type = CamelFromEnumName(
            browser_pb::message::MediaList_Type_Name(list.type()));
        out.name = list.name();
        if (list.has_album_art()) {
          out.art = Base64Of(list.album_art());
        }
        node.lists.push_back(std::move(out));
      }
      break;
    }
    case browser_pb::MEDIA_LIST_NODE: {
      browser_pb::message::MediaListNode list;
      if (!list.ParseFromArray(payload.cdata, payload.size)) {
        return;
      }
      node.kind = "list";
      node.path = list.list().path();
      node.name = list.list().name();
      node.type =
          CamelFromEnumName(browser_pb::message::MediaList_Type_Name(list.list().type()));
      if (list.has_start()) {
        node.start = list.start();
      }
      if (list.has_total()) {
        node.total = list.total();
      }
      for (const auto& song : list.songs()) {
        BrowseSong out;
        out.path = song.path();
        out.name = song.name();
        out.artist = song.artist();
        out.album = song.album();
        node.songs.push_back(std::move(out));
      }
      break;
    }
    case browser_pb::MEDIA_SONG_NODE: {
      browser_pb::message::MediaSongNode song;
      if (!song.ParseFromArray(payload.cdata, payload.size)) {
        return;
      }
      node.kind = "song";
      node.path = song.song().path();
      node.name = song.song().name();
      BrowseSong out;
      out.path = song.song().path();
      out.name = song.song().name();
      out.artist = song.song().artist();
      out.album = song.song().album();
      if (song.has_duration_seconds()) {
        out.duration_seconds = song.duration_seconds();
      }
      if (song.has_album_art()) {
        out.art = Base64Of(song.album_art());
      }
      node.song = std::move(out);
      break;
    }
    default:
      LogUnhandled(Metadata::kBrowse, id, payload.size);
      return;
  }
  state_->PublishBrowseNode(node);
}

bool MetadataChannels::Browse(const std::string& path, int32_t start) {
  auto channel = GetOpen(Metadata::kBrowse);
  if (!channel) {
    return false;
  }
  browser_pb::message::MediaGetNode request;
  request.set_path(path);
  if (start > 0) {
    request.set_start(start);
  }
  // Artwork is tens of kilobytes per entry and a library listing can be hundreds of
  // entries, so it is asked for only on a single song. A host app that wants covers in
  // a list asks for each song's node.
  request.set_get_album_art(false);
  // Posted onto the strand rather than sent inline, for the reason
  // InputChannel::SendReport gives: this arrives on Flutter's platform thread while the
  // channel's own message handling runs on the strand.
  std::weak_ptr<MetadataChannels> weak = weak_from_this();
  strand_.post([weak, request]() {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    if (auto channel = self->GetOpen(Metadata::kBrowse)) {
      channel->Send(browser_pb::MEDIA_GET_NODE, request, "browse request");
    }
  });
  return true;
}

bool MetadataChannels::BrowseSelect(const std::string& path) {
  auto channel = GetOpen(Metadata::kBrowse);
  if (!channel) {
    return false;
  }
  browser_pb::message::MediaBrowserInput input;
  input.mutable_input()->set_action(aap_protobuf::shared::InstrumentClusterInput::ENTER);
  input.set_path(path);
  std::weak_ptr<MetadataChannels> weak = weak_from_this();
  strand_.post([weak, input]() {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    if (auto channel = self->GetOpen(Metadata::kBrowse)) {
      channel->Send(browser_pb::MEDIA_BROWSE_INPUT, input, "browse selection");
    }
  });
  return true;
}

void MetadataChannels::LogUnhandled(Metadata which, uint16_t id, size_t size) {
  // Debug rather than an error, unlike aasdk's own channels. The schema carries ids
  // this head unit has no use for, and a phone sending one is a phone doing its job.
  AASDK_LOG(debug) << "[Metadata] " << MetadataName(which) << " message " << id << ", "
                   << size << " bytes, not decoded";
}

void MetadataChannels::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

}  // namespace aa
