// Builds the ServiceDiscoveryResponse: the head unit's description of itself.
//
// This is the single most consequential message in the protocol. The phone reads it
// once and decides from it what resolution to encode at, what audio formats to send,
// whether touch works, and which features to offer at all. Getting a field wrong here
// shows up much later as a black screen or silent audio, with nothing in the logs.
//
// One rule worth remembering: only advertise a service that is actually serviced. The
// phone will open every channel listed here, and if one is never answered it drops the
// whole connection.

#ifndef ANDROID_AUTO_LINUX_SESSION_SERVICE_DISCOVERY_H_
#define ANDROID_AUTO_LINUX_SESSION_SERVICE_DISCOVERY_H_

#include <cstdint>
#include <string>
#include <vector>

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>

#include "../sensors/sensor_state.h"

namespace aa {

// What the head unit claims to be. Mirrors AaConfig, decoupled so this file does not
// depend on the C ABI.
struct HeadUnitDescription {
  int32_t width = 1280;
  int32_t height = 720;
  int32_t fps = 30;
  int32_t dpi = 140;
  std::string head_unit_name = "Flutter Head Unit";
  std::string car_model = "Universal";
  std::string car_year = "2026";
  // Serial number of the "vehicle". Any stable string will do, but it must not be
  // empty: it was a required field in the schema older phones validate against.
  std::string vehicle_id = "20260913";

  // Which channels to advertise. Each one must have a handler before it is turned on.
  bool enable_video = true;
  bool enable_input = true;
  bool enable_media_audio = false;
  bool enable_system_audio = false;
  bool enable_speech_audio = false;
  bool enable_microphone = false;

  // Which sensors to advertise, one bit each. Empty means no sensor channel at all,
  // which is not a configuration any phone accepts: the driving status subscription is
  // what unlocks most of Android Auto's interface.
  //
  // This is the host app declaring what the car has, not a list of features to turn on.
  // A phone that is told the head unit has a position stops using its own, so a head
  // unit that advertises location and then has no fix has taken navigation away from a
  // phone that was managing perfectly well. Advertise what you can actually supply.
  SensorMask sensors = kRequiredSensors;
};

// The hardware keys this head unit tells the phone it can produce.
//
// Advertising one is a promise, not a subscription: the phone may bind any of them and
// route them itself, and it expects the head unit to be able to send every code on the
// list. Keep it in step with AndroidAutoKey in the platform interface.
const std::vector<int32_t>& SupportedKeycodes();

// Fills `response` in place.
void BuildServiceDiscoveryResponse(
    const HeadUnitDescription& description,
    aap_protobuf::service::control::message::ServiceDiscoveryResponse* response);

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_SERVICE_DISCOVERY_H_
