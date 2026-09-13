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

// Drives the texture pipeline from a generated pattern instead of a phone, so a host
// app can lay its overlay out before any hardware is involved. Started life as M2
// scaffolding and earned its keep; the real H.264 path publishes into the same ring.
AA_EXPORT int32_t aa_session_start_test_pattern(AaSession* session);
AA_EXPORT int32_t aa_session_stop_test_pattern(AaSession* session);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // ANDROID_AUTO_LINUX_AA_CORE_H_
