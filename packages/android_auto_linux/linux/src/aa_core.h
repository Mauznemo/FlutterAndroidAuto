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

// How a phone may reach this head unit, one bit each. Mirrored by
// AndroidAutoTransport in Dart.
//
// The two are not alternatives and a head unit is normally both: the cable is what a
// driver reaches for when the battery is low, and it is also how a phone first learns
// that a given car can project. Wireless costs a Bluetooth service and an open TCP
// port, and nothing at all until a phone asks.
typedef enum {
  AA_TRANSPORT_USB = 1 << 0,
  AA_TRANSPORT_WIRELESS = 1 << 1,
} AaTransport;

// How the Wi-Fi network the phone is told to join is secured, as the protocol's own
// WifiSecurityMode. The gaps are deliberate: the numbering is the wire's.
typedef enum {
  AA_WIFI_OPEN = 1,
  AA_WIFI_WPA_PERSONAL = 4,
  AA_WIFI_WPA2_PERSONAL = 5,
  AA_WIFI_WPA_WPA2_PERSONAL = 6,
} AaWifiSecurity;

// Whether the head unit brought the network up for the phone or is merely on one.
typedef enum {
  // A network that was already there, which the head unit happens to be joined to.
  AA_ACCESS_POINT_STATIC = 0,
  // A network this head unit is hosting.
  AA_ACCESS_POINT_DYNAMIC = 1,
} AaAccessPointType;

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

// Which of the three PCM sinks a call is about, mirrored by AudioStream in
// audio/audio_output.h and AndroidAutoAudioStream in Dart. Android Auto sends these as
// three separate streams and leaves the mixing to the head unit, which is why they are
// controlled separately rather than through one volume.
typedef enum {
  AA_AUDIO_STREAM_MEDIA = 0,
  AA_AUDIO_STREAM_SYSTEM = 1,
  AA_AUDIO_STREAM_SPEECH = 2,
} AaAudioStream;

// Which sensors the head unit tells the phone it has, one bit each. Mirrored by Sensor
// in sensors/sensor_state.h and by AndroidAutoSensor in Dart, so the order is part of
// the ABI.
//
// Advertising one is a declaration that the car has it, not a feature switch. The phone
// subscribes to what is offered and then waits, and for AA_SENSOR_LOCATION it stops
// using its own position the moment it sees the entry, so a head unit that offers a
// position it cannot supply has taken navigation away from a phone that was managing
// without it. Offer what the host app can actually feed.
typedef enum {
  AA_SENSOR_NIGHT_MODE = 1 << 0,
  AA_SENSOR_DRIVING_STATUS = 1 << 1,
  AA_SENSOR_LOCATION = 1 << 2,
  AA_SENSOR_SPEED = 1 << 3,
  AA_SENSOR_RPM = 1 << 4,
  AA_SENSOR_FUEL = 1 << 5,
  AA_SENSOR_PARKING_BRAKE = 1 << 6,
  AA_SENSOR_GEAR = 1 << 7,
  AA_SENSOR_COMPASS = 1 << 8,
  AA_SENSOR_ENVIRONMENT = 1 << 9,
  AA_SENSOR_ODOMETER = 1 << 10,
  AA_SENSOR_TOLL_CARD = 1 << 11,
} AaSensor;

// What the phone tells the head unit about itself, one channel each. Mirrored by
// Metadata in metadata/metadata_state.h and by AndroidAutoMetadata in Dart, both as an
// index and as the bit `1 << index`, so the order is part of the ABI.
//
// The mirror image of AaSensor: those are what the head unit tells the phone about the
// car, these are what the phone tells the head unit about itself. Advertising one
// promises nothing, unlike a sensor. A phone pushes what it has and a head unit that
// ignores it simply draws no turn card.
typedef enum {
  AA_METADATA_NAVIGATION = 0,
  AA_METADATA_MEDIA = 1,
  AA_METADATA_PHONE = 2,
  AA_METADATA_NOTIFICATION = 3,
  AA_METADATA_BROWSE = 4,
} AaMetadata;

// What the car forbids while it is moving, as the protocol's DrivingStatus bits. They
// combine: a parked car sets none of them.
typedef enum {
  AA_DRIVING_UNRESTRICTED = 0,
  AA_DRIVING_NO_VIDEO = 1,
  AA_DRIVING_NO_KEYBOARD = 2,
  AA_DRIVING_NO_VOICE = 4,
  AA_DRIVING_NO_CONFIG = 8,
  AA_DRIVING_LIMIT_MESSAGE_LENGTH = 16,
} AaDrivingRestriction;

// A position fix, for aa_session_set_location.
//
// Latitude and longitude are always read. The other four are skipped when they are NaN,
// because every one of them has a meaningful zero: a bearing of zero is due north, an
// altitude of zero is sea level and a speed of zero is standing still, so none of them
// can double as "not known". A field left out is absent on the wire, which is not the
// same as a field set to zero.
typedef struct {
  double latitude;
  double longitude;
  // Radius in metres of the circle the fix is somewhere in.
  double accuracy_metres;
  double altitude_metres;
  double speed_mps;
  // Direction of travel in degrees clockwise from north.
  double bearing_degrees;
} AaLocation;

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
  // Which sensors to advertise, an OR of AaSensor bits. Zero is read as the two that
  // are not optional, AA_SENSOR_NIGHT_MODE and AA_SENSOR_DRIVING_STATUS: a head unit
  // that answers neither leaves the phone with most of its interface locked.
  int32_t sensors;
  // Which metadata channels to advertise, an OR of `1 << AaMetadata`. Negative means
  // the host app did not ask, and is read as navigation, media and telephony: the three
  // the phone pushes without being asked, which cost the head unit nothing but a
  // channel. Zero is a host app that wants none of them, which is a real choice.
  int32_t metadata;
  // How a phone may reach this head unit, an OR of AaTransport bits. Zero is read as
  // AA_TRANSPORT_USB, so a host app written before wireless existed keeps working.
  int32_t transports;
} AaConfig;

// The Wi-Fi network a phone is told to join, and where to dial once it is on it.
//
// Every string may be NULL or empty, and an empty one means "work it out from the
// machine". The one field nothing can work out is the passphrase: the kernel does not
// keep one and the network manager's copy is behind a privileged interface, so a head
// unit on an ordinary network needs exactly one thing configured.
//
// Nothing here brings a network up. Hosting an access point, or joining someone
// else's, is the machine's configuration and not a plugin's business, in the same way
// the echo canceller for phone calls is.
typedef struct {
  // Defaults to the SSID the wireless interface is on, or hosting.
  const char* ssid;
  const char* passphrase;
  // The access point's MAC. Defaults to the one the interface reports.
  const char* bssid;
  // Which wireless interface to describe. Defaults to the first one with an address.
  // Spelled out rather than "interface" because that is a reserved word in Dart and
  // the generated binding would have to mangle it.
  const char* interface_name;
  // The address the phone connects back to. Defaults to that interface's IPv4.
  const char* ip_address;
  // Which paired phone to prod when wireless starts being offered and nothing asks
  // for it, as "AA:BB:CC:DD:EE:FF". Normally left empty, which prods whichever paired
  // phone is connected. Worth naming on a machine that several phones are paired with.
  //
  // The prod drops and remakes that phone's Bluetooth link, because re-reading the
  // service list is something a phone only does when the link comes up. It costs that
  // phone's Bluetooth audio a few seconds, so it only happens when a phone has gone
  // quiet, which is what being refused for a while does to one.
  const char* phone_address;
  // Defaults to 5288, which is the port Android Auto dials.
  int32_t port;
  // An AaWifiSecurity. Zero is read as AA_WIFI_WPA2_PERSONAL.
  int32_t security;
  // An AaAccessPointType, or negative to decide it from whether the interface is
  // hosting the network rather than joined to it.
  int32_t access_point;
} AaWirelessConfig;

// Called when the session changes state or has something to report.
//
// Invoked from whatever thread the event originated on, so the Dart side must use a
// NativeCallable.listener rather than an isolate local callback. `message` is heap
// allocated by the core and ownership passes to the callee, which must hand it back to
// aa_string_free once it has been copied into Dart. It is NULL when there is nothing
// to say.
typedef void (*AaEventCallback)(int32_t state, char* message);

// Called with one buffer of PCM, exactly as the phone sent it, before this head unit's
// volume or ducking is applied. For a host app that wants to mix the audio itself;
// pair it with aa_session_set_audio_output_enabled(session, 0).
//
// Invoked from an audio writer thread, so the Dart side must use a
// NativeCallable.listener. `data` is heap allocated by the core and ownership passes to
// the callee, which must hand it back to aa_audio_buffer_free once it has been copied
// into Dart. `stream` is an AaAudioStream.
typedef void (*AaAudioCallback)(int32_t stream, uint8_t* data, int32_t size,
                                int32_t sample_rate, int32_t channels);

// Called when the phone says something about itself: a turn coming up, a track
// starting, a call arriving, a notification, or the answer to a browse request.
//
// `kind` is an AaMetadata. `json` is a UTF-8 JSON object, heap allocated by the core,
// and ownership passes to the callee, which must hand it back to aa_string_free once it
// has been copied into Dart. Invoked from an io_context thread, so the Dart side must
// use a NativeCallable.listener.
//
// JSON rather than a struct per kind because the shapes are deep and they are the one
// thing here the protocol is likely to add to: a turn carries a lane diagram and a list
// of destinations, a call list has a call per entry with a photo in it. A field the
// phone did not send is absent from the object rather than present and zero, for the
// reason the sensors document at length. Pictures are base64 of whatever image format
// the phone chose.
typedef void (*AaMetadataCallback)(int32_t kind, char* json);

// Frees a string handed out through AaEventCallback. Safe to call with NULL.
AA_EXPORT void aa_string_free(char* message);

// Frees a buffer handed out through AaAudioCallback. Safe to call with NULL.
AA_EXPORT void aa_audio_buffer_free(uint8_t* data);

// The outputs the audio server is offering, so a host app can present a picker.
//
// One device per line, three tab separated fields: the name to hand to
// aa_session_set_audio_device, a human readable description, and "1" for the device the
// server would use by default or "0" otherwise. Empty when no audio server can be
// reached. The returned string is heap allocated and must be handed back to
// aa_string_free.
//
// Takes no session because it describes the machine rather than a connection, and
// blocks for up to a second, so call it from Dart rather than from a hot path.
AA_EXPORT char* aa_audio_devices(void);

// The inputs the audio server is offering, in the same format as aa_audio_devices, for
// aa_session_set_microphone_device. Monitors of outputs are left out: they would let a
// head unit send the phone its own audio back, which is not what anyone means by a
// microphone.
AA_EXPORT char* aa_microphone_devices(void);

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

// === audio ===
//
// All of these are safe from the Dart main isolate and return immediately. Volume and
// mute are per stream and survive a phone reconnecting, because they describe the head
// unit rather than the phone. Each returns 0 on success and -1 on a bad argument.

// 0.0 to 1.0, clamped. Applied in software with a short ramp, so a change mid track is
// a fade rather than a click.
AA_EXPORT int32_t aa_session_set_audio_volume(AaSession* session, int32_t stream,
                                              double volume);
AA_EXPORT double aa_session_audio_volume(AaSession* session, int32_t stream);

// A muted stream is written as silence rather than not written at all, so the stream
// clock keeps running and unmuting is instant.
AA_EXPORT int32_t aa_session_set_audio_muted(AaSession* session, int32_t stream,
                                             int32_t muted);
AA_EXPORT int32_t aa_session_audio_muted(AaSession* session, int32_t stream);

// Which output to play through, as a name from aa_audio_devices. NULL or empty means
// the head unit's own speakers: the audio server's default, except that a Bluetooth
// device is never chosen for it, because a phone paired for hands free calling moves
// that default and the car's speakers are not the phone's to move. Naming a Bluetooth
// device here still works. Takes effect on the next buffer of each stream, which for a
// stream that is playing is immediately.
AA_EXPORT int32_t aa_session_set_audio_device(AaSession* session, const char* device);
// The device currently selected, or an empty string for the default. Heap allocated,
// free with aa_string_free.
AA_EXPORT char* aa_session_audio_device(AaSession* session);

// Turns the speakers off without touching the protocol: the phone keeps sending, the
// audio callback keeps firing, and nothing is played. For an infotainment system that
// routes audio itself, or an app doing its own mixing.
AA_EXPORT int32_t aa_session_set_audio_output_enabled(AaSession* session,
                                                      int32_t enabled);
AA_EXPORT int32_t aa_session_audio_output_enabled(AaSession* session);

// Installs the raw PCM tap, or clears it with NULL. See AaAudioCallback.
AA_EXPORT int32_t aa_session_set_audio_callback(AaSession* session,
                                                AaAudioCallback on_audio);

// Which audio backend is playing: "PulseAudio", or "none" before the first buffer or on
// a machine with no audio server. Heap allocated, free with aa_string_free.
AA_EXPORT char* aa_session_audio_backend(AaSession* session);

// Times this stream came close to running the speakers dry, since the session started.
// The number to watch for "no audible glitches": it should stay at zero.
AA_EXPORT int64_t aa_session_audio_underruns(AaSession* session, int32_t stream);
// Buffers thrown away because the phone sent faster than they could be played.
AA_EXPORT int64_t aa_session_audio_dropped(AaSession* session, int32_t stream);
// How far behind the head unit the speakers are, in microseconds.
AA_EXPORT int64_t aa_session_audio_latency(AaSession* session, int32_t stream);

// === microphone ===
//
// The head unit's microphone, which is what carries "Hey Google" and everything said
// after the mic button. All of these are safe from the Dart main isolate and return
// immediately.
//
// The capture device is opened when the phone asks for it and closed when the phone lets
// it go, and at no other time. There is no call here that starts recording, deliberately:
// a head unit that can be made to listen by its own host app is a different and much
// worse thing than one that listens when the Assistant is invoked.

// Whether the microphone is open right now, 1 or 0. This is what a "listening" indicator
// shows. It reports the state of the device rather than of the channel, so a phone that
// has opened the channel but not asked to record reads 0.
AA_EXPORT int32_t aa_session_microphone_active(AaSession* session);

// Peak level of the most recent captured buffer, 0.0 to 1.0, for a level meter. Zero
// while the microphone is closed.
AA_EXPORT double aa_session_microphone_level(AaSession* session);

// Bytes captured since the session was created. Answers "has this machine ever actually
// heard anything", which is the question a silent Assistant raises.
AA_EXPORT int64_t aa_session_microphone_bytes(AaSession* session);

// Which input to capture from, as a name from aa_microphone_devices. NULL or empty
// means the head unit's own microphone: the audio server's default, except that a
// Bluetooth device is never chosen for it, for the reason aa_session_set_audio_device
// gives. Takes effect the next time the phone asks for the microphone, because that is
// the only moment this is allowed to open anything.
AA_EXPORT int32_t aa_session_set_microphone_device(AaSession* session,
                                                   const char* device);
// The input currently selected, or an empty string for the default. Heap allocated, free
// with aa_string_free.
AA_EXPORT char* aa_session_microphone_device(AaSession* session);

// Which capture backend is running: "PulseAudio", or "none" before the first capture or
// on a machine with no microphone. Heap allocated, free with aa_string_free.
AA_EXPORT char* aa_session_microphone_backend(AaSession* session);

// === sensors ===
//
// What the head unit tells the phone about the car. All of these are safe from the Dart
// main isolate and return immediately, and each returns 0 on success and -1 on a bad
// argument.
//
// Nothing here reads any hardware. This plugin owns no GPS and no parking brake: the
// host app is the thing running in the vehicle, so it is the thing that knows. A value
// set here is remembered across reconnects, because a phone being unplugged does not
// change what the car is doing, and it is sent to the phone when it changes, but only
// for the sensors the phone actually subscribed to.
//
// A sensor that has never been set is not reported at all rather than reported as zero,
// which is the difference between a car that has no fix yet and a car in the Atlantic.
// Night mode and the driving status are the exceptions: they start at day and
// unrestricted, because the phone needs an answer to those before it will finish
// opening its interface.

// False for day, true for night. Drives the phone's own light and dark theme.
AA_EXPORT int32_t aa_session_set_night_mode(AaSession* session, int32_t night);

// What the car forbids right now, an OR of AaDrivingRestriction bits.
// AA_DRIVING_UNRESTRICTED is a parked car, and is what this head unit reports until the
// host app says otherwise.
AA_EXPORT int32_t aa_session_set_driving_status(AaSession* session,
                                                int32_t restrictions);

// The car's position. See AaLocation for which fields may be NaN.
AA_EXPORT int32_t aa_session_set_location(AaSession* session, const AaLocation* location);

// Road speed in metres per second.
AA_EXPORT int32_t aa_session_set_speed(AaSession* session, double metres_per_second);

// Engine speed in revolutions per minute.
AA_EXPORT int32_t aa_session_set_rpm(AaSession* session, double rpm);

// Tank level as a percentage, remaining range in metres, and whether the low fuel
// warning is lit.
AA_EXPORT int32_t aa_session_set_fuel(AaSession* session, double level_percent,
                                      double range_metres, int32_t low);

// Whether the parking brake is engaged.
AA_EXPORT int32_t aa_session_set_parking_brake(AaSession* session, int32_t engaged);

// The selected gear, as the protocol numbers them: 0 neutral, 1 to 10 the numbered
// gears, 100 drive, 101 park, 102 reverse.
AA_EXPORT int32_t aa_session_set_gear(AaSession* session, int32_t gear);

// Heading in degrees clockwise from north. Distinct from the bearing inside a location
// fix: this is the direction the car points, which is not the direction it is moving.
AA_EXPORT int32_t aa_session_set_compass(AaSession* session, double bearing_degrees);

// Outside temperature in degrees Celsius and barometric pressure in kilopascals. Either
// may be NaN, which leaves that one out.
AA_EXPORT int32_t aa_session_set_environment(AaSession* session,
                                             double temperature_celsius,
                                             double pressure_kpa);

// Total distance travelled, in kilometres.
AA_EXPORT int32_t aa_session_set_odometer(AaSession* session, double kilometres);

// Whether a toll transponder is in the car.
AA_EXPORT int32_t aa_session_set_toll_card(AaSession* session, int32_t present);

// Which sensors the phone has subscribed to, an OR of AaSensor bits, or 0 when no phone
// is connected. Never the same question as which ones were advertised: a phone takes
// what it wants from the list, and this is the first thing to look at when a value is
// being set and nothing on the phone's screen changes.
AA_EXPORT int32_t aa_session_sensor_subscriptions(AaSession* session);

// Sensor batches written since the session was created. The "did anything actually go
// out" number, which is otherwise only answerable by watching the phone's UI.
AA_EXPORT int64_t aa_session_sensor_batches(AaSession* session);

// === metadata ===
//
// What the phone is doing, as opposed to what it looks like. The point of the whole
// plugin: a host app can draw its own turn card, its own now playing bar and its own
// call banner from these rather than only mirroring pixels.
//
// Nothing here is remembered across connections, unlike the sensors and the audio
// settings. What the phone is playing stops being true the moment it is unplugged, so
// the snapshots go empty when a session ends rather than showing a track that finished
// an hour ago.

// Installs the metadata callback, or clears it with NULL. See AaMetadataCallback.
AA_EXPORT int32_t aa_session_set_metadata_callback(AaSession* session,
                                                   AaMetadataCallback on_metadata);

// The latest of one kind, as the same JSON the callback delivers, or an empty string
// when the phone has said nothing. For a host app that starts reading after the phone
// has already spoken. Heap allocated, free with aa_string_free.
AA_EXPORT char* aa_session_metadata(AaSession* session, int32_t kind);

// Which metadata channels the phone actually opened, an OR of `1 << AaMetadata`, or 0
// when no phone is connected. Never the same question as which were advertised, exactly
// as with the sensor subscriptions: this is the first thing to look at when nothing is
// arriving.
AA_EXPORT int32_t aa_session_metadata_channels(AaSession* session);

// Updates received on one kind since the head unit started. The "is anything coming in"
// number.
AA_EXPORT int64_t aa_session_metadata_updates(AaSession* session, int32_t kind);

// Asks the phone for one node of its media library. `path` is NULL or empty for the
// root and otherwise a path out of a previous answer; `start` is the offset into a long
// list. The answer arrives as an AA_METADATA_BROWSE update rather than as a return
// value, because it is a round trip over USB.
//
// Returns 0 if the request was queued, -2 when the browser channel is not open, which
// is the normal answer whenever no phone is connected or the host app did not advertise
// the channel.
AA_EXPORT int32_t aa_session_browse(AaSession* session, const char* path, int32_t start);

// Tells the phone the user picked `path` in the media browser, which is what makes it
// play. Same return values as aa_session_browse.
AA_EXPORT int32_t aa_session_browse_select(AaSession* session, const char* path);

// === wireless ===
//
// Android Auto without a cable. The phone opens a Bluetooth channel to a service this
// head unit publishes, is told which Wi-Fi network to be on and which address to dial,
// and then connects to it. Everything above that connection, SSL and every channel, is
// what already runs over USB.
//
// Nothing here changes how the machine presents itself on Bluetooth. One UUID is added
// to the adapter's service record and nothing is taken away, so a head unit whose own
// software pairs the phone for music and hands free calling keeps working exactly as
// it did. That software is worth keeping: a phone decides a machine is a car partly
// from the hands free profile it offers.

// Every phone this machine is paired with, so a host app can present a picker.
//
// One device per line, four tab separated fields: the Bluetooth address, the name, "1"
// if it is connected right now, and "1" if its device class says it is a phone. Empty
// when BlueZ cannot be reached, which on a machine with no Bluetooth is not a fault.
// Heap allocated, free with aa_string_free.
//
// Takes no session because it describes the machine rather than a connection, and
// blocks briefly, so call it from Dart rather than from a hot path.
AA_EXPORT char* aa_paired_phones(void);

// Sets what the phone will be told about the Wi-Fi network. Takes effect the next time
// wireless starts, so call it before aa_session_start. Returns 0, or -1 on a bad
// argument.
AA_EXPORT int32_t aa_session_set_wireless_config(AaSession* session,
                                                 const AaWirelessConfig* config);

// Publishes the Bluetooth service and opens the projection port. Returns 0 on success,
// -1 if the session is not running, and -2 if the machine cannot offer a network; in
// the last case the reason has already gone out through the event callback. Idempotent.
//
// Called for you by aa_session_start when AaConfig::transports has
// AA_TRANSPORT_WIRELESS in it. This exists so a host app can turn wireless on and off
// while it runs.
AA_EXPORT int32_t aa_session_start_wireless(AaSession* session);

// Stops offering wireless. Does not touch a session that is already projecting over
// Wi-Fi: a driver who switches wireless off mid journey meant "do not start another
// one", not "cut this one off". Returns 0. Idempotent.
//
// The projection port closes, but the Bluetooth service stays published and every
// phone that asks is told no. That is deliberate and it is the opposite of what it
// looks like. A phone that has been introduced to this machine as a wireless car asks
// for the service every five seconds for as long as it is connected over Bluetooth,
// and withdrawing the service does not stop it asking, it only stops it being
// answered: the driver is left with a permanent notification saying the phone is
// connecting while nothing is. Measured on a Pixel 8 Pro: a query every 5.1 seconds
// indefinitely against silence, one query and then nothing when refused.
//
// The service is withdrawn for good by aa_session_destroy, and never published at all
// when AaConfig::transports leaves AA_TRANSPORT_WIRELESS out. A phone paired while it
// is out never learns this machine can project, so it never asks.
AA_EXPORT int32_t aa_session_stop_wireless(AaSession* session);

// Publishes the Bluetooth service and refuses every phone that asks, without offering
// anything. Returns 0. Idempotent, and a no-op when AaConfig::transports leaves
// AA_TRANSPORT_WIRELESS out.
//
// This is the state a head unit is in whenever it is switched on and not projecting,
// and it is worth being in deliberately. A phone that knows the machine as a wireless
// car asks for the service every five seconds for as long as Bluetooth is connected,
// and shows the driver a notification saying it is connecting for as long as nothing
// answers. Answering "not now" makes it stop. See aa_session_stop_wireless.
//
// Call it as soon as the head unit application is up, before anything is started.
AA_EXPORT int32_t aa_session_decline_wireless(AaSession* session);

// Whether wireless is currently being offered, 1 or 0.
AA_EXPORT int32_t aa_session_wireless_active(AaSession* session);

// What the head unit resolved to and is telling phones, as tab separated fields on one
// line: interface, SSID, BSSID, IP, port, "1" or "0" for hosting the network, the same
// for whether Bluetooth is published, and the same for whether a phone has opened the
// Bluetooth channel. Empty while wireless is not running.
//
// The answer to "why is nothing happening": it shows whether the machine found a
// network at all, and whether the phone has got as far as Bluetooth. Heap allocated,
// free with aa_string_free.
AA_EXPORT char* aa_session_wireless_summary(AaSession* session);

// Drives the texture pipeline from a generated pattern instead of a phone, so a host
// app can lay its overlay out before any hardware is involved. Started life as M2
// scaffolding and earned its keep; the real H.264 path publishes into the same ring.
AA_EXPORT int32_t aa_session_start_test_pattern(AaSession* session);
AA_EXPORT int32_t aa_session_stop_test_pattern(AaSession* session);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // ANDROID_AUTO_LINUX_AA_CORE_H_
