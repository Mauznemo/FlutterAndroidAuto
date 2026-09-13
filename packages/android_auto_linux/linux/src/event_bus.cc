#include "event_bus.h"

#include <cstdlib>
#include <cstring>

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
