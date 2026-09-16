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
| M8 | Sensors (night mode, GPS, driving status) | **next** |
| M9 | Metadata channels for native Flutter UI | not started |
| M10 | Wireless Android Auto | not started |
| M11 | Packaging, ARM64, CI, docs | not started |
| M12 | Android implementation package | not started |

---

## M0. Project setup, research, tooling

- [x] Verify the dev machine can be driven headlessly by the agent (screenshot + input)
- [x] Research the Android Auto protocol and pick a base library (see `docs/research.md`)
- [x] Confirm Flutter Linux external texture support is viable (see `docs/architecture.md`)
- [x] `git init` and create the repository layout
- [x] Scaffold the federated packages and the example app
- [x] Write `PLAN.md`, `docs/research.md`, `docs/architecture.md`, `docs/dev-environment.md`
- [x] Write `tools/ui.sh` (screenshot and synthetic input helper for the agent)
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
- [x] SSL handshake with the head unit certificate
  - [ ] Certificate path override (`AaConfig.certificate_path` is accepted but ignored: aasdk hardcodes the certificate in `Cryptor.cpp`, so honouring it needs another patch)
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
| `tools/run-example.sh --bundle` | runs the built binary directly, line buffered. `flutter run` block buffers the app's stdout, so protocol logs arrive in 8 KB lumps minutes late |

`tools/run-example.sh` now also kills any instance already running. Two head units
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

## M8. Sensors

- [ ] `SensorService` channel with the sensor list we advertise
- [ ] Night mode sensor, driven by the host app (Dart sets day/night)
- [ ] Driving status sensor (parked/moving), driven by the host app
- [ ] GPS location sensor, fed from Dart so the host app owns the GPS hardware
- [ ] Optional: speed, RPM, fuel, gear, compass, environment
- [ ] Dart API to push sensor values, with sensible defaults if the app pushes nothing

---

## M9. Metadata channels for native Flutter UI

The point of this project: the host app should be able to render its own widgets from
Android Auto state, not just mirror pixels.

- [ ] `NavigationStatusService`: turn by turn instructions, distance, maneuver icons
- [ ] `MediaPlaybackStatusService`: track, artist, album, playback state, position
- [ ] `PhoneStatusService`: call state, caller id
- [ ] `MediaBrowserService`: browse and search the phone's media library
- [ ] `GenericNotificationService`
- [ ] Model all of these as Dart classes with streams in `android_auto_platform_interface`
- [ ] Example app shows a native Flutter "now playing" bar fed by M9 data, over the projection

---

## M10. Wireless Android Auto

- [ ] Bluetooth RFCOMM service advertising the Android Auto Wireless UUID
- [ ] `WifiProjectionService` handshake: send Wi-Fi SSID, password, IP, port to the phone over RFCOMM
- [ ] TCP transport to the phone on port 5288, then the same SSL and channel stack as M3
- [ ] Handle the host being the Wi-Fi AP versus joining an existing network
- [ ] Reconnect on Wi-Fi drop
- [ ] Dart API to start/stop wireless mode and list paired phones

**Note:** this is a large milestone. Do not start it before M3 to M6 are solid on USB.

---

## M11. Packaging, ARM64, CI, docs

- [ ] Cross build and test on ARM64 (the target mini PC)
- [ ] Cache the aasdk build so a clean `flutter build linux` is not a 10 minute wait
- [ ] Decide static versus shared linking of aasdk into `libandroid_auto_linux_plugin.so`
- [ ] Document the runtime dependencies an end user has to install
- [ ] GitHub Actions: build x86_64 and ARM64, run unit tests
- [ ] `README.md` with a real screenshot, quick start, and the licence situation spelled out
- [ ] Dart API docs on every public member
- [ ] Example app polished enough to serve as the reference integration

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
