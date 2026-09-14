#include "input_channel.h"

#include <algorithm>
#include <string>

#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/PointerAction.pb.h>
#include <aap_protobuf/service/media/sink/message/KeyCode.pb.h>

#include <aasdk/Common/Log.hpp>

#include "../video/video_decoder.h"

namespace aa {
namespace {

namespace control_pb = aap_protobuf::service::control::message;
namespace input_pb = aap_protobuf::service::inputsource::message;
namespace sink_pb = aap_protobuf::service::media::sink::message;

// The rotary encoder is a relative axis rather than a key, and the protocol addresses
// it by the same code it uses for the key form. openauto calls this SCROLL_WHEEL.
constexpr int32_t kRotaryKeycode = sink_pb::KEYCODE_ROTARY_CONTROLLER;

input_pb::PointerAction ToProto(TouchAction action) {
  switch (action) {
    case TouchAction::kUp:
      return input_pb::ACTION_UP;
    case TouchAction::kMoved:
      return input_pb::ACTION_MOVED;
    case TouchAction::kPointerDown:
      return input_pb::ACTION_POINTER_DOWN;
    case TouchAction::kPointerUp:
      return input_pb::ACTION_POINTER_UP;
    case TouchAction::kDown:
    default:
      return input_pb::ACTION_DOWN;
  }
}

const char* ActionName(TouchAction action) {
  switch (action) {
    case TouchAction::kUp:
      return "up";
    case TouchAction::kMoved:
      return "move";
    case TouchAction::kPointerDown:
      return "pointer down";
    case TouchAction::kPointerUp:
      return "pointer up";
    case TouchAction::kDown:
    default:
      return "down";
  }
}

}  // namespace

InputEventRelay::InputEventRelay(std::weak_ptr<InputChannel> channel)
    : channel_(std::move(channel)) {}

void InputEventRelay::onChannelOpenRequest(const control_pb::ChannelOpenRequest& request) {
  if (auto channel = channel_.lock()) {
    channel->onChannelOpenRequest(request);
  }
}

void InputEventRelay::onKeyBindingRequest(const sink_pb::KeyBindingRequest& request) {
  if (auto channel = channel_.lock()) {
    channel->onKeyBindingRequest(request);
  }
}

void InputEventRelay::onChannelError(const aasdk::error::Error& error) {
  if (auto channel = channel_.lock()) {
    channel->onChannelError(error);
  }
}

std::shared_ptr<InputChannel> InputChannel::Create(
    boost::asio::io_context& io_context, aasdk::Strand& strand,
    aasdk::messenger::IMessenger::Pointer messenger, int32_t width, int32_t height,
    LogHandler log) {
  return std::make_shared<InputChannel>(io_context, strand, std::move(messenger), width,
                                        height, std::move(log));
}

InputChannel::InputChannel(boost::asio::io_context& io_context, aasdk::Strand& strand,
                           aasdk::messenger::IMessenger::Pointer messenger, int32_t width,
                           int32_t height, LogHandler log)
    : io_context_(io_context),
      strand_(strand),
      messenger_(std::move(messenger)),
      width_(width > 0 ? width : 1),
      height_(height > 0 ? height : 1),
      log_(std::move(log)),
      move_timer_(io_context) {}

InputChannel::~InputChannel() { Stop(); }

void InputChannel::Start() {
  relay_ = std::make_shared<InputEventRelay>(weak_from_this());
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    // No messenger means the session was torn down while it was being built. A channel
    // made now would hold a null messenger and segfault on its first receive, which is
    // exactly the crash a quick start then stop used to produce.
    if (stopped_.load() || !messenger_) {
      return;
    }
    channel_ = std::make_shared<aasdk::channel::inputsource::InputSourceService>(
        strand_, messenger_);
  }
  Listen();
}

void InputChannel::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  open_ = false;
  // Moved out and destroyed after the lock is dropped, so a send holding its own
  // reference on another thread finishes against a live object.
  aasdk::channel::inputsource::IInputSourceService::Pointer channel;
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    channel = std::move(channel_);
    messenger_.reset();
  }
  move_timer_.cancel();
}

aasdk::channel::inputsource::IInputSourceService::Pointer InputChannel::Channel() const {
  if (stopped_.load()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(channel_mutex_);
  return channel_;
}

void InputChannel::Listen() {
  if (auto channel = Channel()) {
    channel->receive(relay_);
  }
}

void InputChannel::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

aasdk::channel::SendPromise::Pointer InputChannel::MakeSendPromise(const char* what) {
  auto promise = aasdk::channel::SendPromise::defer(io_context_);
  const std::string label(what);
  // Weak, not `this`. A send can be rejected on an io_context thread long after the
  // connection that started it has gone.
  std::weak_ptr<InputChannel> weak = weak_from_this();
  promise->then([]() {},
                [weak, label](const aasdk::error::Error& error) {
                  if (auto self = weak.lock()) {
                    self->Log("Failed to send " + label + ": " + error.what());
                  }
                });
  return promise;
}

void InputChannel::onChannelOpenRequest(const control_pb::ChannelOpenRequest&) {
  if (auto channel = Channel()) {
    control_pb::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    channel->sendChannelOpenResponse(response, MakeSendPromise("input channel open"));
    open_ = true;
    Log("The phone opened the input channel.");
  }
  Listen();
}

void InputChannel::onKeyBindingRequest(const sink_pb::KeyBindingRequest& request) {
  // The phone is asking to claim some of the keys service discovery advertised, so it
  // can route them itself rather than letting the head unit's own UI have them. There
  // is nothing to refuse: the head unit only sends a key when the host app asks it to.
  std::string codes;
  for (int index = 0; index < request.keycodes_size(); ++index) {
    codes += (index == 0 ? "" : ", ") + std::to_string(request.keycodes(index));
  }
  AASDK_LOG(debug) << "[Input] the phone bound keycodes: " << codes;

  if (auto channel = Channel()) {
    sink_pb::KeyBindingResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    channel->sendKeyBindingResponse(response, MakeSendPromise("key binding"));
  }
  Listen();
}

void InputChannel::SendReport(input_pb::InputReport report, const char* what,
                              bool coalesce) {
  // Posted rather than sent inline. This arrives on Flutter's platform thread, and the
  // strand is where the channel's own receive handling runs, so going through it means
  // a report is never sent against a channel the phone is in the middle of opening,
  // reports leave in the order the fingers moved, and the rate limiter below needs no
  // lock. One hop of a few microseconds.
  std::weak_ptr<InputChannel> weak = weak_from_this();
  strand_.post([weak, report = std::move(report), what, coalesce]() mutable {
    auto self = weak.lock();
    if (!self) {
      return;
    }
    if (!self->Channel()) {
      return;
    }
    if (!self->open_.load()) {
      // Nothing acknowledges an input report, so one sent before the phone has opened
      // the channel would vanish without a trace. Say so instead.
      AASDK_LOG(debug) << "[Input] dropped a " << what
                       << " report: the phone has not opened the input channel.";
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!coalesce) {
      // A finger landing or lifting is not a sample, it is an edge, and it carries the
      // current positions anyway. Any movement being held is stale the moment this
      // exists, so it goes rather than being sent first.
      self->pending_move_.reset();
      self->move_timer_.cancel();
      self->Deliver(report, what);
      return;
    }

    if (now - self->last_sent_ >= kMoveInterval) {
      self->Deliver(report, what);
      return;
    }

    // Inside the interval: hold it, replacing whatever was already held, and make sure
    // something is scheduled to send it once the interval is up.
    self->pending_move_ = std::move(report);
    self->move_timer_.expires_at(self->last_sent_ + kMoveInterval);
    self->move_timer_.async_wait(boost::asio::bind_executor(
        static_cast<aasdk::Strand::Base&>(self->strand_),
        [weak](const boost::system::error_code& error) {
          if (error) {
            // Cancelled, which means either a newer movement re-armed this or the
            // channel is going away. Either way the one that did it owns the decision.
            return;
          }
          if (auto self = weak.lock()) {
            self->FlushPendingMove();
          }
        }));
  });
}

void InputChannel::Deliver(const input_pb::InputReport& report, const char* what) {
  auto channel = Channel();
  if (!channel) {
    return;
  }
  last_sent_ = std::chrono::steady_clock::now();
  channel->sendInputReport(report, MakeSendPromise(what));
}

void InputChannel::FlushPendingMove() {
  if (!pending_move_.has_value()) {
    return;
  }
  input_pb::InputReport report = std::move(*pending_move_);
  pending_move_.reset();
  Deliver(report, "touch");
}

void InputChannel::SendTouch(TouchAction action, int32_t action_index,
                             std::vector<TouchPoint> points) {
  if (points.empty()) {
    return;
  }
  // Every report, not just the first. This is one half of the touch latency
  // measurement: the other half is the frame size line in video_channel.cc, and both
  // only exist at debug level, where aasdk is already logging every message anyway.
  AASDK_LOG(debug) << "[Input] touch " << ActionName(action) << " at "
                   << points.front().x << "," << points.front().y << " ("
                   << points.size() << " pointer" << (points.size() == 1 ? "" : "s")
                   << ")";

  input_pb::InputReport report;
  report.set_timestamp(static_cast<uint64_t>(NowMicros()));
  auto* touch = report.mutable_touch_event();
  touch->set_action(ToProto(action));
  touch->set_action_index(static_cast<uint32_t>(
      std::clamp(action_index, 0, static_cast<int32_t>(points.size()) - 1)));
  for (const TouchPoint& point : points) {
    auto* pointer = touch->add_pointer_data();
    pointer->set_x(static_cast<uint32_t>(std::clamp(point.x, 0, width_ - 1)));
    pointer->set_y(static_cast<uint32_t>(std::clamp(point.y, 0, height_ - 1)));
    pointer->set_pointer_id(static_cast<uint32_t>(std::max(point.id, 0)));
  }
  SendReport(std::move(report), "touch", action == TouchAction::kMoved);
}

void InputChannel::SendKey(int32_t keycode, bool down, bool long_press) {
  AASDK_LOG(debug) << "[Input] key " << keycode << (down ? " down" : " up");
  input_pb::InputReport report;
  report.set_timestamp(static_cast<uint64_t>(NowMicros()));
  auto* key = report.mutable_key_event()->add_keys();
  key->set_keycode(static_cast<uint32_t>(keycode));
  key->set_down(down);
  // No modifier keys: a head unit's buttons are not a keyboard, and every phone tested
  // ignores the field for the keycodes this one advertises.
  key->set_metastate(0);
  key->set_longpress(long_press);
  SendReport(std::move(report), "key", false);
}

void InputChannel::SendRotary(int32_t steps) {
  if (steps == 0) {
    return;
  }
  AASDK_LOG(debug) << "[Input] rotary " << steps;
  input_pb::InputReport report;
  report.set_timestamp(static_cast<uint64_t>(NowMicros()));
  auto* event = report.mutable_relative_event()->add_data();
  event->set_keycode(static_cast<uint32_t>(kRotaryKeycode));
  event->set_delta(steps);
  SendReport(std::move(report), "rotary", false);
}

void InputChannel::onChannelError(const aasdk::error::Error& error) {
  if (error.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
    return;
  }
  // Already stopped: see the note on VideoChannel::onChannelError.
  if (stopped_.load()) {
    return;
  }
  Log(std::string("Input channel error: ") + error.what());
  Stop();
}

}  // namespace aa
