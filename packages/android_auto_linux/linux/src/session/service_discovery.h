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

#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>

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
  bool enable_sensors = true;
};

// Fills `response` in place.
void BuildServiceDiscoveryResponse(
    const HeadUnitDescription& description,
    aap_protobuf::service::control::message::ServiceDiscoveryResponse* response);

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_SERVICE_DISCOVERY_H_
