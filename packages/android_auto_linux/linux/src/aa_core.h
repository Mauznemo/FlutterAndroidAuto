// Flat C ABI for the Android Auto head unit core.
//
// This is the only surface Dart binds to. Everything is either a plain scalar, an
// opaque pointer, or a NUL terminated UTF-8 string, so ffigen can generate bindings
// with no hand written glue.
//
// Rules that keep it bindable:
//   - no C++ types, no structs returned by value, no callbacks with non trivial types
//   - every call is safe from the Dart main isolate and returns promptly
//   - anything the core wants to tell Dart goes out through the event callback, never
//     through out parameters
//
// See docs/architecture.md for how this sits in the wider design.

#ifndef ANDROID_AUTO_LINUX_AA_CORE_H_
#define ANDROID_AUTO_LINUX_AA_CORE_H_

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

// The plugin library hides symbols by default, so the ABI has to opt back in.
#define AA_EXPORT __attribute__((visibility("default")))

typedef struct AaSession AaSession;

// Lifecycle states, mirrored by AndroidAutoConnectionState in Dart. Keep the two in
// step: the integers cross the FFI boundary as-is.
typedef enum {
  AA_STATE_IDLE = 0,
  AA_STATE_SEARCHING = 1,
  AA_STATE_HANDSHAKING = 2,
  AA_STATE_CONNECTED = 3,
  AA_STATE_ERROR = 4,
} AaState;

// What a touch report describes, mirrored by AndroidAutoTouchAction in Dart and by
// PointerAction on the wire. All three carry Android's own MotionEvent action
// constants, so the gaps in the numbering are deliberate.
typedef enum {
  AA_TOUCH_DOWN = 0,
  AA_TOUCH_UP = 1,
  AA_TOUCH_MOVED = 2,
  AA_TOUCH_POINTER_DOWN = 5,
  AA_TOUCH_POINTER_UP = 6,
} AaTouchAction;

// One finger. `x` and `y` are in projected video pixels, not logical pixels and not
// normalised: the phone is told the head unit has a touchscreen exactly the size of the
// video it asked for, and it reads these against that. `id` identifies the finger
// across a gesture and should be a small index, the way Android numbers pointers.
typedef struct {
  int32_t id;
  int32_t x;
  int32_t y;
} AaTouchPoint;

// How the head unit describes itself to the phone during service discovery.
typedef struct {
  int32_t width;
  int32_t height;
  int32_t fps;
  int32_t dpi;
  const char* head_unit_name;
  const char* car_model;
  const char* car_year;
  // Directory holding headunit.crt and headunit.key. NULL uses the bundled pair.
  const char* certificate_path;
} AaConfig;

// Called when the session changes state or has something to report.
//
// Invoked from whatever thread the event originated on, so the Dart side must use a
// NativeCallable.listener rather than an isolate local callback. `message` is heap
// allocated by the core and ownership passes to the callee, which must hand it back to
// aa_string_free once it has been copied into Dart. It is NULL when there is nothing
// to say.
typedef void (*AaEventCallback)(int32_t state, char* message);

// Frees a string handed out through AaEventCallback. Safe to call with NULL.
AA_EXPORT void aa_string_free(char* message);

// Creates a session. Does not touch any hardware and does not start any threads yet.
// `on_event` may be NULL, though then nothing will ever be reported.
AA_EXPORT AaSession* aa_session_create(const AaConfig* config,
                                       AaEventCallback on_event);

// Stops the session if it is running and releases everything. The session pointer is
// invalid afterwards. Safe to call with NULL.
AA_EXPORT void aa_session_destroy(AaSession* session);

// Starts the io_context thread pool and begins looking for a phone. Returns 0 on
// success, negative on failure. Idempotent.
AA_EXPORT int32_t aa_session_start(AaSession* session);

// Stops the session and joins its threads. Returns 0 on success. Idempotent.
AA_EXPORT int32_t aa_session_stop(AaSession* session);

// The Flutter texture id carrying the projected video, or -1 while there is no video.
// The texture is registered lazily on the first frame.
AA_EXPORT int64_t aa_session_texture_id(AaSession* session);

// Size of the video the phone is actually sending, or 0 before the first frame.
//
// This is not necessarily the size asked for in AaConfig. The phone picks from the
// video configurations service discovery advertised, and it may change mid session
// without the texture being rebuilt, so the host app reads it rather than assuming.
AA_EXPORT int32_t aa_session_video_width(AaSession* session);
AA_EXPORT int32_t aa_session_video_height(AaSession* session);

// Which decoder is running: "VA-API", "software", or "none" before the first frame.
// The returned string is heap allocated and must be handed back to aa_string_free.
AA_EXPORT char* aa_session_video_backend(AaSession* session);

// Reports a touch to the phone. `points` carries every finger currently down, and
// `action_index` is the index within it of the finger this report is about, which only
// means anything for AA_TOUCH_POINTER_DOWN and AA_TOUCH_POINTER_UP.
//
// Returns 0 if the report was queued, -1 on a bad argument, and -2 when there is no
// input channel, which is the normal answer whenever a phone is not connected. Nothing
// acknowledges an input report, so a caller that ignores the result will never find out
// that its taps went nowhere.
//
// Safe to call from the Dart main isolate: the send is posted onto the channel's own
// thread and this returns immediately.
AA_EXPORT int32_t aa_session_send_touch(AaSession* session, int32_t action,
                                        int32_t action_index, const AaTouchPoint* points,
                                        int32_t count);

// Reports one hardware key transition. Down and up are separate calls, as they are on
// Android: a phone that gets a down and no up believes the button is still held.
// `keycode` must be one of the codes service discovery advertised. Same return values
// as aa_session_send_touch.
AA_EXPORT int32_t aa_session_send_key(AaSession* session, int32_t keycode, int32_t down,
                                      int32_t long_press);

// Reports rotary encoder movement, in detents, positive clockwise. Sent as a relative
// axis rather than a key, which is what makes the phone scroll a list by steps instead
// of treating every detent as a button press. Same return values as
// aa_session_send_touch.
AA_EXPORT int32_t aa_session_send_rotary(AaSession* session, int32_t steps);

// Drives the texture pipeline from a generated pattern instead of a phone, so a host
// app can lay its overlay out before any hardware is involved. Started life as M2
// scaffolding and earned its keep; the real H.264 path publishes into the same ring.
AA_EXPORT int32_t aa_session_start_test_pattern(AaSession* session);
AA_EXPORT int32_t aa_session_stop_test_pattern(AaSession* session);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // ANDROID_AUTO_LINUX_AA_CORE_H_
