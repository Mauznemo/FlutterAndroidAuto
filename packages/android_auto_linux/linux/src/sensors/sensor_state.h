// What this head unit currently believes about the car, and nothing about the protocol.
//
// The API agnostic seam for sensors, the same shape as audio/pcm_sink.h: no protobuf
// and no aasdk, so the values can be set before a phone has ever been plugged in and
// survive every reconnect. session/sensor_channel.cc is the only thing that turns them
// into wire messages.
//
// Every value here comes from the host app. This plugin owns no sensor hardware and
// invents no readings: a car's GPS, its parking brake and its light sensor belong to
// the vehicle the host app is running in, not to an Android Auto library. The two
// exceptions are the two that must have an answer before the phone will finish opening
// its UI, so they start at a default rather than at nothing: night mode is day and the
// driving status is unrestricted. See PLAN.md under M8.
//
// Threading: every setter is called from Flutter's platform thread, the listener runs
// on whichever thread set the value, and Snapshot() is called from the channel strand.
// The mutex covers all three.

#ifndef ANDROID_AUTO_LINUX_SENSORS_SENSOR_STATE_H_
#define ANDROID_AUTO_LINUX_SENSORS_SENSOR_STATE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace aa {

// The sensors this head unit can report. Mirrored by AaSensor in aa_core.h as a bit
// per entry, and by AndroidAutoSensor in Dart, so the order is part of the ABI.
enum class Sensor {
  kNightMode = 0,
  kDrivingStatus = 1,
  kLocation = 2,
  kSpeed = 3,
  kRpm = 4,
  kFuel = 5,
  kParkingBrake = 6,
  kGear = 7,
  kCompass = 8,
  kEnvironment = 9,
  kOdometer = 10,
  kTollCard = 11,
};

constexpr int kSensorCount = 12;

// A set of sensors, one bit each. What service discovery advertises and what the phone
// has subscribed to are both this.
using SensorMask = uint32_t;

constexpr SensorMask SensorBit(Sensor sensor) {
  return static_cast<SensorMask>(1u) << static_cast<int>(sensor);
}

// The two that are not optional. A head unit that cannot answer the driving status
// subscription leaves the phone with most of its interface locked, and one that cannot
// answer night mode leaves it guessing at its own theme.
constexpr SensorMask kRequiredSensors =
    SensorBit(Sensor::kNightMode) | SensorBit(Sensor::kDrivingStatus);

// For logging and for the name the host app sees in an error.
const char* SensorName(Sensor sensor);

// What the phone is allowed to do, as the restrictions the car puts on it.
//
// These are the DrivingStatus bits from the protocol, and they combine: parked is no
// bits set at all and a moving car is some combination of them. They are spelled out
// here so nothing outside sensor_channel.cc has to include the protobuf.
enum DrivingRestriction : int32_t {
  kRestrictNone = 0,
  kRestrictVideo = 1,
  kRestrictKeyboard = 2,
  kRestrictVoice = 4,
  kRestrictConfiguration = 8,
  kRestrictMessageLength = 16,
};

// A position fix.
//
// Latitude and longitude are always present; the rest are NaN when the host app does
// not know them, because every one of them has a meaningful zero. A bearing of zero is
// due north, an altitude of zero is sea level, and a speed of zero is standing still,
// so none of them can double as "no value".
struct Location {
  double latitude = 0.0;
  double longitude = 0.0;
  // Horizontal accuracy in metres, the radius of the circle the fix is somewhere in.
  double accuracy_metres = 0.0;
  double altitude_metres = 0.0;
  double speed_mps = 0.0;
  // Direction of travel in degrees clockwise from north.
  double bearing_degrees = 0.0;
};

// Everything the host app has told this head unit, in one copyable lump.
//
// Read as a whole under one lock rather than field by field, so a batch can never carry
// half of one update and half of the next.
struct SensorValues {
  bool night_mode = false;
  int32_t driving_restrictions = kRestrictNone;
  Location location;
  double speed_mps = 0.0;
  double rpm = 0.0;
  // Percentage of a full tank, 0 to 100.
  double fuel_level = 0.0;
  double fuel_range_metres = 0.0;
  bool fuel_low = false;
  bool parking_brake = false;
  // A protocol Gear value: 0 neutral, 1 to 10 the numbered gears, 100 drive, 101 park,
  // 102 reverse.
  int32_t gear = 0;
  double compass_bearing_degrees = 0.0;
  double temperature_celsius = 0.0;
  double pressure_kpa = 0.0;
  double odometer_km = 0.0;
  bool toll_card_present = false;
};

class SensorState {
 public:
  // Told which sensor changed, on the thread that changed it. Never called with the
  // state's lock held, so an implementation may call straight back in.
  using Listener = std::function<void(Sensor)>;

  SensorState();

  void SetNightMode(bool night);
  void SetDrivingRestrictions(int32_t restrictions);
  void SetLocation(const Location& location);
  void SetSpeed(double metres_per_second);
  void SetRpm(double rpm);
  void SetFuel(double level_percent, double range_metres, bool low);
  void SetParkingBrake(bool engaged);
  void SetGear(int32_t gear);
  void SetCompass(double bearing_degrees);
  void SetEnvironment(double temperature_celsius, double pressure_kpa);
  void SetOdometer(double kilometres);
  void SetTollCard(bool present);

  // Everything at once, for building a batch.
  SensorValues Snapshot() const;

  // Whether the host app has ever given this sensor a value.
  //
  // The difference between "the car has no fix yet" and "the car is at 0,0 off the
  // coast of Africa", which is why a sensor with nothing in it is not reported at all
  // rather than reported as zero. Night mode and the driving status are true from the
  // start: they have defensible defaults and the phone needs an answer at once.
  bool Has(Sensor sensor) const;

  // Which sensors have a value, as a mask.
  SensorMask available() const;

  // Installs the change listener, or clears it with nullptr. One at a time: there is
  // one live channel at a time, and it clears this on the way out.
  void SetListener(Listener listener);

  // === what the phone has done with all this ===
  //
  // Written by the channel and read from Flutter's platform thread, so they live here
  // rather than on the channel: the channel comes and goes with the connection and the
  // host app asking "is the phone reading my sensors" should not have to race it.

  // Which sensors the phone has subscribed to on the live connection. Never the same
  // question as which ones were advertised: a phone takes what it wants from the list,
  // and the difference is the first thing to look at when a value is being set and
  // nothing on the screen changes.
  SensorMask subscriptions() const { return subscriptions_.load(); }
  // Batches written since this head unit started. The "did anything actually go out"
  // number, which is otherwise only answerable by watching the phone's UI.
  uint64_t batches_sent() const { return batches_sent_.load(); }

  void NoteSubscribed(Sensor sensor);
  void ClearSubscriptions();
  void NoteBatchSent();

 private:
  // Marks the sensor as having a value and tells the listener. Must be called with the
  // lock released.
  void Publish(Sensor sensor);

  mutable std::mutex mutex_;
  SensorValues values_;
  SensorMask available_ = kRequiredSensors;
  Listener listener_;

  std::atomic<SensorMask> subscriptions_{0};
  std::atomic<uint64_t> batches_sent_{0};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SENSORS_SENSOR_STATE_H_
