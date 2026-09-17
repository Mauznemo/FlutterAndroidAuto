// SPDX-License-Identifier: GPL-3.0-or-later
// The INPUT_SOURCE channel: how the head unit tells the phone it was touched.
//
// This is the only channel that flows the other way. Video, audio and sensors are
// things the phone pushes and the head unit answers; input is the head unit talking
// unprompted, which makes the ordering different from every other channel here:
//
//   channel open request  ->  channel open response
//   key binding request   ->  key binding response        (the phone asking which
//                                                          hardware keys it may claim)
//   ... then nothing, until a finger lands ...
//   input report          ->  no reply, no acknowledgement
//
// Reports are fire and forget. The phone never acknowledges one, so a dropped report is
// invisible, which is why the send path refuses to run before the phone has opened the
// channel rather than sending into the void.
//
// Coordinates are in projected video pixels, the size advertised as the touchscreen
// during service discovery. They are not logical pixels and they are not normalised.
// Mapping a widget-local position onto them is the Dart side's job, in AndroidAutoView.

#ifndef ANDROID_AUTO_LINUX_SESSION_INPUT_CHANNEL_H_
#define ANDROID_AUTO_LINUX_SESSION_INPUT_CHANNEL_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>

#include <aasdk/Channel/InputSource/IInputSourceServiceEventHandler.hpp>
#include <aasdk/Channel/InputSource/InputSourceService.hpp>
#include <aasdk/Common/Strand.hpp>
#include <aasdk/Messenger/Messenger.hpp>

namespace aa {

class InputChannel;

// Forwards input channel events to an InputChannel without keeping it alive.
//
// Exactly the reason ControlEventRelay and VideoEventRelay exist: receive() binds the
// handler into a promise the messenger owns, so handing it the channel itself makes
// channel -> promise -> channel and nothing is ever destroyed. A session that is never
// destroyed never releases the USB interface.
class InputEventRelay : public aasdk::channel::inputsource::IInputSourceServiceEventHandler {
 public:
  explicit InputEventRelay(std::weak_ptr<InputChannel> channel);

  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request) override;
  void onKeyBindingRequest(
      const aap_protobuf::service::media::sink::message::KeyBindingRequest& request)
      override;
  void onChannelError(const aasdk::error::Error& error) override;

 private:
  std::weak_ptr<InputChannel> channel_;
};

// One finger, in projected video pixels.
struct TouchPoint {
  int32_t id = 0;
  int32_t x = 0;
  int32_t y = 0;
};

// What a touch report describes. The integers are Android's own MotionEvent action
// constants, which the protocol's PointerAction and the C ABI's AaTouchAction both
// reuse verbatim, so all three stay in step by having the same values rather than by
// anyone remembering to convert.
enum class TouchAction {
  kDown = 0,
  kUp = 1,
  kMoved = 2,
  kPointerDown = 5,
  kPointerUp = 6,
};

class InputChannel : public std::enable_shared_from_this<InputChannel> {
 public:
  using LogHandler = std::function<void(const std::string&)>;

  // `strand` must outlive every channel built on it, see the note on
  // ProtocolSession::Create. `width` and `height` are the touchscreen size advertised
  // during service discovery; reports are clamped to it, because a coordinate outside
  // the screen the head unit claimed to have is one more thing a phone can quietly
  // dislike.
  static std::shared_ptr<InputChannel> Create(boost::asio::io_context& io_context,
                                              aasdk::Strand& strand,
                                              aasdk::messenger::IMessenger::Pointer messenger,
                                              int32_t width, int32_t height,
                                              LogHandler log);

  InputChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
               aasdk::messenger::IMessenger::Pointer messenger, int32_t width,
               int32_t height, LogHandler log);
  ~InputChannel();

  // Arms the first receive.
  void Start();
  void Stop();

  // Whether the phone has opened the channel, which is when reports start being sent
  // rather than dropped.
  bool open() const { return open_.load(); }

  // The three senders below are the only things in this file called from outside the
  // io_context: they run on Flutter's platform thread, straight out of a pointer
  // callback. Each one posts the actual send onto the channel strand, so everything
  // that touches the channel still happens on one thread.

  // `points` carries every finger currently down, in the order the phone should see
  // them. `action_index` is the index within `points` of the finger this report is
  // about, which only matters for the pointer down and pointer up actions.
  //
  // Movement is rate limited, see kMoveInterval. Everything else goes out at once.
  void SendTouch(TouchAction action, int32_t action_index, std::vector<TouchPoint> points);

  // One hardware key transition. Down and up are separate reports, as they are on
  // Android: a phone that gets a down and no up thinks the button is still held.
  void SendKey(int32_t keycode, bool down, bool long_press);

  // A rotary encoder detent, positive clockwise. Sent as a relative event rather than a
  // key, which is what makes the phone scroll a list by steps instead of treating each
  // detent as a button press.
  void SendRotary(int32_t steps);

  // Called by InputEventRelay, never by aasdk directly.
  void onChannelOpenRequest(
      const aap_protobuf::service::control::message::ChannelOpenRequest& request);
  void onKeyBindingRequest(
      const aap_protobuf::service::media::sink::message::KeyBindingRequest& request);
  void onChannelError(const aasdk::error::Error& error);

 private:
  // The shortest gap between two movement reports.
  //
  // Without this the head unit emits one transport write per pointer event it is
  // handed, which on a desktop mouse measured 382 a second: six times the rate at which
  // the phone can possibly show the result, since what comes back is a video stream of
  // at most 60 frames a second. Nothing is lost by holding the extra ones. Movement is the
  // only action rate limited, because it is the only one that repeats; a finger landing
  // or lifting happens once and has to go immediately.
  //
  // Coalescing rather than dropping: the newest position replaces the held one, so the
  // phone always gets where the finger actually is, never a stale sample.
  static constexpr std::chrono::milliseconds kMoveInterval{16};

  // A reference to the live channel, or nullptr once stopped.
  //
  // This is the piece that makes the class safe to send from two threads. Every other
  // channel in this session is only ever touched from the io_context, so it can hold
  // its aasdk channel in a plain member; this one is reached from Flutter's platform
  // thread on every finger movement, while Stop() can be running on either thread. A
  // caller takes its own reference and works from that, so a teardown mid send drops
  // the channel when the last reference goes rather than out from under whoever is
  // using it.
  aasdk::channel::inputsource::IInputSourceService::Pointer Channel() const;

  void Listen();
  // `coalesce` marks a report that may wait for kMoveInterval and be replaced by a
  // newer one in the meantime.
  void SendReport(aap_protobuf::service::inputsource::message::InputReport report,
                  const char* what, bool coalesce);
  // Strand only. Writes the report out and starts the rate limit interval.
  void Deliver(const aap_protobuf::service::inputsource::message::InputReport& report,
               const char* what);
  // Strand only. Sends whatever movement is being held, if the interval has passed.
  void FlushPendingMove();
  aasdk::channel::SendPromise::Pointer MakeSendPromise(const char* what);
  void Log(const std::string& message);

  boost::asio::io_context& io_context_;
  aasdk::Strand& strand_;
  aasdk::messenger::IMessenger::Pointer messenger_;
  int32_t width_;
  int32_t height_;
  LogHandler log_;

  // Guards channel_ and messenger_ only. Held for the length of a pointer copy, never
  // across a send.
  mutable std::mutex channel_mutex_;
  aasdk::channel::inputsource::IInputSourceService::Pointer channel_;
  std::shared_ptr<InputEventRelay> relay_;

  std::atomic<bool> open_{false};
  std::atomic<bool> stopped_{false};

  // The rate limiter. Touched only on the strand, so no lock.
  boost::asio::steady_timer move_timer_;
  std::optional<aap_protobuf::service::inputsource::message::InputReport> pending_move_;
  std::chrono::steady_clock::time_point last_sent_{};
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_SESSION_INPUT_CHANNEL_H_
