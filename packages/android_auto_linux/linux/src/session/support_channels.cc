#include "support_channels.h"

#include <aap_protobuf/service/sensorsource/message/DrivingStatus.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace sensor_pb = aap_protobuf::service::sensorsource::message;

}  // namespace

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
  if (description_.enable_sensors) {
    AddSensor();
  }
}

void SupportChannels::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  // Moved out and destroyed after the lock is dropped, so a handler holding its own
  // reference on an io thread finishes against a live object.
  aasdk::channel::sensorsource::ISensorSourceService::Pointer sensor;
  {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    sensor = std::move(sensor_);
    messenger_.reset();
  }
}

aasdk::channel::sensorsource::ISensorSourceService::Pointer SupportChannels::Sensor()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channels_mutex_);
  return sensor_;
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
  // Already stopped: see the note on VideoChannel::onChannelError.
  if (stopped_.load()) {
    return;
  }
  Log("The " + what + " channel failed: " + error.what());
}

}  // namespace aa
