#include "event_bus.h"

#include <cstdlib>
#include <cstring>

#include <aasdk/Common/Log.hpp>

namespace aa {

EventBus::EventBus(AaEventCallback callback) : callback_(callback) {}

void EventBus::Emit(AaState state, const std::string& message) {
  AaEventCallback callback = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || callback_ == nullptr) {
      return;
    }
    last_state_ = state;
    callback = callback_;
  }

  // Into the protocol log as well as out to Dart. An event is the only record of why a
  // session ended, and reading it next to the USB traffic that caused it is what makes
  // the cause obvious; in the UI it is one line that the next event overwrites.
  AASDK_LOG(info) << "[Event] state " << static_cast<int32_t>(state)
                  << (message.empty() ? std::string() : ": " + message);

  // strdup rather than passing c_str(): the listener runs later, on the Dart isolate,
  // long after this string has gone out of scope.
  char* copy = nullptr;
  if (!message.empty()) {
    copy = strdup(message.c_str());
  }
  // Called outside the lock. The callback hops threads, and holding a lock across that
  // is how deadlocks with the Dart side start.
  callback(static_cast<int32_t>(state), copy);
}

void EventBus::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  callback_ = nullptr;
}

AaState EventBus::last_state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_state_;
}

}  // namespace aa
