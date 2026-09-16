#include "service_discovery.h"

#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/inputsource/InputSourceService.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchScreenType.pb.h>
#include <aap_protobuf/service/media/sink/message/KeyCode.pb.h>
#include <aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h>
#include <aap_protobuf/service/media/shared/message/MediaCodecType.pb.h>
#include <aap_protobuf/service/media/sink/MediaSinkService.pb.h>
#include <aap_protobuf/service/media/sink/message/AudioStreamType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h>
#include <aap_protobuf/service/media/source/MediaSourceService.pb.h>
#include <aap_protobuf/service/sensorsource/SensorSourceService.pb.h>
#include <aap_protobuf/service/sensorsource/message/Sensor.pb.h>
#include <aap_protobuf/service/control/message/DriverPosition.pb.h>
#include <aap_protobuf/service/control/message/HeadUnitInfo.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>

#include <aasdk/Common/Log.hpp>
#include <aasdk/Messenger/ChannelId.hpp>

#include "sensor_channel.h"

namespace aa {
namespace {

namespace pb = aap_protobuf;
namespace sink = aap_protobuf::service::media::sink;
namespace shared = aap_protobuf::service::media::shared;

int32_t ChannelNumber(aasdk::messenger::ChannelId id) {
  return static_cast<int32_t>(id);
}

// The protocol only has names for a fixed set of resolutions, so the configured size
// has to land on one of them.
sink::message::VideoCodecResolutionType ResolutionFor(int32_t width, int32_t height) {
  if (width == 800 && height == 480) {
    return sink::message::VIDEO_800x480;
  }
  if (width == 1920 && height == 1080) {
    return sink::message::VIDEO_1920x1080;
  }
  if (width == 2560 && height == 1440) {
    return sink::message::VIDEO_2560x1440;
  }
  if (width == 3840 && height == 2160) {
    return sink::message::VIDEO_3840x2160;
  }
  // 1280x720 is the safe default: every phone supports it, and an unusual size here
  // fails in ways that are hard to attribute later.
  return sink::message::VIDEO_1280x720;
}

void AddVideoService(const HeadUnitDescription& description,
                     pb::service::control::message::ServiceDiscoveryResponse* response) {
  auto* service = response->add_channels();
  service->set_id(ChannelNumber(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO));

  auto* sink_service = service->mutable_media_sink_service();
  sink_service->set_available_type(shared::message::MEDIA_CODEC_VIDEO_H264_BP);
  sink_service->set_display_id(0);

  auto* video = sink_service->add_video_configs();
  video->set_codec_resolution(ResolutionFor(description.width, description.height));
  video->set_frame_rate(description.fps >= 60 ? sink::message::VIDEO_FPS_60
                                              : sink::message::VIDEO_FPS_30);
  video->set_density(static_cast<uint32_t>(description.dpi));
  video->set_real_density(static_cast<uint32_t>(description.dpi));
  // No margins: the host app decides how to letterbox the texture in Flutter, and
  // asking the phone to letterbox as well would double up.
  video->set_width_margin(0);
  video->set_height_margin(0);
  video->set_video_codec_type(shared::message::MEDIA_CODEC_VIDEO_H264_BP);
}

void AddAudioSink(pb::service::control::message::ServiceDiscoveryResponse* response,
                  aasdk::messenger::ChannelId channel_id,
                  sink::message::AudioStreamType stream_type, uint32_t sampling_rate,
                  uint32_t channels) {
  auto* service = response->add_channels();
  service->set_id(ChannelNumber(channel_id));

  auto* sink_service = service->mutable_media_sink_service();
  sink_service->set_available_type(shared::message::MEDIA_CODEC_AUDIO_PCM);
  sink_service->set_audio_type(stream_type);

  auto* audio = sink_service->add_audio_configs();
  audio->set_sampling_rate(sampling_rate);
  audio->set_number_of_bits(16);
  audio->set_number_of_channels(channels);
}

void AddInputService(const HeadUnitDescription& description,
                     pb::service::control::message::ServiceDiscoveryResponse* response) {
  auto* service = response->add_channels();
  service->set_id(ChannelNumber(aasdk::messenger::ChannelId::INPUT_SOURCE));

  auto* input = service->mutable_input_source_service();
  auto* touchscreen = input->add_touchscreen();
  // Touch coordinates are in projected pixels, so the touchscreen the head unit claims
  // to have is exactly the size of the video it asked for.
  touchscreen->set_width(description.width);
  touchscreen->set_height(description.height);
  touchscreen->set_type(pb::service::inputsource::message::CAPACITIVE);
  touchscreen->set_is_secondary(false);
  for (const int32_t keycode : SupportedKeycodes()) {
    input->add_keycodes_supported(keycode);
  }
  input->set_display_id(0);
}

void AddMicrophoneService(
    pb::service::control::message::ServiceDiscoveryResponse* response) {
  auto* service = response->add_channels();
  service->set_id(ChannelNumber(aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE));

  auto* source = service->mutable_media_source_service();
  source->set_available_type(shared::message::MEDIA_CODEC_AUDIO_PCM);
  auto* audio = source->mutable_audio_config();
  audio->set_sampling_rate(16000);
  audio->set_number_of_bits(16);
  audio->set_number_of_channels(1);
}

// How the head unit's position was arrived at, as the LocationCharacterization bits
// Android's car framework defines. RAW_GNSS_ONLY says the fix comes straight from a
// satellite receiver with no dead reckoning or sensor fusion behind it, which is the
// honest answer for a head unit relaying a position the host app handed it. The phone
// uses this to decide how much to trust the fix between updates.
constexpr uint32_t kLocationRawGnssOnly = 0x100;

void AddSensorService(const HeadUnitDescription& description,
                      pb::service::control::message::ServiceDiscoveryResponse* response) {
  auto* service = response->add_channels();
  service->set_id(ChannelNumber(aasdk::messenger::ChannelId::SENSOR));

  auto* sensors = service->mutable_sensor_source_service();
  // Exactly what the host app said the car has, no more. Every one of these is a
  // promise the phone acts on: it subscribes to what is listed here and waits for the
  // readings, and for location it stops using its own the moment it sees the entry.
  // src/session/sensor_channel.cc refuses a subscription to anything not on this list
  // rather than accepting it and then sending nothing.
  //
  // Driving status in particular is not optional: without it the phone assumes it
  // cannot verify the car is parked and locks parts of the UI out.
  for (int index = 0; index < kSensorCount; ++index) {
    const auto sensor = static_cast<Sensor>(index);
    if ((description.sensors & SensorBit(sensor)) == 0) {
      continue;
    }
    sensors->add_sensors()->set_sensor_type(SensorTypeOf(sensor));
  }
  if ((description.sensors & SensorBit(Sensor::kLocation)) != 0) {
    sensors->set_location_characterization(kLocationRawGnssOnly);
  }
}

}  // namespace

const std::vector<int32_t>& SupportedKeycodes() {
  // The set openauto advertises, which is the set phones are actually tested against.
  // Advertising a key is a promise that the head unit can produce it, not a request to
  // receive it, so the list has to match what InputChannel::SendKey will ever be asked
  // for and AndroidAutoKey on the Dart side.
  //
  // The rotary encoder is in here as a keycode even though the head unit reports it as
  // a relative axis. That is how the protocol names the device.
  static const std::vector<int32_t> keycodes = {
      sink::message::KEYCODE_DPAD_UP,          sink::message::KEYCODE_DPAD_DOWN,
      sink::message::KEYCODE_DPAD_LEFT,        sink::message::KEYCODE_DPAD_RIGHT,
      sink::message::KEYCODE_DPAD_CENTER,      sink::message::KEYCODE_BACK,
      sink::message::KEYCODE_HOME,             sink::message::KEYCODE_CALL,
      sink::message::KEYCODE_ENDCALL,          sink::message::KEYCODE_MEDIA_PLAY,
      sink::message::KEYCODE_MEDIA_PAUSE,      sink::message::KEYCODE_MEDIA_PLAY_PAUSE,
      sink::message::KEYCODE_MEDIA_NEXT,       sink::message::KEYCODE_MEDIA_PREVIOUS,
      // What the protocol calls SEARCH is the microphone button on a head unit: it is
      // what starts the Assistant, not a text search.
      sink::message::KEYCODE_SEARCH,           sink::message::KEYCODE_ROTARY_CONTROLLER,
  };
  return keycodes;
}

void BuildServiceDiscoveryResponse(
    const HeadUnitDescription& description,
    pb::service::control::message::ServiceDiscoveryResponse* response) {
  response->Clear();

  if (description.enable_video) {
    AddVideoService(description, response);
  }
  if (description.enable_input) {
    AddInputService(description, response);
  }
  if (description.enable_media_audio) {
    AddAudioSink(response, aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO,
                 sink::message::AUDIO_STREAM_MEDIA, 48000, 2);
  }
  if (description.enable_system_audio) {
    AddAudioSink(response, aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO,
                 sink::message::AUDIO_STREAM_SYSTEM_AUDIO, 16000, 1);
  }
  if (description.enable_speech_audio) {
    AddAudioSink(response, aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO,
                 sink::message::AUDIO_STREAM_GUIDANCE, 16000, 1);
  }
  if (description.enable_microphone) {
    AddMicrophoneService(response);
  }
  if (description.sensors != 0) {
    AddSensorService(description, response);
  }

  response->set_display_name(description.head_unit_name);
  // Every one of these was `required` in the schema openauto was built against, so a
  // phone that still validates against that shape refuses the response outright when
  // one is missing, and refuses it silently: it drops out of accessory mode without a
  // word. Fill them all in even though the current schema calls them optional.
  response->set_driver_position(pb::service::control::message::DRIVER_POSITION_LEFT);

  // The make/model/year fields are marked deprecated in the schema, but phones still
  // read them and leaving them empty makes some builds refuse to project. Deprecated
  // is not the same as unused, so the warning is suppressed rather than obeyed.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  response->set_make("Flutter");
  response->set_model(description.car_model);
  response->set_year(description.car_year);
  response->set_vehicle_id(description.vehicle_id);
  response->set_head_unit_make("Flutter");
  response->set_head_unit_model(description.head_unit_name);
  response->set_head_unit_software_build("1");
  response->set_head_unit_software_version("1.0");
  response->set_can_play_native_media_during_vr(false);
#pragma GCC diagnostic pop

  // The same identity again in the field that replaced the deprecated ones. Newer
  // builds read this and older ones ignore it, so sending both costs a few bytes and
  // removes a whole class of "which schema does this phone speak" guessing.
  auto* head_unit = response->mutable_headunit_info();
  head_unit->set_make("Flutter");
  head_unit->set_model(description.car_model);
  head_unit->set_year(description.car_year);
  head_unit->set_vehicle_id(description.vehicle_id);
  head_unit->set_head_unit_make("Flutter");
  head_unit->set_head_unit_model(description.head_unit_name);
  head_unit->set_head_unit_software_build("1");
  head_unit->set_head_unit_software_version("1.0");

  // session_configuration is deliberately not set. It is absent from the schema
  // openauto used, and an explicit zero is not the same as an absent field on the wire.

  // Dumped in full at debug level, because this message decides everything that
  // follows and a phone that dislikes it says nothing at all: it simply drops out of
  // accessory mode. Turn it on with AA_LOG_LEVEL=DEBUG.
  AASDK_LOG(debug) << "[ServiceDiscovery] response:\n" << response->DebugString();
}

}  // namespace aa
