// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor_channel.h"

#include <cmath>

#include <aasdk/Common/Log.hpp>

#include <aap_protobuf/service/sensorsource/message/DrivingStatus.pb.h>
#include <aap_protobuf/service/sensorsource/message/Gear.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>

namespace aa {

namespace control_pb = aap_protobuf::service::control::message;
namespace sensor_pb = aap_protobuf::service::sensorsource::message;

sensor_pb::SensorType SensorTypeOf(Sensor sensor) {
  switch (sensor) {
    case Sensor::kNightMode:
      return sensor_pb::SENSOR_NIGHT_MODE;
    case Sensor::kDrivingStatus:
      return sensor_pb::SENSOR_DRIVING_STATUS_DATA;
    case Sensor::kLocation:
      return sensor_pb::SENSOR_LOCATION;
    case Sensor::kSpeed:
      return sensor_pb::SENSOR_SPEED;
    case Sensor::kRpm:
      return sensor_pb::SENSOR_RPM;
    case Sensor::kFuel:
      return sensor_pb::SENSOR_FUEL;
    case Sensor::kParkingBrake:
      return sensor_pb::SENSOR_PARKING_BRAKE;
    case Sensor::kGear:
      return sensor_pb::SENSOR_GEAR;
    case Sensor::kCompass:
      return sensor_pb::SENSOR_COMPASS;
    case Sensor::kEnvironment:
      return sensor_pb::SENSOR_ENVIRONMENT_DATA;
    case Sensor::kOdometer:
      return sensor_pb::SENSOR_ODOMETER;
    case Sensor::kTollCard:
      return sensor_pb::SENSOR_TOLL_CARD;
  }
  return sensor_pb::SENSOR_NIGHT_MODE;
}

namespace {

// The reverse, or false for a sensor this head unit has no notion of. The phone can ask
// for any of the twenty two types in the schema, including the ones nothing here
// reports, so this has to be able to say no.
bool SensorFromType(sensor_pb::SensorType type, Sensor* sensor) {
  for (int index = 0; index < kSensorCount; ++index) {
    const auto candidate = static_cast<Sensor>(index);
    if (SensorTypeOf(candidate) == type) {
      *sensor = candidate;
      return true;
    }
  }
  return false;
}

// The protocol scales every real number into a fixed point integer, named by the power
// of ten in the field: latitude_e7 is degrees times ten million, speed_e3 is metres per
// second times a thousand. Rounding rather than truncating, so a value set from Dart
// comes back out as the same number.
int32_t Scaled(double value, double factor) {
  return static_cast<int32_t>(std::llround(value * factor));
}

// Longest a held back reading is allowed to wait, whatever the phone asked for.
//
// The phone's min_update_period is trusted for the shape of the rate limit but not for
// its size: a number that turned out to be in some other unit would otherwise sit on
// the driving status for hours, and a stale driving status is exactly the thing that
// locks the user out of the interface.
constexpr std::chrono::milliseconds kMaxHoldBack{5000};

// What the phone asked for, as a duration.
//
// `min_update_period` is an int64 and the schema does not say what of. Microseconds is
// the reading taken here, and the raw number is logged on every subscription so the
// assumption is checkable against a real phone rather than believed. The one phone
// tested sent 0 for most sensors and 3 for speed and compass, and under the microsecond
// reading the limiter below has never fired against it. Zero means "as often as you
// like", which is what every subscription observed so far has asked for.
std::chrono::steady_clock::duration UpdatePeriod(int64_t raw) {
  if (raw <= 0) {
    return std::chrono::steady_clock::duration::zero();
  }
  const std::chrono::steady_clock::duration period = std::chrono::microseconds(raw);
  const std::chrono::steady_clock::duration cap = kMaxHoldBack;
  return period > cap ? cap : period;
}

}  // namespace

SensorRelay::SensorRelay(std::weak_ptr<SensorChannel> owner) : owner_(std::move(owner)) {}

void SensorRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto owner = owner_.lock()) {
    owner->OnOpen();
  }
}

void SensorRelay::onSensorStartRequest(const sensor_pb::SensorRequest& request) {
  if (auto owner = owner_.lock()) {
    owner->OnStartRequest(request);
  }
}

void SensorRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto owner = owner_.lock()) {
    owner->OnChannelError(error);
  }
}

std::shared_ptr<SensorChannel> SensorChannel::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger, std::shared_ptr<SensorState> state,
    SensorMask advertised, LogHandler log) {
  return std::make_shared<SensorChannel>(io_context, strand, std::move(messenger),
                                         std::move(state), advertised, std::move(log));
}

SensorChannel::SensorChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                             aasdk::messenger::IMessenger::Pointer messenger,
                             std::shared_ptr<SensorState> state, SensorMask advertised,
                             LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      state_(std::move(state)),
      advertised_(advertised),
      log_(std::move(log)),
      flush_timer_(io_context) {}

SensorChannel::~SensorChannel() { Stop(); }

void SensorChannel::Start() {
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (stopped_.load() || !messenger_) {
      return;
    }
    channel_ = std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
        strand_, messenger_);
    relay_ = std::make_shared<SensorRelay>(weak_from_this());
  }
  Listen();

  // Watching the state comes last, so nothing can be published at a channel that is
  // still being built.
  if (state_) {
    std::weak_ptr<SensorChannel> weak = weak_from_this();
    state_->SetListener([weak](Sensor sensor) {
      if (auto self = weak.lock()) {
        self->Publish(sensor);
      }
    });
  }
}

void SensorChannel::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  // First, so that a value set while the session is coming down cannot reach a channel
  // that is on its way out. The listener holds a weak reference anyway, this just saves
  // the trip.
  if (state_) {
    state_->SetListener(nullptr);
    // The subscriptions belonged to the connection that is going away. Leaving them up
    // would have the host app believe a phone that is no longer there is still reading.
    state_->ClearSubscriptions();
  }
  flush_timer_.cancel();
  // Moved out and destroyed after the lock is dropped, so a handler holding its own
  // reference on an io thread finishes against a live object.
  aasdk::channel::sensorsource::ISensorSourceService::Pointer channel;
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    channel = std::move(channel_);
    messenger_.reset();
  }
}

aasdk::channel::sensorsource::ISensorSourceService::Pointer SensorChannel::Channel()
    const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channel_mutex_);
  return channel_;
}

void SensorChannel::Listen() {
  if (auto channel = Channel()) {
    channel->receive(relay_);
  }
}

void SensorChannel::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

aasdk::channel::SendPromise::Pointer SensorChannel::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`: a rejection can arrive on an io_context thread long after the
  // connection that started it has gone.
  std::weak_ptr<SensorChannel> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

void SensorChannel::OnOpen() {
  if (auto channel = Channel()) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    channel->sendChannelOpenResponse(response, MakeSendPromise("sensor channel open"));
  }
  Listen();
}

void SensorChannel::OnStartRequest(const sensor_pb::SensorRequest& request) {
  auto channel = Channel();
  if (!channel) {
    Listen();
    return;
  }

  Sensor sensor = Sensor::kNightMode;
  const bool known = SensorFromType(request.type(), &sensor);
  const bool offered = known && (advertised_ & SensorBit(sensor)) != 0;

  sensor_pb::SensorStartResponseMessage response;
  response.set_status(offered ? aap_protobuf::shared::STATUS_SUCCESS
                              : aap_protobuf::shared::STATUS_INVALID_SENSOR);
  channel->sendSensorStartResponse(response, MakeSendPromise("sensor start"));

  if (!offered) {
    // Refused rather than accepted and then starved. A phone told no falls back to
    // whatever it can do itself, and for location that is the difference between
    // navigation working and navigation waiting for a fix that is never coming.
    Log("The phone asked for a sensor this head unit does not have (type " +
        std::to_string(static_cast<int>(request.type())) + "), refused.");
    Listen();
    return;
  }

  AASDK_LOG(debug) << "[Sensor] the phone subscribed to " << SensorName(sensor)
                   << ", min_update_period " << request.min_update_period();

  state_->NoteSubscribed(sensor);
  period_[static_cast<int>(sensor)] = UpdatePeriod(request.min_update_period());

  // The phone waits for a first reading before it will show anything that depends on
  // the sensor, so answering the subscription and then saying nothing is the same as
  // refusing it. Sent whatever the rate limit says, because this is the phone asking
  // rather than the car changing.
  Offer(sensor, /*ignore_rate_limit=*/true);
  Listen();
}

void SensorChannel::Publish(Sensor sensor) {
  // Posted rather than sent inline, for the reason InputChannel::SendReport gives: this
  // arrives on Flutter's platform thread and the strand is where the channel's own
  // receive handling runs, so going through it means a reading is never sent against a
  // channel the phone is in the middle of opening, and the rate limiter needs no lock.
  std::weak_ptr<SensorChannel> weak = weak_from_this();
  strand_.post([weak, sensor]() {
    if (auto self = weak.lock()) {
      self->Offer(sensor, /*ignore_rate_limit=*/false);
    }
  });
}

void SensorChannel::Offer(Sensor sensor, bool ignore_rate_limit) {
  if (stopped_.load() || !Channel()) {
    return;
  }
  const SensorMask bit = SensorBit(sensor);
  if (!state_ || (state_->subscriptions() & bit) == 0) {
    // Not subscribed. Nothing acknowledges a sensor batch, so a reading sent for a
    // sensor the phone never asked about would vanish exactly as an early input report
    // does.
    return;
  }
  if (!state_->Has(sensor)) {
    // Advertised but nothing in it yet, which is a car that has not got a fix rather
    // than a car at latitude zero. Nothing goes out until the host app says something.
    return;
  }

  const int index = static_cast<int>(sensor);
  const auto now = std::chrono::steady_clock::now();
  if (ignore_rate_limit || period_[index] == std::chrono::steady_clock::duration::zero() ||
      now - last_sent_[index] >= period_[index]) {
    Send(bit);
    return;
  }
  // Inside the phone's interval. Hold the sensor rather than dropping it: the value
  // that matters is the newest one, and it is already in the state, so marking the
  // sensor due is the whole of the work.
  due_ |= bit;
  ArmFlush();
}

void SensorChannel::ArmFlush() {
  if (flush_armed_ || due_ == 0) {
    return;
  }
  // The earliest moment any held sensor is allowed out. One timer for all of them,
  // because a flush sends everything that has come due in a single batch anyway.
  auto deadline = std::chrono::steady_clock::time_point::max();
  for (int index = 0; index < kSensorCount; ++index) {
    if ((due_ & SensorBit(static_cast<Sensor>(index))) == 0) {
      continue;
    }
    const auto ready = last_sent_[index] + period_[index];
    if (ready < deadline) {
      deadline = ready;
    }
  }

  flush_armed_ = true;
  flush_timer_.expires_at(deadline);
  // Bound to the strand, so the handler shares a thread with everything it touches and
  // the rate limiter's state needs no lock. A plain async_wait would run it on either
  // io thread, against an Offer() on the other.
  std::weak_ptr<SensorChannel> weak = weak_from_this();
  flush_timer_.async_wait(boost::asio::bind_executor(
      static_cast<aasdk::Strand::Base&>(strand_),
      [weak](const boost::system::error_code& error) {
        auto self = weak.lock();
        if (!self) {
          return;
        }
        self->flush_armed_ = false;
        if (error) {
          // Cancelled, which means the channel is going away. Whoever cancelled owns
          // the decision.
          return;
        }
        self->Flush();
      }));
}

void SensorChannel::Flush() {
  if (stopped_.load()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  SensorMask ready = 0;
  for (int index = 0; index < kSensorCount; ++index) {
    const SensorMask bit = SensorBit(static_cast<Sensor>(index));
    if ((due_ & bit) == 0) {
      continue;
    }
    if (now - last_sent_[index] >= period_[index]) {
      ready |= bit;
    }
  }
  if (ready != 0) {
    due_ &= ~ready;
    Send(ready);
  }
  // Anything still held has a later deadline than the one that just fired.
  ArmFlush();
}

void SensorChannel::Send(SensorMask which) {
  auto channel = Channel();
  if (!channel || !state_ || which == 0) {
    return;
  }
  // One snapshot for the whole batch, so a value cannot change between two of the
  // fields being filled in.
  const SensorValues values = state_->Snapshot();
  const auto now = std::chrono::steady_clock::now();

  sensor_pb::SensorBatch batch;
  bool filled = false;
  for (int index = 0; index < kSensorCount; ++index) {
    const auto sensor = static_cast<Sensor>(index);
    if ((which & SensorBit(sensor)) == 0 || !state_->Has(sensor)) {
      continue;
    }
    switch (sensor) {
      case Sensor::kNightMode:
        batch.add_night_mode_data()->set_night_mode(values.night_mode);
        break;
      case Sensor::kDrivingStatus:
        batch.add_driving_status_data()->set_status(values.driving_restrictions);
        break;
      case Sensor::kLocation: {
        auto* location = batch.add_location_data();
        location->set_latitude_e7(Scaled(values.location.latitude, 1e7));
        location->set_longitude_e7(Scaled(values.location.longitude, 1e7));
        // The four optional fields are left out entirely when the host app does not
        // know them. An absent field and a field set to zero are not the same thing on
        // the wire, and for a bearing the difference is due north.
        if (!std::isnan(values.location.accuracy_metres)) {
          location->set_accuracy_e3(
              static_cast<uint32_t>(Scaled(values.location.accuracy_metres, 1e3)));
        }
        if (!std::isnan(values.location.altitude_metres)) {
          location->set_altitude_e2(Scaled(values.location.altitude_metres, 1e2));
        }
        if (!std::isnan(values.location.speed_mps)) {
          location->set_speed_e3(Scaled(values.location.speed_mps, 1e3));
        }
        if (!std::isnan(values.location.bearing_degrees)) {
          location->set_bearing_e6(Scaled(values.location.bearing_degrees, 1e6));
        }
        break;
      }
      case Sensor::kSpeed:
        batch.add_speed_data()->set_speed_e3(Scaled(values.speed_mps, 1e3));
        break;
      case Sensor::kRpm:
        batch.add_rpm_data()->set_rpm_e3(Scaled(values.rpm, 1e3));
        break;
      case Sensor::kFuel: {
        auto* fuel = batch.add_fuel_data();
        fuel->set_fuel_level(Scaled(values.fuel_level, 1.0));
        fuel->set_range(Scaled(values.fuel_range_metres, 1.0));
        fuel->set_low_fuel_warning(values.fuel_low);
        break;
      }
      case Sensor::kParkingBrake:
        batch.add_parking_brake_data()->set_parking_brake(values.parking_brake);
        break;
      case Sensor::kGear:
        batch.add_gear_data()->set_gear(static_cast<sensor_pb::Gear>(values.gear));
        break;
      case Sensor::kCompass:
        batch.add_compass_data()->set_bearing_e6(
            Scaled(values.compass_bearing_degrees, 1e6));
        break;
      case Sensor::kEnvironment: {
        auto* environment = batch.add_environment_data();
        if (!std::isnan(values.temperature_celsius)) {
          environment->set_temperature_e3(Scaled(values.temperature_celsius, 1e3));
        }
        if (!std::isnan(values.pressure_kpa)) {
          environment->set_pressure_e3(Scaled(values.pressure_kpa, 1e3));
        }
        break;
      }
      case Sensor::kOdometer:
        batch.add_odometer_data()->set_kms_e1(Scaled(values.odometer_km, 1e1));
        break;
      case Sensor::kTollCard:
        batch.add_toll_card_data()->set_is_card_present(values.toll_card_present);
        break;
    }
    last_sent_[index] = now;
    filled = true;
  }

  if (!filled) {
    return;
  }
  state_->NoteBatchSent();
  channel->sendSensorEventIndication(batch, MakeSendPromise("sensor reading"));
}

void SensorChannel::OnChannelError(const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  // Already stopped: see the note on VideoChannel::onChannelError.
  if (stopped_.load()) {
    return;
  }
  Log(std::string("The sensor channel failed: ") + error.what());
}

}  // namespace aa
