#include "sensor_state.h"

namespace aa {

const char* SensorName(Sensor sensor) {
  switch (sensor) {
    case Sensor::kNightMode:
      return "night mode";
    case Sensor::kDrivingStatus:
      return "driving status";
    case Sensor::kLocation:
      return "location";
    case Sensor::kSpeed:
      return "speed";
    case Sensor::kRpm:
      return "rpm";
    case Sensor::kFuel:
      return "fuel";
    case Sensor::kParkingBrake:
      return "parking brake";
    case Sensor::kGear:
      return "gear";
    case Sensor::kCompass:
      return "compass";
    case Sensor::kEnvironment:
      return "environment";
    case Sensor::kOdometer:
      return "odometer";
    case Sensor::kTollCard:
      return "toll card";
  }
  return "unknown sensor";
}

SensorState::SensorState() = default;

void SensorState::SetNightMode(bool night) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.night_mode = night;
  }
  Publish(Sensor::kNightMode);
}

void SensorState::SetDrivingRestrictions(int32_t restrictions) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.driving_restrictions = restrictions;
  }
  Publish(Sensor::kDrivingStatus);
}

void SensorState::SetLocation(const Location& location) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.location = location;
  }
  Publish(Sensor::kLocation);
}

void SensorState::SetSpeed(double metres_per_second) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.speed_mps = metres_per_second;
  }
  Publish(Sensor::kSpeed);
}

void SensorState::SetRpm(double rpm) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.rpm = rpm;
  }
  Publish(Sensor::kRpm);
}

void SensorState::SetFuel(double level_percent, double range_metres, bool low) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.fuel_level = level_percent;
    values_.fuel_range_metres = range_metres;
    values_.fuel_low = low;
  }
  Publish(Sensor::kFuel);
}

void SensorState::SetParkingBrake(bool engaged) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.parking_brake = engaged;
  }
  Publish(Sensor::kParkingBrake);
}

void SensorState::SetGear(int32_t gear) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.gear = gear;
  }
  Publish(Sensor::kGear);
}

void SensorState::SetCompass(double bearing_degrees) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.compass_bearing_degrees = bearing_degrees;
  }
  Publish(Sensor::kCompass);
}

void SensorState::SetEnvironment(double temperature_celsius, double pressure_kpa) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.temperature_celsius = temperature_celsius;
    values_.pressure_kpa = pressure_kpa;
  }
  Publish(Sensor::kEnvironment);
}

void SensorState::SetOdometer(double kilometres) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.odometer_km = kilometres;
  }
  Publish(Sensor::kOdometer);
}

void SensorState::SetTollCard(bool present) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.toll_card_present = present;
  }
  Publish(Sensor::kTollCard);
}

SensorValues SensorState::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return values_;
}

bool SensorState::Has(Sensor sensor) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (available_ & SensorBit(sensor)) != 0;
}

SensorMask SensorState::available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return available_;
}

void SensorState::NoteSubscribed(Sensor sensor) { subscriptions_ |= SensorBit(sensor); }

void SensorState::ClearSubscriptions() { subscriptions_ = 0; }

void SensorState::NoteBatchSent() { ++batches_sent_; }

void SensorState::SetListener(Listener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = std::move(listener);
}

void SensorState::Publish(Sensor sensor) {
  Listener listener;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    available_ |= SensorBit(sensor);
    // Copied out rather than called in place. The listener posts onto the channel
    // strand, which is cheap, but holding this lock across anything that is not this
    // object's own business is how a deadlock gets written by accident.
    listener = listener_;
  }
  if (listener) {
    listener(sensor);
  }
}

}  // namespace aa
