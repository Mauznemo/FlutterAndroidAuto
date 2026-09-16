// The SENSOR channel: what the head unit tells the phone about the car.
//
// The exchange is short and the phone drives all of it:
//
//   channel open request  ->  channel open response
//   sensor start request  ->  start response, then the first reading
//   ... a batch whenever a value changes, for as long as the connection lasts ...
//
// Three things make this channel unlike the others here.
//
// **It is load bearing in a way nothing else is.** Android Auto locks most of its
// interface until the head unit has answered the driving status subscription, so a
// sensor channel that says nothing is not a head unit missing a feature, it is a head
// unit showing a phone screen that says the car is not ready.
//
// **The head unit speaks unprompted, like the input channel.** A reading goes out when
// the host app changes a value, not when the phone asks for one, so the same rules
// apply: the value arrives on Flutter's platform thread, the send happens on the
// channel strand, and a report sent before the phone subscribed is refused rather than
// dropped into the void.
//
// **Advertising a sensor is a promise to have one.** The phone will use the head unit's
// position instead of its own the moment location is advertised, so a head unit that
// offers it and then never sends a fix has taken navigation away from a phone that was
// doing it perfectly well. Which sensors are advertised is the host app's declaration
// of what the car actually has, see HeadUnitDescription::sensors.
//
// This lived in support_channels.cc until M8, answering the phone with a fixed reading.

#ifndef ANDROID_AUTO_LINUX_SESSION_SENSOR_CHANNEL_H_
#define ANDROID_AUTO_LINUX_SESSION_SENSOR_CHANNEL_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>
#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>
#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#include "../sensors/sensor_state.h"

namespace aa {

// The protocol's name for one of our sensors. Lives here rather than in
// service_discovery.cc because the list it has to agree with is the switch in
// sensor_channel.cc that fills the batch: a sensor advertised under one type and sent
// under another is a subscription that is answered and then never fed.
aap_protobuf::service::sensorsource::message::SensorType SensorTypeOf(Sensor sensor);

class SensorChannel;

// Forwards the channel's events without keeping the owner alive.
//
// The reason ControlEventRelay exists, for the same machinery: receive() binds the
// handler into a promise the messenger owns, so a handler that owns the channel makes a
// cycle nothing can break, and a session that never dies never releases the USB
// interface.
class SensorRelay : public aasdk::channel::sensorsource::ISensorSourceServiceEventHandler {
 public:
  explicit SensorRelay(std::weak_ptr<SensorChannel> owner);

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override;
  void onSensorStartRequest(
      const aap_protobuf::service::sensorsource::message::SensorRequest& request) override;
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<SensorChannel> owner_;
};

class SensorChannel : public std::enable_shared_from_this<SensorChannel> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. `state` outlives the session the way the audio output and
  // the microphone do: what the car is doing does not stop being true because a phone
  // was unplugged.
  //
  // `advertised` is the set service discovery offered. A subscription to anything
  // outside it is refused rather than quietly accepted, because a phone waiting for a
  // reading that is never coming is worse off than one told at once that it will not
  // get it.
  static std::shared_ptr<SensorChannel> Create(boost::asio::io_context& io_context,
                                               aasdk::Strand& strand,
                                               aasdk::messenger::IMessenger::Pointer messenger,
                                               std::shared_ptr<SensorState> state,
                                               SensorMask advertised, LogHandler log);

  SensorChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                aasdk::messenger::IMessenger::Pointer messenger,
                std::shared_ptr<SensorState> state, SensorMask advertised,
                LogHandler log);
  ~SensorChannel();

  // Creates the channel, arms the first receive and starts watching the state.
  void Start();
  // Stops watching the state, then drops the channel. Safe from any thread.
  void Stop();

  // Called by SensorRelay, never by aasdk directly.
  void OnOpen();
  void OnStartRequest(
      const aap_protobuf::service::sensorsource::message::SensorRequest& request);
  void OnChannelError(const aasdk::error::Error& error);

 private:
  // A reference to the live channel, or nullptr once stopped.
  //
  // The same rule as InputChannel::Channel(), and for the same reason: handlers run on
  // the io_context while a value can arrive from Flutter's platform thread and Stop()
  // can come from either, so a caller takes its own reference and works from that.
  aasdk::channel::sensorsource::ISensorSourceService::Pointer Channel() const;

  void Listen();
  // Any thread. Posts the send onto the strand.
  void Publish(Sensor sensor);
  // Strand only. Sends the sensor now if its rate limit allows, or marks it due and
  // arms the flush timer if it does not.
  void Offer(Sensor sensor, bool ignore_rate_limit);
  // Strand only. Writes one batch carrying every sensor in `which` that has a value.
  void Send(SensorMask which);
  // Strand only. Sends whatever has come due and re-arms if anything has not.
  void Flush();
  // Strand only. Schedules the next flush, if anything is being held.
  void ArmFlush();
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  std::shared_ptr<SensorState> state_;
  SensorMask advertised_;
  LogHandler log_;

  // Guards channel_ and messenger_. Held for the length of a pointer copy, never across
  // a send.
  mutable std::mutex channel_mutex_;
  aasdk::channel::sensorsource::ISensorSourceService::Pointer channel_;
  std::shared_ptr<SensorRelay> relay_;

  // What the phone has asked for. Kept on the state rather than here, because the host
  // app reads it from Flutter's platform thread and this object only exists while a
  // phone does; see SensorState::subscriptions().
  std::atomic<bool> stopped_{false};

  // Strand only, all five. The rate limiter: what the phone asked for per sensor, when
  // each last went out, and which are waiting for their next slot. The timer's handler
  // is bound to the strand as well, so these need no lock.
  std::array<std::chrono::steady_clock::duration, kSensorCount> period_{};
  std::array<std::chrono::steady_clock::time_point, kSensorCount> last_sent_{};
  SensorMask due_ = 0;
  boost::asio::steady_timer flush_timer_;
  bool flush_armed_ = false;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_SENSOR_CHANNEL_H_
