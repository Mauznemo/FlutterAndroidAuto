#include "support_channels.h"

#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>
#include <aap_protobuf/service/sensorsource/message/DrivingStatus.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace media_pb = aap_protobuf::service::media::shared::message;
namespace source_pb = aap_protobuf::service::media::source::message;
namespace sensor_pb = aap_protobuf::service::sensorsource::message;

// How many audio buffers the phone may have in flight. Ten is what every other
// implementation asks for, and it matters even while the samples are discarded: the
// phone stops sending once it hits the limit, and a stalled audio channel is one more
// thing that can make it give up on the session.
constexpr uint32_t kAudioMaxUnacked = 10;

const char* ChannelName(aasdk::messenger::ChannelId channel) {
  switch (channel) {
    case aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO:
      return "media audio";
    case aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO:
      return "system audio";
    case aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO:
      return "speech audio";
    default:
      return "audio";
  }
}

}  // namespace

AudioSinkRelay::AudioSinkRelay(std::weak_ptr<SupportChannels> owner,
                               aasdk::messenger::ChannelId channel)
    : owner_(std::move(owner)), channel_(channel) {}

void AudioSinkRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto owner = owner_.lock()) {
    owner->OnAudioOpen(channel_);
  }
}

void AudioSinkRelay::onMediaChannelSetupRequest(const media_pb::Setup&) {
  if (auto owner = owner_.lock()) {
    owner->OnAudioSetup(channel_);
  }
}

void AudioSinkRelay::onMediaChannelStartIndication(const media_pb::Start& indication) {
  if (auto owner = owner_.lock()) {
    owner->OnAudioStart(channel_, indication.session_id());
  }
}

void AudioSinkRelay::onMediaChannelStopIndication(const media_pb::Stop&) {
  if (auto owner = owner_.lock()) {
    owner->OnAudioStop(channel_);
  }
}

void AudioSinkRelay::onMediaWithTimestampIndication(
    aasdk::messenger::Timestamp::ValueType, const aasdk::common::DataConstBuffer&) {
  if (auto owner = owner_.lock()) {
    owner->OnAudioData(channel_);
  }
}

void AudioSinkRelay::onMediaIndication(const aasdk::common::DataConstBuffer&) {
  if (auto owner = owner_.lock()) {
    owner->OnAudioData(channel_);
  }
}

void AudioSinkRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto owner = owner_.lock()) {
    owner->OnChannelError(ChannelName(channel_), error);
  }
}

MicrophoneRelay::MicrophoneRelay(std::weak_ptr<SupportChannels> owner)
    : owner_(std::move(owner)) {}

void MicrophoneRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto owner = owner_.lock()) {
    owner->OnMicrophoneOpen();
  }
}

void MicrophoneRelay::onMediaChannelSetupRequest(const media_pb::Setup&) {
  if (auto owner = owner_.lock()) {
    owner->OnMicrophoneSetup();
  }
}

void MicrophoneRelay::onMediaSourceOpenRequest(const source_pb::MicrophoneRequest& request) {
  if (auto owner = owner_.lock()) {
    owner->OnMicrophoneRequest(request.open());
  }
}

void MicrophoneRelay::onMediaChannelAckIndication(const source_pb::Ack&) {
  if (auto owner = owner_.lock()) {
    owner->OnMicrophoneAck();
  }
}

void MicrophoneRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto owner = owner_.lock()) {
    owner->OnChannelError("microphone", error);
  }
}

SensorRelay::SensorRelay(std::weak_ptr<SupportChannels> owner)
    : owner_(std::move(owner)) {}

void SensorRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto owner = owner_.lock()) {
    owner->OnSensorOpen();
  }
}

void SensorRelay::onSensorStartRequest(const sensor_pb::SensorRequest& request) {
  if (auto owner = owner_.lock()) {
    owner->OnSensorStartRequest(request);
  }
}

void SensorRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto owner = owner_.lock()) {
    owner->OnChannelError("sensor", error);
  }
}

std::shared_ptr<SupportChannels> SupportChannels::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger,
    const HeadUnitDescription& description, LogHandler log) {
  return std::make_shared<SupportChannels>(io_context, strand, std::move(messenger),
                                           description, std::move(log));
}

SupportChannels::SupportChannels(boost::asio::io_context& io_context,
                                 aasdk::Strand& strand,
                                 aasdk::messenger::IMessenger::Pointer messenger,
                                 const HeadUnitDescription& description, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      description_(description),
      log_(std::move(log)) {}

SupportChannels::~SupportChannels() { Stop(); }

void SupportChannels::AddAudioSink(aasdk::messenger::ChannelId channel) {
  {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    if (stopped_.load() || !messenger_) {
      return;
    }
    AudioSink sink;
    sink.channel =
        std::make_shared<aasdk::channel::mediasink::audio::AudioMediaSinkService>(
            strand_, messenger_, channel);
    sink.relay = std::make_shared<AudioSinkRelay>(weak_from_this(), channel);
    audio_.emplace(channel, std::move(sink));
  }
  ListenAudio(channel);
}

void SupportChannels::AddMicrophone() {
  {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    if (stopped_.load() || !messenger_) {
      return;
    }
    microphone_ = std::make_shared<aasdk::channel::mediasource::MediaSourceService>(
        strand_, messenger_, aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE);
    microphone_relay_ = std::make_shared<MicrophoneRelay>(weak_from_this());
  }
  ListenMicrophone();
}

void SupportChannels::AddSensor() {
  {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    if (stopped_.load() || !messenger_) {
      return;
    }
    sensor_ = std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
        strand_, messenger_);
    sensor_relay_ = std::make_shared<SensorRelay>(weak_from_this());
  }
  ListenSensor();
}

void SupportChannels::Start() {
  if (description_.enable_media_audio) {
    AddAudioSink(aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO);
  }
  if (description_.enable_system_audio) {
    AddAudioSink(aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO);
  }
  if (description_.enable_speech_audio) {
    AddAudioSink(aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO);
  }
  if (description_.enable_microphone) {
    AddMicrophone();
  }
  if (description_.enable_sensors) {
    AddSensor();
  }
}

void SupportChannels::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  // Moved out and destroyed after the lock is dropped, so a handler holding its own
  // references on an io thread finishes against live objects.
  std::map<aasdk::messenger::ChannelId, AudioSink> audio;
  aasdk::channel::mediasource::IMediaSourceService::Pointer microphone;
  aasdk::channel::sensorsource::ISensorSourceService::Pointer sensor;
  {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    audio.swap(audio_);
    microphone = std::move(microphone_);
    sensor = std::move(sensor_);
    messenger_.reset();
  }
}

SupportChannels::AudioSink SupportChannels::Audio(
    aasdk::messenger::ChannelId channel) const {
  if (stopped_.load()) {
    return AudioSink{};
  }
  std::lock_guard<std::mutex> lock(channels_mutex_);
  auto found = audio_.find(channel);
  return found == audio_.end() ? AudioSink{} : found->second;
}

aasdk::channel::mediasource::IMediaSourceService::Pointer SupportChannels::Microphone()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channels_mutex_);
  return microphone_;
}

aasdk::channel::sensorsource::ISensorSourceService::Pointer SupportChannels::Sensor()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channels_mutex_);
  return sensor_;
}

void SupportChannels::SetAudioSession(aasdk::messenger::ChannelId channel,
                                      int32_t session_id) {
  std::lock_guard<std::mutex> lock(channels_mutex_);
  auto found = audio_.find(channel);
  if (found != audio_.end()) {
    found->second.session_id = session_id;
  }
}

void SupportChannels::ListenAudio(aasdk::messenger::ChannelId channel) {
  AudioSink sink = Audio(channel);
  if (sink.channel) {
    sink.channel->receive(sink.relay);
  }
}

void SupportChannels::ListenMicrophone() {
  if (auto microphone = Microphone()) {
    microphone->receive(microphone_relay_);
  }
}

void SupportChannels::ListenSensor() {
  if (auto sensor = Sensor()) {
    sensor->receive(sensor_relay_);
  }
}

void SupportChannels::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

aasdk::channel::SendPromise::Pointer SupportChannels::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`: a rejection can arrive on an io_context thread long after the
  // connection that started it has gone.
  std::weak_ptr<SupportChannels> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

void SupportChannels::OnAudioOpen(aasdk::messenger::ChannelId channel) {
  AudioSink sink = Audio(channel);
  if (sink.channel) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    sink.channel->sendChannelOpenResponse(response,
                                          MakeSendPromise("audio channel open"));
  }
  ListenAudio(channel);
}

void SupportChannels::OnAudioSetup(aasdk::messenger::ChannelId channel) {
  AudioSink sink = Audio(channel);
  if (sink.channel) {
    media_pb::Config response;
    response.set_status(media_pb::Config::STATUS_READY);
    response.set_max_unacked(kAudioMaxUnacked);
    response.add_configuration_indices(0);
    sink.channel->sendChannelSetupResponse(response, MakeSendPromise("audio setup"));
  }
  ListenAudio(channel);
}

void SupportChannels::OnAudioStart(aasdk::messenger::ChannelId channel,
                                   int32_t session_id) {
  SetAudioSession(channel, session_id);
  ListenAudio(channel);
}

void SupportChannels::OnAudioStop(aasdk::messenger::ChannelId channel) {
  SetAudioSession(channel, -1);
  ListenAudio(channel);
}

void SupportChannels::OnAudioData(aasdk::messenger::ChannelId channel) {
  // The samples are dropped on the floor until M6. The acknowledgement is not optional
  // though: without it the phone runs out of unacked buffers and stops sending, and a
  // stalled channel is one more reason for it to end the session.
  AudioSink sink = Audio(channel);
  if (sink.channel && sink.session_id >= 0) {
    source_pb::Ack ack;
    ack.set_session_id(sink.session_id);
    ack.set_ack(1);
    sink.channel->sendMediaAckIndication(ack, MakeSendPromise("audio ack"));
  }
  ListenAudio(channel);
}

void SupportChannels::OnMicrophoneOpen() {
  if (auto microphone = Microphone()) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    microphone->sendChannelOpenResponse(response,
                                        MakeSendPromise("microphone channel open"));
  }
  ListenMicrophone();
}

void SupportChannels::OnMicrophoneSetup() {
  if (auto microphone = Microphone()) {
    media_pb::Config response;
    response.set_status(media_pb::Config::STATUS_READY);
    response.set_max_unacked(kAudioMaxUnacked);
    response.add_configuration_indices(0);
    microphone->sendChannelSetupResponse(response, MakeSendPromise("microphone setup"));
  }
  ListenMicrophone();
}

void SupportChannels::OnMicrophoneRequest(bool open) {
  if (auto microphone = Microphone()) {
    source_pb::MicrophoneResponse response;
    response.set_status(0);
    response.set_session_id(++microphone_session_);
    microphone->sendMicrophoneOpenResponse(response,
                                           MakeSendPromise("microphone open response"));
    if (open) {
      // Answered, but nothing will follow. Say so rather than leaving a silent
      // Assistant looking like a bug in the projection.
      Log("The phone asked for the microphone. There is no capture until M7, so voice "
          "input will not work.");
    }
  }
  ListenMicrophone();
}

void SupportChannels::OnMicrophoneAck() { ListenMicrophone(); }

void SupportChannels::OnSensorOpen() {
  if (auto sensor = Sensor()) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    sensor->sendChannelOpenResponse(response, MakeSendPromise("sensor channel open"));
  }
  ListenSensor();
}

void SupportChannels::OnSensorStartRequest(const sensor_pb::SensorRequest& request) {
  auto sensor = Sensor();
  if (!sensor) {
    ListenSensor();
    return;
  }

  sensor_pb::SensorStartResponseMessage response;
  response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
  sensor->sendSensorStartResponse(response, MakeSendPromise("sensor start"));

  // The phone waits for a first reading before it will show anything that depends on
  // the sensor, so answering the subscription and then saying nothing is the same as
  // refusing it. Driving status in particular gates most of the interface.
  sensor_pb::SensorBatch batch;
  bool has_data = false;
  if (request.type() == sensor_pb::SENSOR_DRIVING_STATUS_DATA) {
    // Unrestricted: the host app has no way to report the car's real state until M8,
    // and claiming to be moving would lock the user out of an interface they are
    // sitting in front of.
    batch.add_driving_status_data()->set_status(sensor_pb::DRIVE_STATUS_UNRESTRICTED);
    has_data = true;
  } else if (request.type() == sensor_pb::SENSOR_NIGHT_MODE) {
    batch.add_night_mode_data()->set_night_mode(false);
    has_data = true;
  }
  if (has_data) {
    sensor->sendSensorEventIndication(batch, MakeSendPromise("sensor reading"));
  }
  ListenSensor();
}

void SupportChannels::OnChannelError(const std::string& what,
                                     const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  Log("The " + what + " channel failed: " + error.what());
}

}  // namespace aa
