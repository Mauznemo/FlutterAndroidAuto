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
| M1 | Native build: vendor and modernise aasdk | not started |
| M2 | C ABI core + Dart FFI + texture plumbing | not started |
| M3 | USB transport, AOAP, SSL, service discovery | not started |
| M4 | Video channel to Flutter texture | not started |
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
- [ ] Confirm an Android test phone is available and USB access works

---

## M1. Native build: vendor and modernise aasdk

Goal: `libaasdk.a` builds from source inside this repo on Ubuntu 26.04 and links
against the system Boost 1.90, protobuf 3.21, OpenSSL 3.5, libusb 1.0.29.

- [ ] Install host build dependencies (`tools/setup-dev-machine.sh --build-deps`)
- [ ] Add `opencardev/aasdk` as a git submodule under `packages/android_auto_linux/linux/third_party/aasdk`, pinned to a known commit
- [ ] Try a stock build, capture the full error list into `docs/aasdk-port-notes.md`
- [ ] Port `boost::asio::io_service` to `io_context` (95 files affected)
  - [ ] `io_service` to `io_context`
  - [ ] `io_service::strand` to `boost::asio::strand<boost::asio::io_context::executor_type>`
  - [ ] `strand.wrap(h)` to `boost::asio::bind_executor(strand, h)`
  - [ ] `ip::address::from_string` to `ip::make_address`
  - [ ] Keep the diff as a patch series under `packages/android_auto_linux/linux/patches/` so the submodule stays clean
- [ ] Build with `-DSKIP_BUILD_PROTOBUF=ON -DSKIP_BUILD_ABSL=ON -DAASDK_TEST=OFF` against system protobuf
  - [ ] If the bundled `.proto` files need protoc > 3.21, fall back to the vendored protobuf build and record why
- [ ] Verify the generated protobuf sources cover every channel we need (control, input, media sink/source, sensor, video, bluetooth, navigation, media playback, wifi projection)
- [ ] Produce a static `libaasdk.a` plus the generated protobuf objects, position independent (`-fPIC`), so it can be linked into a shared plugin library
- [ ] Add a standalone CMake smoke target that links aasdk and prints its version, to keep M1 regression-testable

**Risk:** Boost 1.90 removed `io_service`. Confirmed unavoidable, see `docs/research.md`.
**Fallback if the port fights back:** build a pinned Boost 1.86 into the repo, or drop
Boost Asio for a small hand-rolled epoll reactor behind the same interfaces.

---

## M2. C ABI core + Dart FFI + texture plumbing

Goal: the example app starts, calls into native code, gets a texture id back, and
shows a test pattern in a `Texture` widget with Flutter widgets on top.

- [ ] Create `packages/android_auto_linux/linux/src/` with the core skeleton
  - [ ] `aa_core.h`: the flat C ABI (opaque handle, create/destroy/start/stop, config struct, event callback)
  - [ ] `aa_core.cc`: lifecycle, owns the Boost `io_context` thread pool
  - [ ] `event_bus.{h,cc}`: native to Dart events, one queue drained on the platform thread
- [ ] Wire `AndroidAutoLinuxPlugin` (GTK entry point) to capture the `FlTextureRegistrar` into a process-global
- [ ] Implement `present/gl_adapter.{h,cc}`: an `FlTextureGL` subclass fed from a dmabuf ring
  - [ ] Create a `GdkGLContext` shared with Flutter's, per `fl_view` window
  - [ ] Define the API agnostic frame descriptor (`fd`, modifier, stride, offset, fourcc, size) that the seam is built on
  - [ ] Keep `FlTextureGL` confined to this one file, see the Vulkan readiness rules in `docs/architecture.md`
  - [ ] Render a moving test pattern to prove the texture pipeline end to end
- [ ] `ffigen` config + generated bindings in `packages/android_auto_linux/lib/src/bindings/`
- [ ] Dart side: `AndroidAutoLinux` registers itself as the platform implementation
- [ ] `NativeCallable.listener` based event stream from native to Dart
- [ ] Define the public API in `android_auto_platform_interface`
  - [ ] `AndroidAutoConfig` (resolution, fps, dpi, head unit identity, enabled services)
  - [ ] `AndroidAutoConnectionState` enum and event stream
  - [ ] `textureId` future
- [ ] Define `AndroidAutoView` widget in `android_auto` (Texture + `Listener` for touch)
- [ ] Example app renders the test pattern with a Flutter overlay on top
- [ ] `tools/run-example.sh` builds and launches the example, agent verifies by screenshot

**Checkpoint:** a screenshot showing the native test pattern with Flutter widgets composited over it.

---

## M3. USB transport, AOAP, SSL, service discovery

Goal: plug in a phone, get to the point where the phone has accepted our head unit
and announced its services.

- [ ] Install the udev rule for AOAP devices (`tools/99-android-auto.rules`), no root needed at runtime
- [ ] USB enumeration and hotplug via aasdk's `USBHub` / libusb hotplug
- [ ] AOAP accessory mode switch (control requests 51/52/53), wait for re-enumeration as `18d1:2d00`-`2d05`
- [ ] Open the AOAP bulk endpoints, wrap in aasdk `USBTransport`
- [ ] SSL handshake with the head unit certificate
  - [ ] Ship `headunit.crt` / `headunit.key` as plugin assets, allow the app to override the path
- [ ] Version request/response, then `ServiceDiscoveryRequest`
- [ ] Build the `ServiceDiscoveryResponse` describing our head unit (this is where we advertise video size, input, audio configs)
- [ ] Log the negotiated channel list to Dart as a structured event
- [ ] Surface connect/disconnect/error states through the Dart event stream
- [ ] Reconnect cleanly after unplug and replug, 10 times in a row without a leak

**Checkpoint:** logs show the phone's service discovery response, and Android Auto is
"active" on the phone screen.

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
