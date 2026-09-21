# Flutter Android Auto: implementation plan

A Flutter plugin that runs a real Android Auto head unit **inside** the Flutter app,
rendering the projected phone screen into a Flutter `Texture` so the host app can
draw its own widgets on top of it.

Linux first (x86_64 and ARM64). The package layout is federated so an Android
implementation can be added later without touching the app-facing API.

> Working agreement: every box below is a checkpoint. Tick it only when the thing is
> actually verified on this machine, not when the code merely compiles. Sessions are
> expected to pick up from the first unticked box.

---

## 0. Status at a glance

| Milestone | Title | State |
|---|---|---|
| M0 | Project setup, research, tooling | done |
| M1 | Native build: vendor and modernise aasdk | **done** |
| M2 | C ABI core + Dart FFI + texture plumbing | **done** |
| M3 | USB transport, AOAP, SSL, service discovery | **done** |
| M4 | Video channel to Flutter texture | **done** |
| M5 | Input channel (touch, keys, rotary) | **done** |
| M6 | Audio output (media, system, speech) | **done** |
| M7 | Microphone input | **done** |
| M7b | Phone calls over Bluetooth HFP | **done**, bar a two way call |
| M8 | Sensors (night mode, GPS, driving status) | **done** |
| M9 | Metadata channels for native Flutter UI | **done**, bar a call and two channels this phone never opens |
| M10 | Wireless Android Auto | **done**, bar media audio, touch and sensors over Wi-Fi |
| M11 | Packaging, ARM64, CI, docs | **in progress**, bar running it on an ARM64 device |
| M12 | Android implementation package | not started |

---

## M0. Project setup, research, tooling

- [x] Verify the dev machine can be driven headlessly by the agent (screenshot + input)
- [x] Research the Android Auto protocol and pick a base library (see `docs/research.md`)
- [x] Confirm Flutter Linux external texture support is viable (see `docs/architecture.md`)
- [x] `git init` and create the repository layout
- [x] Scaffold the federated packages and the example app
- [x] Write `PLAN.md`, `docs/research.md`, `docs/architecture.md`, `dev/dev-environment.md`
- [x] Write `dev/ui.sh` (screenshot and synthetic input helper for the agent)
- [x] Write `tools/setup-dev-machine.sh` (one shot host provisioning)
- [x] Extend `CLAUDE.md` with the facts every later session needs
- [x] **Decide the project licence**: GPL-3.0-or-later, confirmed 2026-09-12
- [ ] Confirm an Android test phone is available and USB access works (blocks M3, not M2)

---

## M1. Native build: vendor and modernise aasdk

Goal: `libaasdk.a` builds from source inside this repo on Ubuntu 26.04 and links
against the system Boost 1.90, protobuf 3.21, OpenSSL 3.5, libusb 1.0.29.

- [x] Install host build dependencies (`tools/setup-dev-machine.sh --build-deps`)
- [x] Add `opencardev/aasdk` as a git submodule under `packages/android_auto_linux/linux/third_party/aasdk`, pinned to `9bf6adf`
- [x] Try a stock build, capture the full error list into `docs/aasdk-port-notes.md`
- [x] Port `boost::asio::io_service` to `io_context` (95 files affected)
  - [x] `io_service` to `io_context`
  - [x] `io_service::strand` to `aasdk::Strand`, a thin subclass of `boost::asio::strand<io_context::executor_type>` that keeps the old call style, see `docs/aasdk-port-notes.md`
  - [x] `ip::address::from_string` to `ip::make_address`
  - [x] `IOContextWrapper`'s `io_context->post/dispatch` to the free functions
  - [x] `strand.context()` narrowed from `execution_context&` to `io_context&`
  - [x] Keep the diff as a patch under `packages/android_auto_linux/linux/patches/` so the submodule stays clean
- [x] Raise the three `cmake_minimum_required()` calls CMake 4 rejects
- [x] Drop the `system` component from `find_package(Boost)`, gone in Boost 1.90
- [x] Put aasdk's include directory on the `aasdk` target so `add_subdirectory()` consumers can use it
- [x] Build with `-DSKIP_BUILD_PROTOBUF=ON -DSKIP_BUILD_ABSL=ON -DAASDK_TEST=OFF` against system protobuf 3.21
- [x] Verify the generated protobuf covers every channel we need (254 `.pb.h` files: control, input, media sink/source, sensor, browser, playback, nav, phone, bluetooth, wifi projection, vendor, notification, radio)
- [x] Decide static versus shared: shared, 1.6 MB + 2.1 MB stripped, not worth patching upstream to change
- [x] Add a standalone smoke target that links aasdk and exercises the ported strand, to keep M1 regression-testable
- [x] Verify the whole thing round-trips from a clean submodule: reset, apply patch, build, smoke test passes

Reproduce with `tools/build-aasdk.sh`, which fetches the submodule, applies the patch,
builds and runs `packages/android_auto_linux/linux/smoke/aasdk_smoke.cpp`. Expected:

```
boost      : 1.90.0
channel    : MEDIA_SINK_VIDEO
strand     : dispatch, post and get_io_service all behave
protobuf   : ServiceDiscoveryRequest round trips
OK
```

The Boost 1.90 risk turned out to be real but contained: five distinct build failures,
all mechanical, no need for the pinned-Boost or hand-rolled-reactor fallbacks. Full
write up in `docs/aasdk-port-notes.md`.

---

## M2. C ABI core + Dart FFI + texture plumbing

Goal: the example app starts, calls into native code, gets a texture id back, and
shows a test pattern in a `Texture` widget with Flutter widgets on top.

- [x] Create `packages/android_auto_linux/linux/src/` with the core skeleton
  - [x] `aa_core.h`: the flat C ABI (opaque handle, create/destroy/start/stop, config struct, event callback)
  - [x] `aa_core.cc`: lifecycle, owns the Boost `io_context` thread pool
  - [x] `event_bus.{h,cc}`: native to Dart events, closed before the Dart callable is torn down
- [x] Wire `AndroidAutoLinuxPlugin` (GTK entry point) to capture the `FlTextureRegistrar` into a process-global
- [x] `frame_ring.{h,cc}`: the API agnostic seam, a three slot rotation between producer and raster thread
- [x] Implement `present/gl_adapter.{h,cc}`: an `FlTextureGL` subclass fed from the frame ring
  - [x] Define the API agnostic frame descriptor (`fd`, modifier, stride, offset, fourcc, size) the seam is built on
  - [x] Keep `FlTextureGL` confined to this one file, see the Vulkan readiness rules in `docs/architecture.md`
  - [x] Register the texture from the platform thread, not from the producer thread
  - [x] Render a moving test pattern to prove the texture pipeline end to end
- [x] Link aasdk into the plugin so the M3 transport work has nothing left to integrate
- [x] `ffigen` config + generated bindings in `packages/android_auto_linux/lib/src/bindings/`
- [x] Dart side: `AndroidAutoLinux` registers itself as the platform implementation
- [x] `NativeCallable.listener` based event stream from native to Dart
- [x] Define the public API in `android_auto_platform_interface`
  - [x] `AndroidAutoConfig` (resolution, fps, dpi, head unit identity)
  - [x] `AndroidAutoConnectionState` enum and event stream
  - [x] `textureId` future
  - [x] `startTestPattern` / `stopTestPattern`
- [x] Define `AndroidAutoView` widget in `android_auto` (Texture, with the `Listener` for touch landing in M5)
- [x] Example app renders the test pattern with a Flutter overlay on top
- [x] Verify by screenshot: pattern animates, overlay takes input, two stop/start cycles survive

Verified on this machine: the sweeping bar moved across three screenshots taken a
second apart (x = 1244, 1005, 283), so frames really are flowing rather than one frame
being stuck. Three clicks on a button drawn over the texture registered as three taps.
Two full stop/start cycles left the app alive, with a fresh texture id each time.

Not carried over from the original M2 list:
- A `GdkGLContext` shared with Flutter's turned out to be unnecessary. Only the raster
  thread touches GL, inside `populate()`, where Flutter's own context is already current.
  A shared context is needed once the producer creates GL objects itself, which is the
  dmabuf path in M4, not before.

**Checkpoint met:** the native test pattern renders in a `Texture` with Flutter widgets
composited over it, and those widgets still receive input.

---

## M3. USB transport, AOAP, SSL, service discovery

Goal: plug in a phone, get to the point where the phone has accepted our head unit
and announced its services.

- [x] Install the udev rule for AOAP devices (`tools/setup-dev-machine.sh --udev`), no root needed at runtime
- [x] USB enumeration and hotplug via aasdk's `USBHub` and `ConnectedAccessoriesEnumerator`
- [x] AOAP accessory mode switch, verified: the phone re-enumerated `18d1:4ee7` to `18d1:2d01`
- [x] Open the AOAP bulk endpoints, wrap in aasdk `USBTransport`
- [x] SSL handshake with the head unit certificate. There is no path override: aasdk
      compiles the certificate and key in as string literals in `Cryptor.cpp`, so
      supplying another one means rebuilding aasdk. `AaConfig.certificate_path` used to
      be accepted and silently ignored, and was removed on 2026-09-17.
- [x] Version request/response, then `ServiceDiscoveryRequest`
- [x] Build the `ServiceDiscoveryResponse` describing our head unit
- [x] Log the negotiated channel list to Dart as a structured event
- [x] Surface connect/disconnect/error states through the Dart event stream
- [x] Graceful shutdown: send `ByeByeRequest` before dropping the link
- [x] Answer the replies that keep a session alive: ping, audio focus, navigation focus
- [x] Stay connected: held a session for two minutes with no drop
- [x] Stop and resume, 5 cycles in a row, every one reaching connected again
- [x] Recover automatically from a phone left wedged by a run that died without saying goodbye
- [x] Physical unplug and replug: reconnects on its own, about six seconds after the cable goes back in

**Checkpoint met, with one caveat.** The head unit reaches `connected` and reports
`Channels advertised: MEDIA_SINK_VIDEO, INPUT_SOURCE, SENSOR`, verified against a Pixel
with its screen off. Whether Android Auto shows as active on the phone itself has not
been checked, because that needs the screen on.

### How stop and resume actually works, and why it took so long

Ending an Android Auto session is not enough to let the next one start. The sequence
that works is:

1. Send `ByeByeRequest` and **wait for the acknowledgement**. A fixed sleep was not
   enough. Until the phone answers, it keeps Android Auto running and keeps its claim on
   the USB interface.
2. Then reset the USB device. After a ByeBye the phone closes the session but stays in
   accessory mode, and it will not answer a fresh version request on those endpoints.
   What re-arms it is going through the AOAP handshake again, and that only happens once
   it has left accessory mode. `libusb_reset_device` asks for that without anyone
   physically unplugging the cable.
3. On the next start, the phone is back in normal mode, the enumerator switches it into
   accessory mode, and the session begins from scratch.

Skipping step 1 leaves the phone wedged. Skipping step 2 makes every other reconnect
time out, which is what the alternating success and failure pattern turned out to be.

A run that dies without reaching step 1 (a crash, or a force kill) leaves the phone
wedged for the next launch. That is handled: a session that errors before it ever
reached connected bounces the phone and starts over, up to three times.

### Losing the cable mid session

Different case, different handling. The phone vanishing while connected is not an error
the user has to act on, it is a wait, so it reports `searching` rather than `error` and
the session recovers on its own when the cable goes back in.

Two things were needed. `USBHub::handleDevice` does nothing at all while its promise is
null, and handing over the first device clears it, so every later arrival was silently
ignored: discovery is now re-armed immediately after each handover. And the automatic
recovery above only covered sessions that had never connected, which excluded exactly
this case.

Measured end to end, with the cable pulled and put back:

| Time | USB | State |
|---|---|---|
| t=59s | `2d01` | connected |
| t=68s | `2d01` | searching, "The phone disconnected. Waiting for it to come back." |
| t=71s | none | cable out |
| t=80s | `4ee7` | cable back, phone in normal mode |
| t=86s | `2d01` | connected again, no user action |

The drop is noticed at t=68s, before the device even disappears from `lsusb`, because
the transport fails first.

### Reading aasdk's USB errors

`USB_TRANSFER`'s "Native Code" is a `libusb_transfer_status`, not a `libusb_error`. So
**2 is TIMED_OUT and 4 is STALL**. Misreading 2 as STALL cost an hour chasing halted
endpoints that were never halted.

### Lifetime bugs found along the way

aasdk was written for openauto, which builds its object graph once and exits the process
when the phone goes away, so nothing in it is designed to be torn down and rebuilt in
place. Seven crashes came out of this, each a different instance of the same shape.

| What outlived what | Symptom |
|---|---|
| `AOAPDevice` held a freed libusb config descriptor | segfault in `~AOAPDevice` on the second connect |
| `AOAPDevice` destructor ran after `libusb_exit` | segfault in `libusb_close` |
| `USBHub`'s queued cancel ran after `libusb_exit` | segfault in `libusb_hotplug_deregister_callback` |
| `USBHub`'s hotplug callback fired after the hub was destroyed | `std::bad_weak_ptr`, process terminated |
| aasdk's `Channel` posted to a strand owned by a destroyed session | segfault in Boost.Asio |
| `MessageInStream` dereferenced a null `promise_` during teardown | segfault in an io_context thread |
| Our own send rejection handler captured a raw `this` | segfault after the session was gone |

Three of these are upstream bugs now fixed in `linux/patches/`: the use after free, the
ten unguarded `promise_` dereferences, and endpoint halt clearing. The rest are handled
by giving the long lived pieces process lifetime: libusb, the USB connector and the
channel strand are created once and never destroyed. See `src/session/usb_context.h`.

There was also a reference cycle worth remembering: passing `shared_from_this()` as a
channel's event handler makes session to channel to promise to session, so the session
never dies, the USB interface is never released, and every reconnect fails with
`LIBUSB_ERROR_BUSY`. `ControlEventRelay` holds a weak reference instead.

---

## M4. Video channel to Flutter texture

Goal: the phone's projected screen appears inside the Flutter app.

- [x] `VideoService` channel: accept setup, config, start/stop indications
- [x] H.264 Annex-B depacketisation from the AA media stream
- [x] Decoder abstraction with two backends
  - [x] VA-API via `libavcodec` with `AV_HWDEVICE_TYPE_VAAPI`, frames exported as dmabuf
  - [x] Software `libavcodec` fallback, `AV_PIX_FMT_YUV420P` to RGBA with libswscale
- [x] Zero copy path: dmabuf to `EGLImage` to GL texture (`EGL_EXT_image_dma_buf_import`)
- [x] NV12 to RGBA on the GPU with a small shader, because Flutter only takes `GL_RGBA8`
- [x] Audit that nothing above the present seam names a GL type, so `vk_adapter` stays a
      contained addition when Linux moves to Impeller Vulkan
- [x] Frame pacing: mark texture frame available from the producer, drop late frames
- [x] Send `VideoFocus` requests so the phone knows the head unit is showing the projection
- [x] Support 720p30 and 1080p60, both verified against the phone. 800x480 and the
      portrait sizes are mapped in `service_discovery.cc` but have not been tried.
- [ ] Handle resolution changes mid-session without tearing down the texture. The code
      does it by construction (the ring carries the size per frame, the output texture is
      reallocated in place and keeps its id), but the only size change tested so far went
      through a restart, so this is unproven rather than done.
- [x] Measure end to end latency and log it

**Checkpoint met.** Google Maps projected from a Pixel 8 Pro, 1280x720, inside the
example app, with the Flutter status bar drawn over it and overlay clicks still counting.
Stop and start resumes video. Measured wire to frame: **0.9 to 1.1 ms on VA-API**, 2.9 ms
on the software fallback.

### What M3 actually left behind

M3's checkpoint was "reaches connected", and the head unit reported connected because it
had sent its service discovery response. The phone had never accepted it. It opened no
channel, sent nothing, and dropped out of accessory mode a second later. So the first
half of M4 was not video work at all: it was finishing M3.

Two things were wrong with the service discovery response, and neither reports an error.
A phone that dislikes it simply stops talking.

**Fields that are optional in the schema and required in practice.** The response left
`vehicle_id` and `driver_position` unset. Both were `required` in the schema openauto was
built against, and the phone still validates against that shape. `session_configuration`
was being set to an explicit zero, which is not the same as absent on the wire; it is now
left out. `headunit_info` is sent as well as the deprecated make/model/year fields, so
either generation of phone finds what it is looking for.

**Advertising only the channels that are implemented does not work.** Android Auto does
not treat service discovery as a menu. A head unit offering video, input and sensors is
one it refuses to project to. Offering the three audio sinks and the microphone as well
is what makes it open every channel and start encoding. This was measured by bisection,
not guessed: `AA_SERVICES=video` and `AA_SERVICES=video,input,sensor` both get silence,
`AA_SERVICES=all` gets a projection.

That inverts the rule M3 recorded. It is still true that a channel which is advertised
and then never serviced gets the connection dropped, so `src/session/support_channels.cc`
answers all of them: the audio sinks accept and acknowledge the stream and discard the
PCM, the microphone accepts the channel and captures nothing, input answers key binding
requests, and the sensor channel answers the subscription and reports "parked" and "day".
That last one is not politeness. The phone locks most of its interface until the head unit
has told it the driving status.

Each becomes a real service in its own milestone. None of it is meant to survive that.

### Android Auto's H.264 is a profile no GPU decodes

The phone advertises and encodes Baseline profile: `profile_idc` 66 with no constraint
flags. No VA-API driver implements it. Baseline allows arbitrary slice ordering, flexible
macroblock ordering and redundant slices, which no encoder has emitted this century, so
drivers expose only the Constrained Baseline subset that leaves them out. libavcodec used
to paper over the difference and no longer does, so the stream fails hardware setup:

```
[h264] Codec h264 profile 66 not supported for hardware decode.
[h264] Failed setup for format vaapi: hwaccel initialisation returned error.
```

The decoder now sets `constraint_set1_flag` in the SPS, which says "this really is the
constrained subset". For a stream from Android's MediaCodec encoder that is true.
`AA_VIDEO_DECODER=software` is the way out if a phone ever proves it is not.

That failure was invisible before, and worth remembering for the next hardware path: a
`get_format` callback that returns `AV_PIX_FMT_NONE` when the hardware format is gone
leaves the decoder with no output format at all, and **every** packet afterwards comes
back as `AVERROR_INVALIDDATA`. A driver limitation reads exactly like corrupt video.
`ChooseFormat` falls back to whatever libavcodec offers instead, and the reported backend
comes from the frame that came out rather than the one that was asked for.

### The colours were wrong for one line's worth of reason

Every `glBindTexture` lands on whichever texture unit is active. The converter imported
the luma plane on unit 0 and the chroma plane on unit 1, then resized the output texture
while unit 1 was still current, which replaced the chroma plane with the previous frame.
The picture decoded and displayed perfectly, in entirely the wrong colours. The output
texture now gets a unit of its own and is bound before the planes, not after.

### Debugging affordances added along the way

Chasing a phone that answers silence needs visibility that did not exist. All of it is
off unless asked for.

| Knob | What it does |
|---|---|
| `AA_LOG_LEVEL=DEBUG` | aasdk's own protocol log, plus the service discovery exchange in full and libavcodec's diagnostics |
| `AA_SERVICES=video,input,sensor` | narrows or widens the advertised channel set without a rebuild, which is the only way to bisect a response the phone will not comment on |
| `AA_VIDEO_DECODER=software` | forces the fallback, which is how a driver problem gets told apart from a decoder problem |
| `dev/run-example.sh --bundle` | runs the built binary directly, line buffered. `flutter run` block buffers the app's stdout, so protocol logs arrive in 8 KB lumps minutes late |

`dev/run-example.sh` now also kills any instance already running. Two head units
fighting over one phone produce symptoms indistinguishable from a protocol bug: handshakes
that half complete, reads that time out, a phone that goes quiet. An hour went into that
one, and the second instance was only noticed because a human looked at the taskbar.

## M5. Input channel (touch, keys, rotary)

Goal: touching the projection does to the phone what touching the phone would.

- [x] `InputService` channel setup, advertise touchscreen plus the buttons we support
- [x] Flutter `Listener` to projected pixels to `TouchEvent` (down/move/up)
- [x] Correct coordinate mapping when the Texture is letterboxed or scaled
- [ ] Multi touch (the protocol carries pointer ids). Written and reviewed against
      Android's `MotionEvent` rules, but **not verified**: this machine has one pointer
      and `ydotool` cannot produce a second, so nothing here has had two fingers on it.
- [x] Hardware key events (back, home, play/pause, next, prev, call, mic)
- [x] Rotary encoder / D-pad support for non touch head units
- [x] Rate limit movement, see "382 reports a second" below
- [ ] Latency check: touch to visible reaction under 100 ms. Measured, but the target
      turns out to be about the phone rather than about us, see below.

**Checkpoint met.** Google Maps driven from inside the example app: the map pans with a
drag, the zoom buttons work, and the phone bound all sixteen advertised keycodes
(`19, 20, 21, 22, 23, 4, 3, 5, 6, 126, 127, 85, 87, 88, 84, 65536`) the moment the
channel opened, which is the phone confirming it accepted the advertisement.

Adding `keycodes_supported` to service discovery was the risk here, given M4's history
of phones silently refusing a response they dislike. It was accepted first time.

### Where input latency actually goes

The plugin's own contribution is not the interesting part of it:

| Leg | Time |
|---|---|
| Flutter pointer event to the report handed to USB | 0.30 ms mean, 1.00 ms worst |
| Wire to decoded frame (M4's figure, unchanged) | 1.0 ms |

Everything else is the phone deciding what to draw and encoding it. End to end, finger
down to the phone's reaction appearing on the wire, measured against each trial's own
idle frame size: **94, 95, 102, 105, 115, 119, 127, 138 ms** for a map drag. That is
above the 100 ms the box asks for, but almost none of it is ours, and part of it is not
even the phone: a synthetic drag steps every ~30 ms, so Android's touch slop threshold
takes two or three steps to cross before the map will move at all.

Tapping a button instead, to remove the slop, did not give a cleaner answer: Google Maps
responds to a zoom tap in 139 ms once and not at all when already at maximum zoom, which
is indistinguishable from a lost report without knowing the app's state. The honest
statement is the table above plus "the rest is the phone", not a single number.

### 382 reports a second

The first version sent one report, and so one USB bulk write, per Flutter pointer event.
On a desktop mouse that measured **382 a second**. Nothing needs that: what comes back is
a video stream of at most 60 frames a second, so more than one touch sample per frame
cannot produce a distinguishable picture.

Movement is now coalesced to one report per 16 ms, newest position replacing the held
one, so the phone always gets where the finger is rather than a stale sample. Finger
down, finger up, keys and rotary are never delayed: they are edges, not samples.
Measured after the change, 170 pointer events in a second became 49 reports.

### The disconnect that looked like a cable

Reported as: a few seconds of dragging on the map, then "the phone disconnected", and
only Stop then Start would bring it back while the phone still showed Android Auto
running. It was not the cable, and it was not really about dragging either.

`LIBUSB_TRANSFER_ERROR` on the bulk IN endpoint kills the transport, **but leaves the
phone enumerated and still in accessory mode**. The recovery M3 built waits for the USB
hub to hand it a device, and the hub only fires on arrival. Nothing arrived, because
nothing had left. So the session sat in `searching` forever, and the only way out was a
stop, whose `ResetDevice()` is what bounced the phone into re-enumerating.

A connected session that loses its transport now bounces the phone itself rather than
waiting to be told about it. That is right for both cases: if the cable really is out,
the reset fails harmlessly on a device that has already gone, and re-arming discovery is
what the replug needed anyway. Verified with fault injection, four consecutive cycles,
**4.6 seconds from dead transport to projecting again**, no user action.

Two things made this findable, both worth keeping:

- aasdk logged `Transfer Cancelled.` for *every* failed transfer. Cancelled is one of six
  statuses, and telling a stall from a timeout from a device that has gone is the whole
  diagnosis. It now prints what libusb actually said, with the endpoint and byte counts.
  This is the same trap `USB_TRANSFER`'s native code already set once, recorded under M3.
- `"The phone disconnected"` was **replacing** the underlying error rather than carrying
  it, so every transport failure read identically in the UI.

One dead transport is also one event, however many channels notice it. All seven do,
within a millisecond, and they report their failures as messages on the *connected*
state, which put `reached_connected` back up in between and started a second bounce. The
recovery is now debounced.

### The dropouts, and where that was left

The `LIBUSB_TRANSFER_ERROR` above is `-EPROTO`, a USB transaction error, caught with
usbmon. It is a physical layer fault: it hits the **ADB interface too**, which this code
never opens, twice within 14 ms of each other on both interfaces. Wiggling the cable for
two minutes did not provoke it, so it is not a loose connector in the obvious mechanical
sense, but it is below the software either way.

Every occurrence moved **zero bytes**, and that is what makes it survivable. USB's data
toggle means such a transfer lost nothing, so `USBEndpoint` now resubmits instead of
failing upwards. The first time that ran against a real fault it still dropped the
session, for a reason worth remembering: the retry fired, and the **resubmit was refused**
because the controller had halted the endpoint. One retry line, then a dead session, which
reads exactly like the retry never happening. Clearing the halt and resubmitting is the
answer.

- [x] Retry a zero length transfer failure rather than tearing the session down
- [x] Clear a halted endpoint when the resubmit is refused
- [ ] **Confirm a real transaction error is absorbed invisibly.** Not done, and it cannot
      be done by injection: a faked fault discards a transfer that really arrived, so the
      stream breaks afterwards with `SSL_READ` whatever the recovery does. What injection
      did prove is the mechanics, which is not nothing given the resubmit refusal above
      went unnoticed for a night. This needs one real fault, and they are erratic: every
      few minutes one evening, none in 46 minutes the next morning.

Do not read a long clean run as proof. Twice now a session has run for tens of minutes
with the retry never once executing, which says the link behaved, not that the code works.
Check `Transaction error on endpoint` in the log before concluding anything.

### Fault injection

The failures above take minutes of real use to appear once, or do not appear for an hour,
and unplugging the cable tests the case that already worked. Two knobs make them
reproducible on demand:

| Knob | Exercises |
|---|---|
| `AA_FAULT_TRANSPORT_AFTER=20` | the reconnect path, by killing the transport without touching USB |
| `AA_FAULT_TRANSFER_AFTER=400` | the retry path, by turning the Nth bulk IN into a transaction error whose resubmit is refused as a halted endpoint |


---

## M6. Audio output (media, system, speech)

Goal: the phone's audio comes out of the machine's speakers, mixed the way a head unit
is supposed to mix it.

- [x] `MediaAudioService`, `SystemAudioService`, `SpeechAudioService` channels
- [x] PCM sink abstraction, first backend PipeWire via `libpulse-simple`
- [x] Correct stream formats: media 48 kHz stereo 16 bit, system and speech 16 kHz mono 16 bit
- [x] Audio focus request/response handling, duck media under speech
- [x] Expose per stream volume and mute through the Dart API
- [x] Let the host app choose the output device (the infotainment system may route audio itself)
- [x] Optional: expose raw PCM to Dart for apps that want to do their own mixing
- [x] Underrun and xrun handling, no audible glitches over a 10 minute playback
- [ ] **The system sink has never carried a buffer.** The phone opens it and accepts the
      setup response, and it is the same code as the two that work, but nothing in a
      session produced one: not a shell notification, not the volume keys, not touch
      feedback with `sound_effects_enabled` on. Advertised, answered, never heard.

**Checkpoint met.** Music from the phone playing through the laptop's speakers, at the
volume the Flutter app asked for, ducking under navigation prompts and coming back up.

### Measured

| | |
|---|---|
| Continuous media playback | **11 minutes, 0 underruns, 0 buffers dropped** |
| Buffer held at the server | 118 to 161 ms, never outside it |
| Volume, 100% against 25% | -11.4 dB measured, -12.0 dB asked for |
| Mute | digital silence, peak sample 0 |
| Duck under a guidance prompt | engaged for 2.3 s, released 0.5 s after the last speech buffer |
| Output device change while playing | stream reopened on the named sink in 40 ms |

Volume and ducking were checked by recording the sink monitor and reading the envelope,
not by listening. That is worth keeping in mind for the next audio change: `parecord`
on `@DEFAULT_MONITOR@` plus a per-100 ms RMS is what turns "sounds right" into a number.
The first attempt at the volume check compared two recordings a minute apart and got
-2.2 dB for a -12 dB change, because the track had moved on to a quieter passage. Back
to back, same passage, is the only way it means anything.

### The head unit is what ducks

Android Auto sends media, system and speech as three separate channels. The phone
therefore *cannot* duck its own music under its own navigation prompt: by the time the
two exist they are on different channels heading for different speakers, and whoever
mixes them is whoever ducks. openauto does not, which is a known complaint about it.

Media drops to 25% while the speech stream is carrying audio, and for 500 ms after, so
the pauses inside one spoken instruction do not make it flutter. The change is ramped
over 20 ms rather than stepped, because a step on a music stream is an audible click and
a click under a navigation prompt is more noticeable than the ducking it announces.

Deliberately **not** driven by the audio focus request. A phone asks for
`GAIN_TRANSIENT_MAY_DUCK` before its own media as well as before a prompt, so ducking on
the request makes the media stream duck against itself. The speech channel carrying
audio is the unambiguous signal, so that is the one used. The focus exchange still got a
real state machine, because the answer changes the phone's behaviour: a transient
request is now answered `GAIN_TRANSIENT` rather than `GAIN`.

Watch for `[Audio] media ducked under speech` in the log. It is logged once per
transition, because the alternative way to answer "did it duck" is to record the
speakers and read the envelope, and that is not a thing to repeat.

### What an underrun is, and the first definition that was wrong

The first version counted an underrun whenever the server's buffer fell below 25 ms.
Every navigation prompt then reported one, and it was not wrong about the buffer: a
stream the phone feeds in **real time** never gets ahead of the speakers, so a spoken
instruction sits at 10 ms buffered from its first sample to its last. Nothing skipped.

The counter now only arms once the buffer has been comfortably full on that open, which
is the difference between a stream that ran ahead and then starved, and one that was
never ahead in the first place. Media reaches 150 ms and stays there; speech never does
and never counts.

### Acknowledge on queue, not on playback

A buffer is acknowledged the moment it is handed to `AudioOutput`, not when it is heard.
The phone is allowed ten buffers ahead, the queue here is bounded at one second and
drops oldest first, and the real pacing is `pa_simple_write` waiting on the speakers.
Acknowledging on playback instead would put a USB round trip inside the audio clock.

Blocking is the whole point of the design, and it is why each stream has a thread of its
own: waiting on the speakers from an io_context thread would stall the USB transport,
which is the thing that makes a phone drop the session.

---

## M7. Microphone input

Goal: the Assistant hears what is said in the car.

- [x] `MediaSource` (microphone) channel, 16 kHz mono 16 bit
- [x] PipeWire capture source, selectable device
- [x] Start and stop on the phone's request only, never capture otherwise
- [x] Expose a "mic active" flag to Dart so the app can show an indicator
- [x] Verify "Hey Google" and the mic button both work. Both do, but only the mic button
      goes through this code: see "Hey Google does not use the head unit's microphone".

**Checkpoint met.** The mic button on the test bench's keypad opens the Assistant, a
spoken sentence reaches it through the laptop's microphone, and it acts on it: "Navigiere
nach Hamburg" put a route to Hamburg on the projection. Transcription came back with the
question mark on "Wie ist das Wetter in Berlin?", which is the recogniser rather than a
guess at what arrived.

### Measured

| | |
|---|---|
| Capture format | 16 kHz mono 16 bit, confirmed as the stream PipeWire opened |
| Buffer | 1024 bytes, 32 ms, one USB write each |
| Open to first buffer | 45 ms |
| Buffers dropped, five Assistant sessions, ~740 buffers | **0** |
| Buffers the phone acknowledged | **0**, see below |
| Recording streams while the phone has not asked | **none**, checked with `pactl` |
| Stop pressed mid capture | microphone closed 9 ms before the goodbye went out |
| Chosen input across a reconnect | kept |

### Nothing captures unless the phone asks

The one rule this milestone is really about. The capture device is opened when the phone
sends its microphone open request and closed when it sends the close, and there is no
other path to it: no Dart call starts recording, and `AudioInput` creates the thread on
Start and joins it on Stop, so "is this machine listening" has the same answer as "does
that thread exist".

It is checkable from outside the app, which is the point:

```bash
pactl list short source-outputs
```

Empty while idle. One `s16le 1ch 16000Hz` stream while the Assistant is listening. Empty
again a moment later, and empty immediately after Stop even when Stop lands mid sentence.
The desktop's own microphone indicator agrees, which is what a person in the car would
actually be going on.

### The phone asks for a window of two and then never acknowledges

`MicrophoneRequest` carries `max_unacked`, and this Pixel sets it to 2 on every open. It
then sends no `Ack` at all: zero across five sessions and roughly seven hundred buffers.

So a head unit that gated its sends on acknowledgements would stall after two buffers and
the Assistant would hear a 64 ms fragment of every sentence. The backpressure here is
counted against the **send** instead, from the moment a buffer is posted to the moment
the messenger has written it, which holds whether or not the phone ever answers. The
phone's number is honoured once it has acknowledged anything, and ten buffers (320 ms of
speech) until then; see `MicrophoneChannel::Window`.

Nothing was dropped either way, because a USB write completes in about a millisecond and
buffers arrive 32 ms apart. The bound is there for the marginal link of M5, not for this.

### "Hey Google" does not use the head unit's microphone

The hotword works, confirmed by a person saying it out loud, and it starts the Assistant
on the projection exactly as the mic key does. It just never reaches this code: the phone
listens for it on **its own** microphone, and only then asks the head unit for a
microphone for the query that follows.

Which means the one channel this milestone built is not on the hotword path at all, and
nothing here can put it there. Worth knowing before chasing it: the phone opened the
microphone channel at service discovery and then never asked to record until the hotword
had already fired or the mic key was pressed, so a head unit in a noisy car cannot make
"Hey Google" more reliable by improving its own capture.

Also worth knowing for anyone testing this the way the rest of M7 was tested: the
synthetic speech from `spd-say` does not trigger the hotword, because Google's hotword
stage does speaker verification and a synthesised voice is meant to fail it. That is a
property of the test method, not a finding about the head unit.

### What is not implemented

**Echo cancellation and noise suppression are not implemented.** The phone asks for both
in the open request (`anc_enabled`, `ec_enabled`) and both arrive as 0 from this phone.
They are hints about what the head unit's hardware has already done rather than a
request, so nothing is owed, but in a real car with music playing they are the difference
between a recogniser that works and one that does not. Logged at debug so a poor
recognition rate has somewhere to start.

### Testing this without a person in the room

Speech through the speakers into the laptop's own microphone, which is the acoustic path
a car has:

```bash
spd-say -l de -r -20 -w "Navigiere nach Hamburg"
```

One warning from doing it: a virtual capture device made with
`pactl load-module module-null-sink media.class=Audio/Source` broke recording **machine
wide** on this PipeWire, and the symptom was `pa_simple_new` timing out after 30 seconds
against a device that had worked a minute earlier. That looked exactly like a bug in the
capture code and was not one. Unload it before concluding anything.

---

## M7b. Phone calls over Bluetooth HFP

Numbered M7b rather than inserted as M8 because it was never in the plan, and renumbering
everything after it would make "M8" mean two different things depending on which commit
you are reading.

Goal: a call made on the phone is heard and answered through the car, not through the
phone.

- [x] Establish whether call audio can use the projection link at all. **It cannot.**
- [x] Pair the phone and confirm Android Auto stops reporting no Bluetooth
- [x] Confirm the downlink reaches the head unit's speakers
- [x] Confirm the uplink is routed from the head unit's microphone to the phone
- [x] Echo cancellation, measured rather than assumed
- [x] Stop an empty device name following the audio server's default onto a phone
- [ ] Confirm the far end actually hears the driver clearly. Needs a two way call with a
      person on it, which no script on this machine can stand in for.
- [ ] Confirm what media does while a call is up. Probably the phone's business, unchecked.

**Checkpoint met, with one honest gap.** A call placed on the phone comes out of the
machine's speakers and the machine's microphone goes back up to the phone, with the echo
cancelled. What has not been confirmed is how the driver sounds at the other end, because
that needs someone to listen.

### Measured

| | |
|---|---|
| Transport | Bluetooth HFP, this machine as the hands free unit |
| Codec negotiated | LC3-SWB, super wideband |
| Downlink routing | `bluez_input` to the default sink, made by WirePlumber |
| Uplink routing | the default source to `bluez_output`, made by WirePlumber |
| Code written here to carry call audio | **none** |
| Echo reaching the microphone | +19.4 dB above the room floor |
| Echo left after cancellation | 56.8 dB down, below the uplink's own idle noise floor |
| Default sink or source moved onto the phone | never, across 166 snapshots |
| Bluetooth node lifetime | created when SCO comes up, gone with the call |

### Android Auto does not carry call audio, and the Bluetooth channel is not needed

Calls go over HFP. The projection link carries media, system and guidance audio and the
microphone, and that is the whole list. `AUDIO_STREAM_TELEPHONY` and
`MEDIA_SINK_TELEPHONY_AUDIO` exist in the schema and aasdk even ships a
`TelephonyAudioChannel`, but nothing opens them. Do not spend an evening on it.

The expensive assumption that turned out to be wrong: that the head unit must advertise
its Bluetooth address over the projection link, through aasdk's `BluetoothService`
channel, before the phone would route a call to it. It does not. Ordinary out of band
pairing was enough for Android Auto to stop reporting no Bluetooth and to put a call
through. That channel may still earn its place in M10, where a wireless handover has no
other way to hand the phone the head unit's details, but it is not what makes calling
work.

### Both directions are bridged by WirePlumber, which is why there is no code here

With the phone paired, its card carries one profile, `Audio Gateway (A2DP Source &
HSP/HFP AG)`, describing the **phone's** role: the phone is the gateway, this machine is
the hands free unit. That is the car kit role.

When a call starts, PipeWire creates two nodes, and they are ordinary streams rather than
devices, which is the part that matters:

```
bluez_input.<addr>.0   ->  the default sink      (far end to the speakers)
the default source     ->  bluez_output.<addr>.1 (microphone to the phone)
```

Because they follow the defaults, nothing has to be told a device name, on any machine.
Both nodes exist only while SCO is up, so a snapshot taken between calls shows nothing and
proves nothing. `dev/audio-graph.sh --watch` records across a call.

### Echo cancellation is the whole of the work

A head unit plays the far end through the car's speakers and listens in the same cabin, so
without cancellation it sends the far end straight back, a few hundred milliseconds late.
The phone will not do it: a phone on speakerphone cancels its own echo, but once the call
is on a hands free unit it no longer knows what the speakers played or when. `ec_enabled`
is the phone asking whether this was handled, not offering to handle it.

This is configuration rather than code: a PipeWire drop-in in `tools/config/`, installed
by `tools/install-echo-cancel.sh`, reasoned through in `docs/echo-cancellation.md`. It
names no device, and the canceller's virtual source outranks every real capture device so
it becomes the default input, which is what puts it in front of both the call uplink and
the Android Auto microphone channel without either being wired to it.

It needs no per machine calibration. The filter estimates the echo path, its delay and the
way the cabin colours it, from the signals themselves at runtime, which is how it reached
56.8 dB on a laptop it had been told nothing about. What it cannot survive is a microphone
driven into clipping by the speakers, because that breaks the linear relationship it
depends on. That is a gain staging problem, not a settings problem.

The trap when measuring it: at a quiet volume the speakers never reach the microphone, so
both recordings look identical and a canceller doing nothing at all looks perfect. The
first measurement taken here fell into exactly that and reported 34.9 dB of cancellation
that was not happening. Check the raw microphone actually rises above its silent floor
first.

### A phone must not be able to become the car's speakers

An empty device name in the audio API used to mean "the audio server's default", and the
audio server moves its default onto a Bluetooth device when one connects. Pairing a phone
could therefore have dragged the head unit's own media onto the phone's uplink. It now
means the head unit's own hardware, resolved to a concrete non Bluetooth device, and an
explicitly named Bluetooth device still works. See `DefaultHeadUnitDevice` in
`pulse_sink.cc`.

This one is defensive. On this machine WirePlumber never actually moved either default,
not once across a whole call, so it is a rule with no observed fault behind it.

## M8. Sensors

Goal: the head unit stops pretending. Until now the sensor channel answered every
subscription with a fixed "parked" and "day" so the phone would project at all; now the
host app says what the car is doing and the phone acts on it.

- [x] `SensorService` channel with the sensor list we advertise
- [x] Night mode sensor, driven by the host app (Dart sets day/night)
- [x] Driving status sensor (parked/moving), driven by the host app
- [x] GPS location sensor, fed from Dart so the host app owns the GPS hardware
- [x] Optional: speed, RPM, fuel, gear, compass, environment, and five more
- [x] Dart API to push sensor values, with sensible defaults if the app pushes nothing
- [ ] Confirm what a phone does when an advertised sensor is never fed. Reasoned about
      at length and designed against, never actually measured.

**Checkpoint met.** Night mode flips the phone's map between its light and dark themes.
Moving puts "Während der Fahrt nur Spracheingabe" in the Maps search bar and takes the
keyboard away; parked gives it back. A position pushed from Dart moves the phone's map
to it, which is the whole of the GPS sensor working: the phone had stopped using its own
receiver and was drawing the fix this machine handed it.

### Measured

| | |
|---|---|
| Sensors implemented | 12 |
| Sensors the example advertises | night mode, driving status, location |
| Subscriptions the phone raised, of those three | **3** |
| Subscriptions the phone raised when all 12 were advertised | **8**, see below |
| `min_update_period` the phone asked for | 0, except speed and compass at 3 |
| Readings sent in a seven minute session | 88, all of them the 1 Hz position feed |
| Sensor channel errors, failed sends, dropped connections | **0** |
| Night mode to the phone's map changing | under a second |
| Values surviving a stop and a start | night mode and the position, both |

### The phone subscribes to eight of the twelve, and not the four you would guess

Advertising every sensor once, to find out what this Pixel actually wants:

```
driving status, night mode, speed, gear, parking brake, location, toll card, compass
```

Not subscribed: **rpm, fuel, environment, odometer**. Which is a reasonable division:
the eight are things Android Auto can do something with (restrict the interface, draw a
map, decide the car is reversing), and the four are dashboard readings it has no use
for. They are implemented anyway, because a head unit is not the thing that gets to
decide what a future Android Auto finds interesting, and because the phone asking is
not the only reason to have them: M9 wants the same values for the app's own widgets.

### `min_update_period` is a number nothing here can read

Every subscription carries one, as an `int64`, and the schema does not say what of.
This phone sends **0** for six of the eight and **3** for speed and compass.

Three of nothing useful. Microseconds would be 333 kHz, milliseconds 333 Hz, seconds
would hold speed back to once every three seconds and be actively wrong. So the unit is
not determinable from one phone sending one number, and the code reads it as
microseconds, which is the conservative choice: under that reading the limiter never
fires, and the two readings that would be wrong are the slow ones.

The rate limiter exists anyway, in `SensorChannel::Offer` and `Flush`. It holds the
newest value rather than dropping it, coalesces everything that comes due into one
batch, and caps any hold back at five seconds whatever the phone asked for, because a
driving status stuck in the past is exactly what locks a user out of the interface. It
has never fired against a real phone. Do not delete it on that basis; do not trust it on
that basis either.

### Advertising a sensor is a promise, and location is the one that bites

The phone stops using its own receiver the moment the head unit says it has a position.
Not "prefers": stops. So a head unit that advertises `SENSOR_LOCATION` and then never
sends a fix has taken navigation away from a phone that was managing perfectly well, and
it has done it silently.

Which is why the advertised set is `AndroidAutoConfig.sensors`, read once at service
discovery, and why it defaults to the two that are not optional rather than to
everything. It is the host app declaring what the car has. A subscription to anything
outside that set is answered with `STATUS_INVALID_SENSOR` rather than accepted, because
a phone told no falls back to what it can do itself and a phone told yes waits.

The example advertises location, and therefore feeds a fix every second from the moment
it starts. That is the obligation, honoured. A head unit with no receiver takes location
out of the set.

### A sensor with no value is not a sensor reading zero

The difference between a car that has not got a fix yet and a car in the Atlantic. A
sensor the host app has never set is not put in a batch at all, and `SensorState`
tracks that per sensor rather than inferring it from the value.

Night mode and driving status are the two exceptions, and they start at day and
unrestricted rather than at nothing, because the phone waits for an answer to those
before it finishes opening its interface. That is not a default in the sense of a
preference, it is the only pair of answers that leaves a person sitting in a parked car
able to use the screen in front of them.

The same reasoning runs through the optional fields inside a reading. A `LocationData`
with no altitude leaves the field absent rather than setting it to zero, because sea
level is a real altitude, zero bearing is due north and zero speed is standing still.
Dart spells that `null`, the C ABI spells it `NaN`, and `SensorChannel::Send` is where
the two meet.

### What the car is doing outlives the connection

`SensorState` is process lifetime, beside the video decoder, the audio output and the
microphone, for a plainer reason than any of them: a parking brake does not come off
because a cable was pulled out. Verified by turning night mode on, stopping, starting,
and watching the phone come back with a dark map.

It also means a host app can set values before it has ever started a session. The Dart
side keeps the write closures rather than the values, so replaying them into a new core
is the same code path as setting them in the first place; see `_writeSensor` and
`_applySensorSettings` in `android_auto_linux.dart`.

### The driving restrictions are a set, not a switch

`DrivingStatus` is a bitmask: no video, no keyboard, no voice, no configuration, limit
message length. Parked is none of them. "Moving" is not defined by the protocol at all,
so `setParked(false)` picks video, keyboard and configuration and says so, leaving voice
alone because voice is the one interaction that is safe at speed, and leaving message
length alone because truncating messages is a choice about content rather than about
safety. An app that disagrees calls `setDrivingRestrictions` with its own set.

Worth being deliberate about: the phone locks the matching parts of its interface
immediately and nobody in the car can override it.

### Testing this without a car

The example's sensor panel drives all three of the first class sensors and shows what
the phone actually subscribed to, which is the first thing to look at when a value is
being set and nothing changes on the screen. The subscription list and the reading count
come from the core, not from what Dart believes it sent.

One trap, and it cost a quarter of an hour: **the phone can be pinned to night**.
Android Auto has its own setting, Einstellungen, Display, Tag-/Nachtmodus für Karten,
with Tag, Nacht and Automatisch. Only Automatisch reads the head unit's sensor, and a
phone sitting on Nacht makes a perfectly working night mode sensor look like a no-op in
both directions. Check that before touching any code.

---

## M9. Metadata channels for native Flutter UI

The point of this project: the host app should be able to render its own widgets from
Android Auto state, not just mirror pixels. Everything before this gets the phone's
screen onto a texture; this is what lets a head unit draw its own.

- [x] `NavigationStatusService`: turn by turn instructions, distance, maneuver icons
- [x] `MediaPlaybackStatusService`: track, artist, album, playback state, position
- [ ] `PhoneStatusService`: call state, caller id. The channel opens, a message arrives
      and decodes, but no call was placed during the test, so the call fields
      themselves are unverified.
- [ ] `MediaBrowserService`: browse and search the phone's media library. Implemented
      both ways, never exercised: **this phone does not open the channel.** See below.
- [ ] `GenericNotificationService`. Same, and the subscribe that would start it has
      never been sent for real.
- [x] Model all of these as Dart classes with streams in `android_auto_platform_interface`
- [x] Example app shows a native Flutter "now playing" bar fed by M9 data, over the projection

**Checkpoint met.** A Flutter now playing bar with the cover art, the title, the artist,
the app and a running progress bar, drawn over the projection from the playback channel
rather than read off the phone's pixels. Beside it a turn card with an arrow chosen from
the maneuver, the distance to it, the road it leads onto, the arrival time and the
destination address, which appears when guidance starts and vanishes when it ends.

### Measured

| | |
|---|---|
| Channels implemented | 5 |
| Channels this Pixel opened, of the five advertised | **3**: navigation, playback, telephony |
| Channels it never opened | generic notification, media browser |
| Navigation messages in one short session | 66, all decoded |
| Playback messages while a track played | about one a second |
| Telephony messages | 1 per connection, with no call in progress |
| Message ids that reached the "not decoded" log | **0** |
| Failed sends, parse failures, channel errors of our own making | **0** |
| Projection after adding five channels to service discovery | unchanged, 1280x720 VA-API |

### aasdk names these channels but does not speak them

Four of the five have a service class in aasdk and only two parse anything, and those
two stop at the message ids openauto's phones were sending: the turn event and the
distance event, **both marked deprecated in the schema**, and not the navigation state a
current phone actually sends. `PhoneStatus`, `MediaBrowser` and `GenericNotification`
parse nothing past the channel open and log an error for everything else.

Extending the vendored submodule would have meant five more classes inside it and a much
larger patch to carry for ever, for messages this plugin is the only consumer of.
aasdk's `Channel` base is public and does the part that is actually hard, so instead
there is one `MetadataChannel` here that answers the open request and hands every other
message to a decoder, and one decoder per channel in `metadata_channels.cc`. The patch in
`linux/patches/` did not grow by a line.

### What this phone opens, and what it ignores

Advertising all five and watching:

```
opened:      NAVIGATION_STATUS, MEDIA_PLAYBACK_STATUS, PHONE_STATUS
not opened:  GENERIC_NOTIFICATION, MEDIA_BROWSER
```

The three it opens are the three it pushes without being asked. The two it ignores are
the two where the head unit has to speak first: a notification needs a subscribe, and a
browse needs a request. So the default advertised set is those three, and the other two
are opt in through `AndroidAutoConfig.metadata`. The example advertises all five anyway,
because a test bench that cannot exercise a channel cannot tell whether it works.

Which leaves both of them **implemented and unverified**. The decoders are written
against the schema and the send paths exist; nothing has ever answered them. Do not
record them as working on the strength of the code compiling.

### A channel that exists is not a channel that is open

The first real bug this milestone produced, and the same one the input channel had.
`Browse()` found the channel object, which exists from the moment the channel is
advertised, and sent a request on it. The phone had never opened it, so the request went
nowhere with nothing to say so: the media library simply stayed empty for ever.

`GetOpen()` now refuses unless the phone has actually opened the channel, and the host
app is told no. The example prints the refusal rather than showing an empty list, which
is the difference between a bug report and a fact.

### Android Auto splits one picture across several messages

A head unit wants one object with the turn and the distance to it in. The protocol sends
the shape of the turn on `INSTRUMENT_CLUSTER_NAVIGATION_STATE` and the distance on
`INSTRUMENT_CLUSTER_NAVIGATION_CURRENT_POSITION`, and the track's title on
`MEDIA_PLAYBACK_METADATA` and whether it is playing on `MEDIA_PLAYBACK_STATUS`. So
`MetadataState` merges rather than replaces: each message writes its own fields and every
update publishes the whole.

Two exceptions, both deliberate. A `PhoneStatus` carries the entire call list every time,
so that one **replaces**: a call that has ended is a call no longer in the list, and
merging would leave it on screen for ever. And a navigation status of anything but active
or rerouting **clears** the turn fields, because an instruction left on screen after
guidance ends is the one mistake a head unit can make that actively misleads a driver.

### Ask for ENUM, not IMAGE

`NavigationStatusService` has to say which of two instrument cluster types it is. IMAGE
asks the phone to render each turn arrow and send it as a picture sized to
`image_options`, which is what a car with a fixed cluster display wants. This is a
Flutter app: it would rather be told the turn is a normal left and draw that itself, at
its own resolution in its own style.

That choice is also what decides which messages arrive. Under ENUM the phone sends the
navigation state and current position; the deprecated pair carries neither lanes nor a
destination. Both are decoded, because a phone on an older build sends only the
deprecated pair, but only one of them has ever been seen on this machine.

The two message sets name the same turns differently, and that would have leaked into the
API: `slightTurn` plus a separate side field from the old one, `turnSlightLeft` from the
new. `ManeuverFromTurnEvent` folds the old vocabulary into the new one, so a host app
learns one list of forty three names rather than two.

### One JSON string per update, and why

Everything else in the C ABI is a scalar or a NUL terminated string. None of this fits:
a turn carries a lane diagram and a list of destinations, a call list has a photo per
entry. So each update crosses as one UTF-8 JSON object, built by a seventy line writer
in `metadata/json.cc`, with pictures base64 inline.

That buys one owner, one `aa_string_free`, and a snapshot getter that returns everything
rather than everything except the pictures. It costs a third on the images, which are
encoded once where they arrive rather than on every update that carries them along, so a
playback position ticking once a second does not re-encode fifty kilobytes of cover art.

The absent-is-not-zero rule from M8 runs all the way through: a field the phone did not
send is absent from the object, not present and zero, and comes out as `null` in Dart. A
track with no album and a track on an album called "" are different questions.

### What the phone is playing is not like what the car is doing

`SensorState` is process lifetime because a parking brake does not come off when a cable
is pulled out. `MetadataState` is the opposite and says so: the music stops. The object
lives as long as the session so a host app can keep one listener across reconnects, but
its contents are cleared when a connection ends, and the Dart side clears its snapshots
on `stop()` for the same reason. Verified: pressing stop takes the now playing bar away
rather than leaving a track that finished.

---

## M10. Wireless Android Auto

- [x] Bluetooth RFCOMM service advertising the Android Auto Wireless UUID
- [x] `WifiProjectionService` handshake: send Wi-Fi SSID, password, IP, port to the phone over RFCOMM
- [x] TCP transport to the phone on port 5288, then the same SSL and channel stack as M3
- [x] Handle the host being the Wi-Fi AP versus joining an existing network
- [x] Reconnect on Wi-Fi drop
- [x] Dart API to start/stop wireless mode and list paired phones
- [x] A wireless connection must never displace a session that is already connected
- [x] Exercise the whole path without a phone (`AA_WIRELESS_FAKE_PHONE`)
- [x] **A real phone projects over Wi-Fi.** All twelve channels, video decoded, one session with no reconnects, ended by the phone saying goodbye
- [x] Microphone and speech audio over Wi-Fi
- [x] Wireless enabled from `AndroidAutoConfig` alone, no environment variables and no shell script
- [x] A head unit that is not projecting refuses phones rather than ignoring them
- [ ] Media audio, touch input and the sensor channel watched over Wi-Fi rather than over the cable. None is transport specific, so none is expected to differ, but none has been observed

Full write up in `docs/wireless.md`.

### What was verified

On a Pixel 8 Pro, over an access point this machine hosted: one Bluetooth channel, one
dial-in, version 256.1792, SSL, service discovery, and all twelve advertised channels
opened. Video ran 1280x720 through VA-API at 1.0 to 1.4 ms from wire to frame,
indistinguishable from the cable. The Assistant was invoked twice, capturing microphone
buffers and answering on the speech stream, with nothing dropped and no underruns.

### The head unit listens on both legs

The phone advertises no Android Auto UUID of its own, so the head unit is the RFCOMM
server; and it fills in the `ip_address` in `WifiStartRequest`, so it is the TCP server
too. Neither was apparent from the protobuf.

### Two things decide whether a phone will ever ask

Both fail identically and silently, which is what makes them expensive.

**A phone reads the service list at pairing time** and decides from it that a machine
is a wireless car. One paired before the service existed never asks, however long it is
advertised afterwards.

**BlueZ publishes a useless record unless it is given an RFCOMM channel.** With no
`Channel` in the `RegisterProfile` options the protocol descriptor list is L2CAP and
nothing else, so a phone reads it, finds nothing to dial, and disconnects. Neither end
logs anything. This is why every working implementation hardcodes a channel number.

### Only the passphrase has to be configured, and a hosting head unit needs a MAC

SSID, BSSID and address are read off the machine; a passphrase cannot be read by
anything unprivileged. The exception is access point mode, where `SIOCGIWESSID` and
`SIOCGIWAP` both answer `EINVAL` while `SIOCGIWMODE` works, so a hosting head unit
correctly knows it is hosting and reads an empty BSSID in the same breath. An empty
BSSID is silently fatal: the phone rejects the offer without scanning and calls it
incorrect credentials. When hosting, the BSSID comes from `SIOCGIFHWADDR`.

Bringing a network **up** is not this plugin's business, the same call
`docs/echo-cancellation.md` makes about the echo canceller.

### The start request is an instruction

`WifiStartRequest` carries the address to dial, so the phone connects when it arrives.
Sent before the phone has a network it is answered and forgotten and the phone never
dials in; sent to a phone that is already projecting it tears down a working session to
obey. Once, when the phone reports it is on the network, is the only right number of
times.

### Not projecting means refusing, not going quiet

A phone that knows this machine asks for the service every five seconds for as long as
Bluetooth is connected and does not stop, showing the driver a notification meanwhile.
Withdrawing the service stops the answering, not the asking. Refusing makes it give up,
and is measurably cheaper for the phone: zero service queries over a hundred seconds
against a query every 5.1 seconds. So the Bluetooth service is published whenever the
host app has configured wireless, and a head unit that is open but not started answers
no. It is never published when wireless is left out of the transports, which is what a
head unit wanting Bluetooth music and nothing else should do.

The one cosmetic residue: a notification the phone has already shown is not taken down
by the refusal. It can be swiped away and stays away, because a refused phone does not
retry.

### A phone hosting a hotspot cannot join the head unit

Which rules out testing this on a machine whose only internet is that hotspot. Use
Bluetooth tethering for the machine's network, or put both ends on an ordinary one.

## M11. Packaging, ARM64, CI, docs

- [ ] Cross build and test on ARM64 (the target mini PC)
- [x] Cache the aasdk build so a clean `flutter build linux` is not a 10 minute wait
- [x] Decide static versus shared linking of aasdk into `libandroid_auto_linux_plugin.so`
- [x] Document the runtime dependencies an end user has to install
- [x] GitHub Actions: build x86_64 and ARM64, run unit tests
- [x] `README.md` with a real screenshot, quick start, and the licence situation spelled out
- [x] Dart API docs on every public member
- [ ] Example app polished enough to serve as the reference integration

The remaining boxes are ARM64, which needs the device and a Flutter installed from a
git clone rather than from the x64 only prebuilt archives, and the example app, which
is one 1375 line `main.dart`: a good test bench, not yet a reference integration.

### The bundle did not work anywhere but here

Found by copying `build/linux/x64/release/bundle` somewhere else and running `ldd` on
the plugin, which is the whole test and takes ten seconds:

```
libaasdk.so.2026       => not found
libaap_protobuf.so.2026 => not found
```

aasdk versions itself by the day it was built, so a shared build is
`libaasdk.so.2026.09.17+git.9bf6adf` with a `libaasdk.so.2026` SONAME symlink beside it.
`android_auto_linux_bundled_libraries` had been pointed at the SONAME, on the reasoning
that the SONAME is what `DT_NEEDED` records, so the SONAME is what has to be in the
bundle. Flutter's install step then copied the symlink and not its target, and the
result kept working on this machine anyway, because the plugin's `RUNPATH` is an
absolute path into the build tree. The first machine that would have noticed is one
that did not have the build tree, which is to say every machine it would ever ship to.

**Linking aasdk statically is the fix and the answer to the checkbox above.** Nothing
else on a head unit links aasdk, so a shared copy is shared with nobody, and the licence
position is identical either way. The bundle is now three libraries, all real files, and
the relocation test passes.

Two things had to follow it:

| | |
|---|---|
| Position independent code | A static archive going into a shared object needs it. Set on the directory, since the targets belong to aasdk |
| A linker version script | `CXX_VISIBILITY_PRESET hidden` only reaches the plugin's own objects, so the plugin was exporting **6159** dynamic symbols where 65 are the interface. All of protobuf and Boost.Log, in a process where Flutter loads every plugin into one address space and the first definition of a symbol wins |

### Three things found while packaging, each worth its own line

- **The plugin shipped 23 MB of DWARF.** aasdk overrides `CMAKE_CXX_FLAGS_RELEASE` with
  `-g -O3 -DNDEBUG`. The release plugin was 26 MB; it is 3.3 MB now. CMake has
  `RelWithDebInfo` for anyone who wants the symbols.
- **Every consumer of aasdk linked Boost.Test.** `find_package` asks for
  `unit_test_framework` unconditionally and `${Boost_LIBRARIES}` carries it into aasdk's
  PUBLIC link interface, so `libboost_unit_test_framework` was in the plugin's
  `DT_NEEDED` with no test in sight.
- **`tools/port-aasdk.sh apply` did not reproduce the patch.** The USBEndpoint transfer
  retry, the halt clearing and `AA_FAULT_TRANSFER_AFTER` are hand written and live in no
  function in the script, so regenerating the patch from `apply` silently deleted about
  130 lines of the port. Caught by a patch that came out *smaller* after adding to it.
  `apply` now applies the committed patch, the script's transforms moved to `regen`, and
  both the script and `docs/aasdk-port-notes.md` say which does what and why it matters.

### ccache, because `flutter clean` is the expensive part

aasdk and its generated protobuf are 346 of the build's translation units. `flutter
clean` throws all of it away, which is what made a clean rebuild feel like a punishment.
ccache keys on preprocessed source and lives in `~/.cache`, so it survives.

| | Wall clock |
|---|---|
| `flutter clean`, then build, cold cache: 346 compiles, 0 hits | 196 s |
| `flutter clean`, then build, warm cache: 346 compiles, 346 hits | 20 s |

CMake finds it if it is installed and says at configure time which case it is in.
Nothing requires it, `-DAA_USE_CCACHE=OFF` turns it off, and CI restores the same cache
between runs.

### The first tests in the repository

There were none, and `flutter test` from the root does not work in a workspace whose
root is not a package: it looks for `./test` and stops. Both `CONTRIBUTING.md` and
`AGENTS.md` told contributors to run exactly that. The command is `flutter test
packages/*/test`, and all three files now say so.

45 tests, over the layers that have no native dependency and are worth holding still:

- **Metadata decoding**, 18 tests. This is a seam between `metadata/json.cc` writing
  JSON and Dart reading it, and a mismatch there is completely silent: nothing throws,
  a turn card just never fills in. Covers the flattened distance keys, an empty string
  being an absent field rather than an empty one, a maneuver name from a newer schema
  landing on `unknown` rather than null, and values of the wrong type.
- **Touch mapping and MotionEvent rules**, 14 tests. Letterboxing undone so the centre
  of the box is the centre of the video, a tap on the bars dropped but a drag onto them
  clamped, the phone's own video size winning over the requested one, and the
  down/pointerDown/pointerUp/up sequence with `action_index` naming the finger that
  changed. Slots stay small and get reused.
- **Controller lifecycle**, 13 tests, against a fake platform.

One real bug fell out of writing them: `AndroidAutoManeuver.turnsLeft` and `turnsRight`
matched the roundabout direction at the end of the name, and two of the maneuvers carry
an exit angle and so end in `WithAngle`. Those two were the only maneuvers that were
neither left nor right, which for a head unit drawing two arrows means drawing neither.

### CI

`.github/workflows/ci.yml`, two jobs. `analyze and test` is pure Dart and answers in
under a minute: analyze, the 45 tests, and a dartdoc run whose warnings are read rather
than its exit code trusted, since dartdoc does not fail on an unresolved reference.
`build` is a matrix over `ubuntu-latest` and `ubuntu-24.04-arm`: the aasdk smoke test,
a debug and a release build of the example, and then the relocation check that would
have caught the bundle bug, as a step that fails the build rather than as a paragraph in
a document.

Two doc references in the platform interface pointed at `AndroidAutoView` and
`AndroidAutoController`, which live in the package that depends on it rather than the
other way round, so they could never resolve. Now code spans.

### ARM64 needs Flutter installed from a git clone, and that is all

The `aarch64` CI job failed in nine seconds, before compiling anything:

```
Unable to determine Flutter version for channel: stable version: 3.47.4 architecture: arm64
```

Read straight off `releases_linux.json`, which lists `x64` and nothing else, that looks
like Flutter not supporting Linux ARM64 at all. It was written up here as exactly that,
and it was wrong. **Only the prebuilt archives are x64 only.** Installing Flutter by
cloning its repository works on ARM64: the engine artifacts exist for `linux-arm64` in
debug, profile and release, the arm64 Dart SDK is published beside them, and
`flutter precache --linux` fetches them. Confirmed by hand on an ARM64 machine, and
then against the artifact URLs.

So the CI job now installs Flutter with the action on x64 and with a clone on arm64,
and the milestone is not blocked on anything but running it. What is still owed is the
device check: a phone, a cable, and the video, audio, input and sensor paths exercised
there. VA-API is a different driver stack on ARM and is the most likely thing to need
work, with the software decoder as the fallback if it does.

### The first release shipped a build that did not compile on Ubuntu 24.04

Found by CI, minutes after 0.1.0 went to pub.dev, which is the wrong order and the
subject of the entry below.

`metadata_channels.cc` suppresses `-Wdeprecated-declarations-switch-case` around the
navigation decoder. That warning arrived in clang 19. Ubuntu 24.04 ships clang 18,
where the name is an unknown warning group, and Flutter's `apply_standard_settings`
compiles with `-Werror`, so it is a hard error rather than a warning:

```
error: unknown warning group '-Wdeprecated-declarations-switch-case', ignored
       [-Werror,-Wunknown-warning-option]
```

The code already guarded against exactly this with `#pragma GCC diagnostic ignored
"-Wpragmas"`, and that guard is correct for GCC and useless for clang, which calls the
same thing `-Wunknown-warning-option`. Both names are now suppressed. It went unnoticed
because this laptop is on clang 21, where the warning exists and nothing fires.

**The lesson is about the gate, not the pragma.** The native build moved to
release-only, so it now runs *after* publishing and cannot block it, and the local build
check in `dev/release.sh` uses whatever compiler this machine has. One machine, one
compiler, is not a portability test.

---

## M12. Android implementation package

Sketch only, do not build yet. Recorded so the structure does not have to change later.

- [ ] `packages/android_auto_android` implementing `android_auto_platform_interface`
- [ ] On Android the host device can be the head unit over USB host mode, same aasdk core via NDK
- [ ] Video decodes with MediaCodec into a `SurfaceTexture` bridged to a Flutter texture
- [ ] Audio through AAudio/OpenSL
- [ ] The app-facing `android_auto` package and the example app must not need any change

---

## Open decisions

These need a call from the project owner. The first one blocks M1.

1. ~~**Licence.**~~ **Decided 2026-09-12: GPL-3.0-or-later for the whole repository.**
   `aasdk` is GPL-3.0-or-later, so `android_auto_linux` has no other option, and the
   infotainment app this plugin is built for is GPL-3.0 anyway. One `LICENSE` at the
   root covers every package.

   The federated split still matters, just not for licensing reasons today:
   `android_auto` and `android_auto_platform_interface` contain no aasdk derived code,
   so if a permissive protocol implementation ever appears they can be relicensed
   without untangling anything. Keep it that way.

2. **Head unit certificate.** Every open source implementation ships the same publicly
   known Google Automotive Link certificate. It is not a secret, but it is also not ours.
   The plugin will load it from a configurable path so integrators can drop in their own.

3. **Package names.** `android_auto`, `android_auto_linux`,
   `android_auto_platform_interface` are all free on pub.dev as of 2026-09-12.

4. **Minimum Flutter version.** Pinned to `>=3.24.0` for now. Revisit once the texture
   path is proven against Impeller on Linux.

5. **Renderer direction.** Flutter 3.47 made Impeller the only renderer on Linux, on its
   OpenGLES backend, and `--no-enable-impeller` is a verified no-op. Impeller's Vulkan
   backend for Linux desktop is in progress upstream, and Flutter GPU and `flutter_scene`
   both want Vulkan. The project therefore targets Impeller, treats GL as today's
   backend rather than the design, and hands Flutter a dmabuf so the present adapter is
   the only piece that has to change. Nothing here is a decision to make, it is a
   constraint to hold, but it needs watching: see "Upstream to watch" below.

---

## Upstream to watch

Check these when picking up M4, and again before M11.

- [ ] [flutter/flutter#183495](https://github.com/flutter/flutter/issues/183495) design
      doc, Impeller Vulkan backend for Linux and Windows desktop. Tracking issue #181711,
      draft PR #183382. When this lands, Linux external textures need a Vulkan path.
- [ ] Whether the Linux embedder gains a Vulkan external texture entry point, or
      `FlTextureGL` gets a GL interop shim over Vulkan. The answer decides how much of
      `present/vk_adapter.cc` we actually write.
- [ ] `flutter_scene` and Flutter GPU on Linux. The host app wants these, and the
      retained backend only activates on Metal and Vulkan contexts.

## Things that are easy to forget

- The example app has to be run with the host app drawing **over** the texture from day
  one, otherwise the whole premise goes untested until it is too late to change.
- Every native thread that touches GL needs the shared context made current first.
- The video pipeline produces a **dmabuf**, never "a GL texture". That one habit is what
  keeps the move to Impeller Vulkan from being a rewrite.
- aasdk's `io_context` must never be run on Flutter's platform thread.
- Touch coordinates are in the **projected** resolution, not the widget's logical pixels.
- Audio focus is a state machine, not a boolean. Get it wrong and speech cuts out.
- The phone will drop the connection if a channel that was advertised in service
  discovery is never serviced.
