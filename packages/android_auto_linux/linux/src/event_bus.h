// Getting news from native threads to the Dart isolate.
//
// aasdk delivers everything on its io_context threads, and none of those may call into
// Dart directly. The Dart side registers a NativeCallable.listener, whose function
// pointer is safe to invoke from any thread: the runtime marshals the call onto the
// isolate. So this is a thin, thread safe holder around that pointer rather than a
// queue of its own.

#ifndef ANDROID_AUTO_LINUX_EVENT_BUS_H_
#define ANDROID_AUTO_LINUX_EVENT_BUS_H_

#include <mutex>
#include <string>

#include "aa_core.h"

namespace aa {

class EventBus {
 public:
  explicit EventBus(AaEventCallback callback);

  // Reports a state change. `message` may be empty. The string is copied onto the heap
  // and ownership passes to Dart, which frees it with aa_string_free.
  void Emit(AaState state, const std::string& message = {});

  // Stops delivery. Called before the Dart side tears its listener down, so a late
  // event from an io_context thread cannot reach a dead callable.
  void Close();

  AaState last_state() const;

 private:
  mutable std::mutex mutex_;
  AaEventCallback callback_;
  AaState last_state_ = AA_STATE_IDLE;
  bool closed_ = false;
};

}  // namespace aa

#endif  // ANDROID_AUTO_LINUX_EVENT_BUS_H_
