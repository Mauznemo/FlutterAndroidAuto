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
| M4 | Video channel to Flutter texture | **next** |
| M5 | Input channel (touch, keys, rotary) | not started |
| M6 | Audio output (media, system, speech) | not started |
| M7 | Microphone input | not started |
| M8 | Sensors (night mode, GPS, driving status) | not started |
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

- [ ] `VideoService` channel: accept setup, config, start/stop indications
- [ ] H.264 Annex-B depacketisation from the AA media stream
- [ ] Decoder abstraction with two backends
  - [ ] VA-API via `libavcodec` with `AV_HWDEVICE_TYPE_VAAPI`, frames exported as dmabuf
  - [ ] Software `libavcodec` fallback, `AV_PIX_FMT_YUV420P`
- [ ] Zero copy path: dmabuf to `EGLImage` to GL texture (`EGL_EXT_image_dma_buf_import`)
- [ ] Fallback path: YUV to RGBA on the GPU with a small shader, or `FlPixelBufferTexture`
- [ ] Audit that nothing above the present seam names a GL type, so `vk_adapter` stays a
      contained addition when Linux moves to Impeller Vulkan
- [ ] Frame pacing: mark texture frame available on the Flutter raster thread, drop late frames
- [ ] Send `VideoFocus` requests so the phone knows the head unit is showing the projection
- [ ] Support 480p, 720p, 1080p and 30/60 fps, chosen from `AndroidAutoConfig`
- [ ] Handle resolution changes mid-session without tearing down the texture
- [ ] Measure end to end latency and log it

**Checkpoint:** a screenshot of the phone's Android Auto UI inside the example app,
with a Flutter status bar drawn over it.

---

## M5. Input channel (touch, keys, rotary)

- [ ] `InputService` channel setup, advertise touchscreen plus the buttons we support
- [ ] Flutter `Listener` to normalised coordinates to `TouchEvent` (down/move/up)
- [ ] Correct coordinate mapping when the Texture is letterboxed or scaled
- [ ] Multi touch (the protocol carries pointer ids)
- [ ] Hardware key events (back, home, play/pause, next, prev, call, mic)
- [ ] Rotary encoder / D-pad support for non touch head units
- [ ] Latency check: touch to visible reaction under 100 ms

**Checkpoint:** the agent taps around the projected UI via `tools/ui.sh` and the phone responds.

---

## M6. Audio output (media, system, speech)

- [ ] `MediaAudioService`, `SystemAudioService`, `SpeechAudioService` channels
- [ ] PCM sink abstraction, first backend PipeWire via `libpulse-simple`
- [ ] Correct stream formats: media 48 kHz stereo 16 bit, system and speech 16 kHz mono 16 bit
- [ ] Audio focus request/response handling, duck media under speech
- [ ] Expose per stream volume and mute through the Dart API
- [ ] Let the host app choose the output device (the infotainment system may route audio itself)
- [ ] Optional: expose raw PCM to Dart for apps that want to do their own mixing
- [ ] Underrun and xrun handling, no audible glitches over a 10 minute playback

---

## M7. Microphone input

- [ ] `MediaSource` (microphone) channel, 16 kHz mono 16 bit
- [ ] PipeWire capture source, selectable device
- [ ] Start and stop on the phone's request only, never capture otherwise
- [ ] Expose a "mic active" flag to Dart so the app can show an indicator
- [ ] Verify "Hey Google" and the mic button both work

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
