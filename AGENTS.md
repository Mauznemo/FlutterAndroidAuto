## Style

- Never use em dashes (—) or en dashes (–), anywhere: not in UI strings, dart docs, normal comments, or markdown. Use a comma, parentheses, or a separate sentence instead.
- When defining widgets or classes put all the vars on top of the constructor and not the other way around.
- Update this or other AGENTS.mds if the info in here changes or something new is worth adding if it will be needed for every later session. Do not clutter it with one of info or things that are just common sense or easy to figure out without having it here. If you think something one off needs explaining the dart docs with /// is the right place, not this file.

## What this project is

A Flutter plugin that runs an Android Auto head unit inside a Flutter app, rendering the
projected phone screen into a Flutter `Texture` so the host app can draw widgets on top.
Linux first, federated package layout so Android can be added later.

**Read `PLAN.md` first in every session.** It holds the milestones with checkboxes and is
the source of truth for what is done and what is next. Tick a box only when the thing is
verified on this machine, not when it merely compiles. Also update the status table at
the top of `PLAN.md` when a milestone changes state.

Background reading, only when relevant: `docs/architecture.md` (how the pieces fit),
`docs/echo-cancellation.md` (phone calls, which are not in this code), `docs/wireless.md`
(Android Auto without a cable), `docs/aasdk-port-notes.md` (what the vendored aasdk
needed), `docs/packaging.md` (how a build is linked and what a machine needs to run it),
`docs/releasing.md` (cutting a release and publishing to pub.dev),
`dev/dev-environment.md` (this machine). `docs/research.md` is a dated record of
what was known before the work started, not current guidance; read it for why a decision
was taken, never for what the code does now.

## Layout

| Path | What |
|---|---|
| `packages/android_auto` | app facing API, pure Dart, no GPL code |
| `packages/android_auto_platform_interface` | the contract, pure Dart, no GPL code |
| `packages/android_auto_linux` | Linux implementation, links aasdk, **GPL-3.0** |
| `example/` | the reference integration and test bench, run this to verify anything visually. `lib/head_unit/` is what a product would ship, `lib/bench/` the diagnostic panels |
| `tools/` | what a stranger needs: `setup-dev-machine.sh`, `build-aasdk.sh`, `port-aasdk.sh`, `install-echo-cancel.sh`, `wireless-ap.sh`, and `config/` for the PipeWire drop-in the fourth of those installs |
| `dev/` | this machine's own tooling, not part of the plugin: `ui.sh`, `run-example.sh`, `wireless-test.sh`, `wireless-capture.sh`, `fake-wireless-phone.py`, `audio-graph.sh`, `release.sh` with `release_support.py`, and `dev-environment.md` |
| `pubspec.yaml` | the workspace root, not a package. Its only content is the member list |

Keep aasdk code out of the two pure Dart packages. That split is what keeps a future
permissive implementation possible.

The `tools/` and `dev/` split is the same kind of line. `tools/` is for anyone who clones
this: provisioning, the aasdk build, the echo canceller, the access point. `dev/` is for
driving this particular machine, and assumes KDE on Wayland, a German keyboard and a
paired Pixel. Nothing in `tools/` may call into `dev/`.

## Driving the author's machine, which is not every machine

**Everything in this section is one laptop.** It is here because the agent needs it to
look at what it built, not because the project requires any of it. The machine is
described in full in `dev/dev-environment.md`; the short version is KDE Plasma on
Wayland, one 1920x1080 output at scale 1 so screenshot pixels equal screen coordinates,
a German keyboard, and passwordless sudo. X11 tools (xdotool, scrot, wmctrl) do not work
here. On any other machine, expect none of it to apply and do not teach the plugin about
it.

```bash
dev/ui.sh setup              # once per boot: starts ydotoold, flattens pointer accel
dev/ui.sh shot               # full screen png, prints the path, then Read it
dev/ui.sh crop <f> <x> <y> <w> <h>
dev/ui.sh click <x> <y>      # also: move, rclick, drag, scroll
dev/ui.sh key ctrl+s         # also: esc, enter, tab, f1..f12
dev/ui.sh paste "text"       # use this, not `type`, see below
dev/run-example.sh --bg      # build and launch the test bench detached
dev/run-example.sh --bundle  # run the built binary directly, line buffered logs
```

Four things that will bite otherwise, and these are rules rather than description:

- **`dev/ui.sh setup` is required after every reboot.** Without the flat pointer
  acceleration it sets on the ydotool virtual device, clicks land in the wrong place.
- **The keyboard layout is German (QWERTZ)** and ydotool sends raw keycodes, so
  `ui.sh type` mangles y/z and symbols. Use `ui.sh paste` for exact text.
- **Never run `pkill -f <pattern>`**, it matches the agent's own shell command line and
  kills the session. Resolve the pid first with `pgrep` into a variable built by
  concatenation, then `kill` it. `pgrep -x android_auto_example` never matches either:
  the name is over 15 characters. Match on `bundle/android_auto_ex""ample` instead.
- **Never leave two copies of the example app running.** They fight over the phone and
  the result looks exactly like a protocol bug: handshakes that half complete, reads that
  time out, a phone that goes silent. `dev/run-example.sh` kills the old one first, so
  use it rather than launching the bundle by hand.

## Build and run

The three packages and the example are one Dart workspace, so `flutter pub get` and
`flutter analyze` are run from the repository root and there is one `pubspec.lock`, at
the root, which is committed.

**`flutter test` on its own does not work from the root** and says "Test directory
"test" not found": the workspace root is not a package and has no tests of its own.
Name the members instead, which is what CI does:

```bash
flutter test packages/*/test
```

```bash
flutter pub get && cd example && flutter build linux --debug
dev/run-example.sh --bg
```

A clean rebuild is about twenty seconds with ccache installed and about three minutes
without, because `flutter clean` deletes the object tree and aasdk's 346 translation
units are almost all of it. CMake finds ccache on its own and says at configure time
which case it is in. `-DAA_USE_CCACHE=OFF` turns it off.

**Wipe a build with `flutter clean`, never `rm -rf example/build`.** The second leaves
`build/native_assets/linux` missing and the next build dies at the install step with
"file INSTALL cannot find", which looks like a project fault and is not.

`--bg` runs through `flutter run`, which block buffers the app's stdout: native logging
arrives in 8 KB lumps, minutes late, which is useless for watching a protocol exchange.
Use `--bundle` for that, which runs the built binary directly and line buffers.

Environment knobs, all off unless set. The five marked **debug only** are compiled out
unless `AA_ENABLE_FAULT_INJECTION` is on, which a Debug build does by default and a
Release build does not, so they will silently do nothing in a release build:

| Knob | What it does |
|---|---|
| `AA_LOG_LEVEL=DEBUG` | aasdk's own protocol log, the service discovery exchange in full, and libavcodec's diagnostics |
| `AA_SERVICES=video,input,sensor` | narrows or widens the advertised channel set without a rebuild. `all` for everything. The metadata channels are `navigation`, `media_status`, `phone_status`, `notification`, `browser` |
| `AA_VIDEO_DECODER=software` | forces the software decoder, to tell a driver problem from a decoder problem |
| `AA_FAULT_TRANSPORT_AFTER=20` | **debug only.** Kills the transport after N seconds without touching USB, to exercise the reconnect path on demand |
| `AA_FAULT_TRANSFER_AFTER=400` | **debug only.** Turns the Nth completed bulk IN into a transaction error whose resubmit is refused as a halted endpoint, to exercise the retry |
| `AA_FAULT_SLOW_START=3000` | **debug only.** Stalls N ms inside `ProtocolSession::Start`, between the messenger existing and the channels being handed it, so a stop pressed during it lands in the window that used to crash |
| `AA_TRANSPORTS=wireless` | narrows or widens the transports without a rebuild, `usb`, `wireless` or both. Wireless cannot be tested while the cable is in, because a wireless connection is never allowed to displace a connected session |
| `AA_WIRELESS_FAKE_PHONE=/tmp/aaw.sock` | **debug only.** Listens on a Unix socket and treats a connection to it as the RFCOMM socket BlueZ would have handed over, so the whole wireless path can run with no phone. Drive it with `dev/fake-wireless-phone.py` |
| `AA_WIRELESS_SSID`, `AA_WIRELESS_PASSPHRASE` | **debug only.** What to tell the phone to join, for a test that cannot stop to type into a text field. A real head unit gets these from its host app |
| `AA_AUTOSTART=1` | the example app presses its own Start button. Example app only, not the plugin |
| `AA_SIMULATE=1` | the example app registers `AndroidAutoSimulator` instead of the real head unit, the same pure Dart stand in macOS and Windows always get. Example app only |

Built against Flutter 3.47.4 stable, which on this machine is the snap at
`~/snap/flutter/common/flutter`. Nothing requires the snap; that is just where it is
here.

**Impeller is the only renderer on Linux now** and it runs its **OpenGLES** backend
(`Using the Impeller rendering backend (OpenGLESSDF)`). `--no-enable-impeller` is a
verified no-op, there is no Skia fallback to retreat to. Impeller's Vulkan backend for
Linux desktop is in progress upstream, and the project has to be ready for it, so the
video pipeline hands Flutter a **dmabuf**, never a raw GL texture. See
`docs/architecture.md`.

## macOS and Windows, which run a simulator

Most host apps are written on a Mac or a Windows machine, so `android_auto` registers
`AndroidAutoSimulator` there (inline `dartPluginClass` in its pubspec, no native code,
aasdk never built). It lives in `packages/android_auto/lib/src/simulator/`. Three rules
keep it honest:

- **It goes through the same seams as a phone.** The view still maps touches and calls
  `sendTouch` in projected video pixels; the simulator hit tests them against its own
  widget tree via `MetaData` targets rather than taking Flutter gestures. The one seam it
  added is `AndroidAutoPlatform.buildProjection`, which is a `Texture` everywhere else.
- **Its picture size is `video_margins.cc` ported to Dart** (`frameForView`,
  `visibleForView`). Change one and change the other, or a layout tried on a Mac is not
  the layout the car gets.
- **Nothing in it may need the engine to finish a future.** `Picture.toImage` never
  completes under `flutter test`, and `package:test` then waits on it forever, which hangs
  every host app test that connects the simulator. Cover art is encoded by hand in
  `simulated_art.dart` for that reason.

`release.yml` builds the example on macOS and Windows runners (the `desktop` job), which
is the only proof it compiles there. It is kept out of the per push CI on purpose, for
cost, and `dev/release.sh` gates on it along with the Linux builds.

## Native build

```bash
tools/build-aasdk.sh           # build aasdk alone and smoke test it. Must print OK.
tools/port-aasdk.sh reset      # throw away local aasdk edits
tools/port-aasdk.sh apply      # redo the port in the working tree, from the patch
tools/port-aasdk.sh regen      # redo it from the script's transforms, for a new pin
tools/port-aasdk.sh patch      # regenerate linux/patches/ from the working tree
```

**`apply` and `regen` are not the same thing and the difference has already cost work
once.** Part of the port is hand written rather than scripted: the USBEndpoint transfer
retry, the halt clearing, `AA_FAULT_TRANSFER_AFTER` and the video channel's
`UpdateUiConfig` send and reply live in the patch and in no function in the script. So `regen` produces *less* than the patch holds, and `patch` run
straight after it silently deletes the difference. `apply` applies the committed patch,
which is what "redo the port" means in every ordinary case. Reach for `regen` only when
the submodule pin has moved somewhere the patch will not apply, and expect to put the
hand written parts back by hand.

aasdk is vendored as a submodule at
`packages/android_auto_linux/linux/third_party/aasdk`, pinned to `9bf6adf`. **It does not
build unpatched**: Boost removed `io_service` in 1.87, and there are four other
breakages. The port is a single patch in `linux/patches/`, generated by
`tools/port-aasdk.sh`, and the submodule is never committed dirty.

**The plugin's CMake applies that patch itself**, at configure time, keyed on whether
`include/aasdk/Common/Strand.hpp` exists. So a clean clone builds with no manual step,
and `tools/build-aasdk.sh` is for proving the port on its own rather than for making the
build work. If you change the port, regenerate the patch, then `tools/port-aasdk.sh
reset` and rebuild to check CMake still applies it cleanly. Full write up in
`docs/aasdk-port-notes.md`.

Three things in the port are about packaging rather than about compiling, and undoing
any of them puts back a bug: the library type is taken from the parent project so the
plugin can link aasdk statically, Boost.Test is only asked for when aasdk's own tests
are being built (it was in every consumer's `DT_NEEDED` otherwise), and
`CMAKE_CXX_FLAGS_RELEASE` no longer forces `-g`, which was 23 MB of DWARF in a shipped
library. See `docs/packaging.md`.

The key piece: `aasdk::Strand` subclasses `boost::asio::strand<io_context::executor_type>`
to restore the old one argument `dispatch`/`post`, `get_io_service()` and an
`io_context&` flavoured `context()`. That is what keeps the port a type substitution
instead of a rewrite, so prefer extending it over touching call sites.

## Releasing

`dev/release.sh` cuts a release: it bumps the three packages in lockstep, generates a
changelog per package from the `feat`, `fix` and `refactor` commits that touched it,
builds on CI (Linux on both architectures, macOS, Windows), and only then publishes to
pub.dev in dependency order and opens a GitHub release. Never run it unless asked to. `--dry-run` does every check
and pushes nothing.

`release.yml` runs on `workflow_dispatch` only. It used to run on `release: published`,
which was backwards: the release is created after publishing, so the build could only
report on a release that had already gone out. That is how 0.1.0 shipped unbuildable on
clang 18. The script dispatches it between pushing and publishing instead, and accepts
a run that has already passed for that commit without rebuilding.

The one rule worth carrying into any work on the pubspecs: **the cross package
dependencies are caret constraints, not paths**, because pub will not publish a path
dependency. They resolve to the sibling directories anyway, because a Dart workspace
prefers its own members, but only while the local version satisfies the constraint. So
versions and constraints move together or `flutter pub get` silently starts resolving
against pub.dev. Full write up in `docs/releasing.md`.

## Committing and pull requests
Do NOT git commit unless you are told to do so!
Once toled to, for commit message and PR style: read CONTRIBUTING.md before writing either.

## Native layout

| File | What |
|---|---|
| `linux/src/aa_core.h` | the flat C ABI, the only thing Dart binds to |
| `linux/src/aa_core.cc` | session lifecycle, owns the `io_context` thread pool |
| `linux/src/frame_ring.*` | the API agnostic seam, three slots, producer to raster thread |
| `linux/src/present/gl_adapter.*` | **the only file allowed to name a GL type** |
| `linux/src/present/texture_registry.*` | the `FlTextureRegistrar` the GTK entry point captured |
| `linux/src/event_bus.*` | native to Dart events |
| `linux/src/video/video_decoder.*` | H.264 to frames on its own thread, VA-API or software |
| `linux/src/video/video_margins.*` | the frame size and the margins that fit it to the view, **no protobuf** |
| `linux/src/session/video_channel.*` | the MEDIA_SINK_VIDEO channel |
| `linux/src/audio/pcm_sink.*` | the API agnostic seam for playback, **PulseAudio is named only in pulse_sink.cc** |
| `linux/src/audio/pcm_source.*` | the same seam for capture, **PulseAudio is named only in pulse_source.cc** |
| `linux/src/audio/audio_output.*` | three streams, a writer thread each, volume, mute and ducking |
| `linux/src/audio/audio_input.*` | the microphone, one capture thread, created on the phone's request and joined on its release |
| `linux/src/session/audio_channels.*` | the three MEDIA_SINK audio channels |
| `linux/src/session/microphone_channel.*` | the MEDIA_SOURCE_MICROPHONE channel |
| `linux/src/sensors/sensor_state.*` | what the head unit believes about the car, **no protobuf**, process lifetime |
| `linux/src/session/sensor_channel.*` | the SENSOR channel, the only thing that turns those into wire messages |
| `linux/src/metadata/metadata_state.*` | what the phone has said about itself, **no protobuf**, cleared when the connection ends |
| `linux/src/metadata/json.*` | the small JSON writer the metadata ABI carries its updates in |
| `linux/src/session/metadata_channel.*` | a channel aasdk names but does not speak: open response plus a decoder hook |
| `linux/src/session/metadata_channels.*` | the five metadata decoders, the only thing that turns wire messages into state |
| `linux/src/bluetooth/bluez_client.*` | **the only file that names a D-Bus type**, the RFCOMM service and the paired device list |
| `linux/src/wireless/wifi_network.*` | which SSID, BSSID and address to tell the phone, read off the interface |
| `linux/src/wireless/aaw_handshake.*` | the Bluetooth conversation that precedes wireless projection, **the only file that names the `aaw` protobufs** |
| `linux/src/session/wireless_connector.*` | Bluetooth, then the handshake, then the acceptor on 5288, ending at a transport |
| `linux/src/test_pattern.*` | drives the texture without a phone, for overlay layout |
| `linux/android_auto_linux_plugin.map` | the linker version script: the `aa_*` ABI and the GTK registrar are exported, everything aasdk and protobuf drag in is not |

After changing `aa_core.h`, regenerate the Dart bindings:

```bash
cd packages/android_auto_linux && dart run ffigen --config ffigen.yaml
```

`AaState` in `aa_core.h` and `AndroidAutoConnectionState` in the platform interface cross
FFI as plain integers, so their orders must stay in step.

Only the raster thread touches GL, inside `populate()`, where Flutter's context is
already current. That stayed true once video landed: the dmabuf is imported as an
`EGLImage` and converted to RGBA by a shader inside `populate()`, so there is still no
second GL context and no cross-context fence to get right.

Two rules that cost real time in the video work:

- **Every `glBindTexture` lands on whichever texture unit is active.** The NV12 converter
  binds luma on unit 0, chroma on unit 1 and the output texture on unit 2, in that order.
  Binding the output while unit 1 was current replaced the chroma plane with the previous
  frame: a perfect picture in entirely the wrong colours.
- `populate()` runs inside Flutter's own rasterisation, so `GlStateGuard` saves and
  restores everything the converter touches. Impeller re-binds most of what it uses, but
  "most" is not a contract.

## What Android Auto demands before it will project

Learned the hard way, and none of it reports an error. A phone that dislikes the
service discovery response just stops talking and drops out of accessory mode.

- **Advertise every channel, not only the implemented ones.** A head unit offering video,
  input and sensors is one Android Auto refuses to project to. The three audio sinks and
  the microphone have to be there too. The older rule holds on top of this: an advertised
  channel that is never serviced gets the connection dropped.
- **The sensor channel is load bearing.** The phone locks most of its interface until the
  head unit answers the driving status subscription.
- **Fill in the fields the schema calls optional.** `vehicle_id` and `driver_position`
  were `required` in the schema openauto was built against and phones still validate
  against that shape. Do not set `session_configuration` at all: an explicit zero is not
  the same as an absent field on the wire.
- **Android Auto's H.264 is Baseline profile, which no GPU decodes.** The decoder sets
  `constraint_set1_flag` in the SPS to make it Constrained Baseline. Without that, VA-API
  setup fails and *every* packet afterwards returns `AVERROR_INVALIDDATA`, which reads
  exactly like corrupt video rather than a driver limitation.
- A libavcodec `get_format` callback must never return `AV_PIX_FMT_NONE` when the
  hardware format is gone. That leaves the decoder with no output format at all. Fall
  back to whatever is offered.

## Input, the only channel that flows outwards

Everything else is the phone pushing and the head unit answering. Input is the head unit
talking unprompted, and that makes three things different.

- **Coordinates are projected video pixels.** Not logical pixels, not normalised, and
  measured from the corner of the texture, which is the frame less its margins (see
  the next section). `AndroidAutoView` maps widget-local positions through the same
  `applyBoxFit` arithmetic `FittedBox` paints with, so letterboxing stays consistent
  between what is drawn and where taps land.
- **Touch follows Android's `MotionEvent` rules**, because that is what the phone's input
  stack expects: first finger `ACTION_DOWN`, extra fingers `ACTION_POINTER_DOWN` with
  `action_index` naming the one that changed, last finger `ACTION_UP`. Flutter's
  ever-growing pointer ids are remapped to small reused slots.
- **Nothing acknowledges an input report.** A report sent before the phone has opened the
  channel vanishes without a trace, so the send path refuses rather than sending into the
  void.

`InputChannel` is also the only channel called from Flutter's platform thread while an io
thread can be tearing it down. Hence `std::atomic` flags, a mutex around `channel_`, and
a `Channel()` helper that hands every caller its own reference. `VideoChannel` and
`SensorChannel` had the same shape of race, for a different pair of threads, and now
have the same treatment; see the lifetime rules below.

**Movement is rate limited to one report per 16 ms.** Without it the head unit emits one
USB bulk write per Flutter pointer event, measured at 382 a second on a desktop mouse.
What comes back is a video stream of at most 60 fps, so extra samples cannot produce a
distinguishable picture. Only movement is limited: a finger landing or lifting is an edge,
not a sample, and has to go at once.

## Margins, and why the texture is the view's size

The protocol only names 16:9 frame sizes. `AndroidAutoView` reports its size in physical
pixels, the phone is told at service discovery to leave `width_margin` and
`height_margin` clear, and the present adapter crops them off. **When the view fits in
the frame the picture is the view's exact size, not merely its shape**, so the phone lays
out a screen that big at its usual density and it is drawn one to one. Matching only
the shape and shrinking a larger picture looked grainy after a big window was made
small: every glyph the phone drew ended up at two thirds of its size. Only a view larger
than the frame gets a picture of its shape that is then stretched. All of the following
was measured on the Pixel 8 Pro, none of it is in the schema, and any of it could be
different on another phone:

- **The picture is centred** in the frame, each total split evenly between two sides.
  Totals are kept even so the split is whole; a side may be odd, which is fine because
  the converter samples chroma at its true position.
- **Touch is relative to the picture's corner and unscaled.** Tapping at frame
  coordinates missed by exactly the top margin. Nothing adds the margins back.
- **The touchscreen stays announced at the full frame**, not at the picture size. Both
  worked with fixed margins, but only the full frame still holds a picture that grows
  after a mid session change.
- **A mid session change is focus native, then `UpdateUiConfigRequest`, then focus
  projected**, in `VideoChannel::Resize`. Never send the update on its own: the phone
  re-lays out its launcher, leaves the app in front drawn for the old margins, and the
  stream freezes until something else on screen changes. The focus release makes the
  phone stop and restart the stream, which is also what makes the crop frame exact.
  The reply echoes the per side margins the phone took, and those are what is cropped.
- **A narrow picture changes the phone's layout**, not only its size: at 832x720 it
  moves its app rail from the left side to the bottom.
- The one third floor on the visible size in `video_margins.cc` is a guard against
  transient layouts, not a limit anything has been seen to enforce.
- **Every change of view size relays out the phone**, not only a change of shape, since
  the picture follows the size. Each one freezes the stream for about a second, after
  the 400 ms the view has to hold still.

**A texture drawn smaller than the video is shrunk in `gl_adapter`, not by Flutter.**
Flutter samples an external texture with one bilinear read per screen pixel whatever
the scale, which skips source pixels and turns small text and the phone's compression
noise into grain. `Texture.filterQuality` does not help: Impeller only adds mipmap
filtering, and an external texture has no mipmaps. So `AndroidAutoView` reports the
size it draws at in physical pixels (`aa_session_set_display_size`), and the converter
renders straight to that size, averaging every source pixel. The taps are spread over
the footprint *less one pixel*, since each is already bilinear; spread over the whole
footprint they blurred text at scales just under one. And `AndroidAutoView` places the
texture on whole physical pixels and draws it at exactly its own size when within two
pixels of it: a texture at a half pixel position is resampled even at one to one. Measured at 0.67x: a
quarter less high frequency noise and visibly smooth text. A texture drawn *larger*
than the video cannot be helped here; that takes a bigger frame from the phone.

**So the frame size follows the view unless the host app names one.** With
`AndroidAutoConfig.width` and `height` left null (0 in `AaConfig`), `FrameSizeForView`
picks, per connection, the smallest of 800x480, 1280x720 and 1920x1080 whose visible
picture covers the view's *physical* pixels to within five percent, which is why the
view reports physical rather than logical pixels. It is settled in `AaSession::Describe`
and cannot change mid session: a view made larger stays stretched until the phone
reconnects, while a smaller one is handled by the shrink above. That is why
`AndroidAutoController.start` reports the window's size when no view has reported one:
a head unit that shows the view only once a phone connects otherwise got 1280x720 on
any screen for the whole connection. 1440p and 2160p are
left to a named size, being untried and reportedly H.265 on the phone's side.

## Audio, and why the head unit is the thing that mixes

Android Auto sends media, system and speech as **three separate PCM streams** and leaves
the mixing to the head unit. The phone cannot duck its own music under its own
navigation prompt, because by the time the two exist they are already on different
channels. Whoever mixes is whoever ducks, and that is this code.

- **Media drops to 25% while the speech stream carries audio**, and for 500 ms after so
  the pauses inside one instruction do not make it flutter. Ramped over 20 ms, never
  stepped: a step on music is an audible click. `[Audio] media ducked under speech` is
  logged once per transition, which is the only way to answer "did it duck" without
  recording the speakers.
- **Ducking is not driven by the audio focus request.** A phone asks for
  `GAIN_TRANSIENT_MAY_DUCK` before its own media too, so that would duck media against
  itself. The focus exchange is answered properly all the same, in
  `ProtocolSession::onAudioFocusRequest`: a transient request gets `GAIN_TRANSIENT`, not
  `GAIN`.
- **Every method on `PcmSink` blocks and none is thread safe.** A sink belongs to one
  writer thread, which is never an io_context thread: `Write` waits for the server to
  take the samples, which is what paces the head unit to real time, and waiting on an io
  thread stalls the USB transport.
- **A buffer is acknowledged when it is queued, not when it is heard.** The phone's ten
  unacked buffers are the flow control, the queue here is bounded at one second and
  drops oldest first, and the pacing is the writer waiting on the speakers.
- **Volume and mute are applied in software**, on the int16 samples, so they work on any
  backend and mute is exactly zero rather than nearly zero. A muted stream is written as
  silence rather than not written, so the stream clock keeps running.
- **An underrun only counts once the buffer has been full on that open.** A stream the
  phone feeds in real time (speech is exactly that) never gets ahead of the speakers and
  sits at 10 ms buffered from first sample to last. Counting those made every navigation
  prompt an underrun. See `Track::primed`.
- `EndStream` **drains rather than flushes**. The phone ends a spoken instruction with
  the stop indication, so discarding what is queued clips the last syllable off every
  prompt. The sink is left open across the gap, because guidance comes every few seconds
  and reopening costs a fresh prebuffer.

**Measuring any of this means recording, not listening.** The recipes, and the two traps
that make a measurement wrong rather than absent, are in `docs/echo-cancellation.md`
under "Verifying it on other hardware".

## The microphone, and the one rule it has to keep

Android Auto asks for the head unit's microphone when the Assistant is invoked, gets
16 kHz mono 16 bit PCM back, and closes it again. `MicrophoneChannel` answers the
protocol and `AudioInput` owns the capture thread.

- **Nothing captures unless the phone asks.** The device is opened on the microphone open
  request and closed on the close, and there is no other path: no Dart call starts
  recording, and the capture thread is created on `Start` and joined on `Stop`, so "is
  this machine listening" and "does that thread exist" are the same question. Check it
  from outside the app with `pactl list short source-outputs`: empty unless the Assistant
  is listening, and empty again immediately after a stop even if the stop lands mid
  sentence. Do not add a way to start it from the host app.
- **The phone asks for a window of two unacked buffers and then never acknowledges one.**
  Zero acks across five sessions and seven hundred buffers. A head unit that gated its
  sends on acknowledgements would stall after two buffers, so the backpressure is counted
  against the send instead, from posting to the messenger having written it. The phone's
  number is honoured once it has acknowledged anything, see `MicrophoneChannel::Window`.
- **Buffers are 32 ms, produced on the capture thread, posted onto the channel strand.**
  The same shape as an input report, and for the same reason: everything that touches the
  channel happens on one thread.
- **Echo and noise cancellation are not implemented in this code, but they are no longer
  absent.** The phone asks for both in `anc_enabled` and `ec_enabled`; they are hints
  about what the head unit has already done, so nothing is owed on the wire, and they are
  still only logged at debug. What changed is that the echo canceller installed for phone
  calls becomes the default capture device, and this channel follows the default, so the
  Assistant now gets cancellation and noise suppression for free. That matters: in a car
  with music playing it is what makes recognition work at all. See
  `docs/echo-cancellation.md`.
- **"Hey Google" is not on this channel.** The hotword works, but the phone listens for
  it on its own microphone and only then asks the head unit for one, for the query that
  follows. Nothing here can make the hotword more reliable. Synthetic speech does not
  trigger it either, because Google's hotword stage does speaker verification.

It can be exercised without a person in the room, through whatever speakers and
microphone the machine has. **Do not fake a microphone with `module-null-sink`**: it
broke recording machine wide here, in a way that looks exactly like a bug in the capture
code. That and the speech synthesis recipe are in `docs/echo-cancellation.md`.

## Sensors, and why advertising one is a promise

The head unit tells the phone what the car is doing. Nothing here reads any hardware:
this plugin owns no GPS and no parking brake, the host app is the thing running in the
vehicle, and `SensorState` is where it says so. `SensorChannel` is the only file that
turns those values into protobuf.

- **Advertising a sensor is a declaration that the car has it, not a feature switch.**
  The phone subscribes to what is offered and then waits. For location it is worse than
  waiting: it **stops using its own receiver** the moment the head unit claims a
  position, so advertising it without feeding it takes navigation away from a phone that
  was managing perfectly well. The advertised set is `AndroidAutoConfig.sensors`, read
  once at service discovery, defaulting to night mode and driving status. A subscription
  to anything outside it is answered `STATUS_INVALID_SENSOR` rather than accepted.
- **A sensor the host app has never set is not sent at all.** Not sent as zero: the
  difference between a car with no fix yet and a car in the Atlantic. Same inside a
  reading, where an unknown altitude is an absent field rather than sea level. Dart
  spells it `null`, the C ABI `NaN`.
- **Night mode and driving status are the two with defaults**, day and unrestricted,
  because the phone will not finish opening its interface without an answer to them.
- **`SensorState` is process lifetime**, like the decoder and the audio output, for a
  plainer reason: a parking brake does not come off because a cable was pulled out. The
  Dart side keeps write closures rather than values, so replaying into a new core is the
  same code path as the original set.
- **`min_update_period` is a number nothing here can read.** This phone sends 0 for most
  sensors and 3 for speed and compass, of an unstated unit. The code reads it as
  microseconds, which is the choice under which the limiter never fires wrongly, and the
  rate limiter in `SensorChannel::Offer` has never fired against a real phone. It holds
  the newest value rather than dropping it and caps any hold back at five seconds.
- **The phone can be pinned to night**, in Einstellungen, Display, Tag-/Nachtmodus für
  Karten. Only *Automatisch* reads the head unit's sensor. A phone on *Nacht* makes a
  working night mode sensor look like a no-op in both directions, so check that before
  touching code.
- The one phone tested, a Pixel 8 Pro, subscribed to eight of the twelve when all were
  advertised: driving status, night mode, speed, gear, parking brake, location, toll
  card, compass. Not rpm, fuel, environment or odometer. Another phone may well choose
  differently, so treat this as one observation rather than the protocol.

## Phone calls, which are not in this code at all

Android Auto does not carry call audio over the projection link. Calls go over Bluetooth
HFP, with this machine as the hands free unit and the phone as the audio gateway. Once
the phone is paired by any means, both directions are bridged by WirePlumber on its own:
the far end arrives as a playback stream following the default sink, and the uplink is a
capture stream following the default source. Verified end to end on 2026-09-16, on the
LC3-SWB codec. Do not go looking for a telephony channel to implement.

- **The aasdk Bluetooth channel is not needed for this.** Advertising `car_address` over
  the projection link turned out not to be required: ordinary out of band pairing was
  enough for Android Auto to stop reporting no Bluetooth and to route a call through the
  machine. That channel may still earn its place for a wireless handover, which has no
  other way to hand the phone the head unit's details.
- **Echo cancellation is configuration, not code, and it is not optional.** Without it
  every call sends the far end back to itself a few hundred milliseconds late, because
  the microphone hears the speakers. The phone will not do it once the call is on a hands
  free unit. `tools/install-echo-cancel.sh`, reasoning in `docs/echo-cancellation.md`.
- **Both the call uplink and the microphone channel follow the default source.** That is
  why the canceller's virtual source is given a priority that makes it the default,
  rather than anything being wired to it by name. It is also why nothing in this
  repository has to know a device name for calls to work.
- **An empty device name means the head unit's own hardware**, not the audio server's
  current default, and a Bluetooth device is never resolved to. A phone that connects
  must not be able to become the car's speakers or the car's microphone. See
  `DefaultHeadUnitDevice` in `pulse_sink.cc`. On this machine WirePlumber never actually
  moved the default, so that rule is defensive rather than a fix for an observed fault.
- `dev/audio-graph.sh` dumps the audio graph, and `--watch` records it over time. The
  Bluetooth nodes only exist while a call is up, so a snapshot taken between calls shows
  nothing and proves nothing.

## Stopping and resuming a session

The order matters and it is not obvious.

1. `ByeByeRequest`, then **wait for the acknowledgement**. The phone keeps Android Auto
   running, and its claim on the USB interface, until it answers.
2. Then `libusb_reset_device`. After a ByeBye the phone stays in accessory mode and will
   not answer a fresh version request; only redoing the AOAP handshake re-arms it, and
   that needs it out of accessory mode first.

Skip step 1 and the phone is wedged. Skip step 2 and every other reconnect times out.

A run that dies before step 1 leaves the phone wedged for the next launch, so a session
that errors before ever reaching connected bounces the phone and retries, up to three
times.

**A stop can arrive while the session is still being built.** `Start()` runs on an io
thread, when the connector hands over a phone that reached accessory mode; `Stop()` and
`Shutdown()` arrive on Flutter's platform thread. `lifecycle_mutex_` serialises them, so
a stop pressed a moment after a start waits for the connection to exist and then says
goodbye on it. Without that, the stop reset `messenger_` between two of the lines in
`Start()` that hand it to a channel, and the channel dereferenced a null messenger on its
first `receive()`: a segfault that took the whole app down, found by hand and then made
reproducible on demand with `AA_FAULT_SLOW_START`.

**Nothing a stopping session says is news.** From the moment `Shutdown()` is asked for,
`ReportState` drops error and connected reports. The channels fail one after another as
the link comes down, and `aa_core.cc` read those as a phone gone wrong: it bounced the
device a second time in the middle of the user's stop, which is what left the next start
unable to enumerate the phone for tens of seconds. The goodbye itself still goes out, and
so does every other state.

Losing the cable mid session is a separate path: it reports `searching`, not `error`, and
resumes on its own. That needs discovery to be re-armed after **every** handover, because
`USBHub::handleDevice` ignores arrivals while its promise is null.

**Reading aasdk USB errors:** `USB_TRANSFER`'s "Native Code" is a
`libusb_transfer_status`, not a `libusb_error`. 2 is TIMED_OUT, 4 is STALL. The patched
`USBEndpoint` now prints the name, the endpoint and the byte counts, so this no longer
has to be decoded by hand.

## USB transaction errors, and what is and is not proven about them

The dropouts are `-EPROTO` transaction errors on the bulk endpoints, confirmed with
usbmon. They are a physical layer fault: they hit the **ADB interface as well**, which
this code never opens, and twice within 14 ms of each other on both. No amount of
software causes a transaction error on an endpoint it does not use.

Two things follow, and only the first is settled.

**A zero length failure loses nothing, so it is retried.** USB's data toggle means a
transaction that moved no bytes leaves the device holding its packet, in either
direction, so `USBEndpoint` resubmits rather than failing upwards, five consecutive times
before giving up. A **partial** transfer is different: part of a frame is gone and the
framing with it, so that one still fails. Watch for `Transaction error on endpoint 0x81,
resubmitting`.

**A transaction error can halt the endpoint,** and then the resubmit is refused outright.
That is what happened the first time this ran in the wild: the retry fired once, the
resubmit was rejected, and the session died anyway looking exactly as if the retry had
never run. `libusb_clear_halt` then resubmits. This is heavier than a plain resubmit,
because clearing a halt resets the data toggle at both ends and a packet the device was
holding can be dropped, but the alternative is a full reconnect which loses that anyway.

What is **not** proven is the end to end outcome. Verified with `AA_FAULT_TRANSFER_AFTER`:
the retry fires, the halt clears, the resubmit succeeds, nothing crashes. Not verified:
that a real fault is absorbed invisibly, because injection cannot be faithful. It discards
a transfer that really did arrive, so the stream breaks afterwards with `SSL_READ` (error
26) whatever the recovery does; a real fault does not discard anything. Confirming that
needs one real transaction error, and they come and go: every few minutes one evening,
then not once in 46 minutes the next morning.

## A dead transport does not mean the phone went away

The case that cost an evening, and the reason `searching` could hang forever.

A failed bulk transfer (`LIBUSB_TRANSFER_ERROR` on a marginal link) kills the transport
**while leaving the phone enumerated and still in accessory mode**. The original
recovery waits for `USBHub` to hand it a device, and the hub only fires on arrival, so
nothing ever came: the device had never left. Stop then Start was the only way out,
because stopping calls `ResetDevice()` and that is what makes the phone re-enumerate.

So a connected session that loses its transport **bounces the phone itself** rather than
waiting to be told about it. Right for both cases: if the cable really is out, the reset
fails harmlessly on a device that has already gone, and re-arming discovery is what the
replug needed anyway. Measured at 4.6 seconds from dead transport back to projecting.

One dead transport is one event however many channels notice it. Every channel with a
receive outstanding can, within a millisecond of the others, and they report their
failures as messages on the **connected** state, which puts `reached_connected` back up
in between. The recovery is debounced with a `recovering`
flag; do not remove it or one failure bounces the phone once per channel.

## aasdk object lifetimes, the thing that keeps biting

aasdk was written for openauto, which builds everything once and exits the process when
the phone disconnects. Nothing in it survives being torn down and rebuilt in place: its
objects hold raw pointers and references to things the caller owns, and they outlive
them in ways no ordering fixes. Seven crashes came out of this, every one of them a
variation on the same theme.

The rules that came out of it, do not undo them:

- **libusb, the `UsbConnector` and the channel strand are process lifetime.** Created
  once, never destroyed. `src/session/usb_context.h` explains why in full.
- **Never pass `shared_from_this()` to an aasdk channel as an event handler.** It makes
  a reference cycle through the channel's promises, and the session then never dies, so
  the USB interface is never released and every reconnect fails with `LIBUSB_ERROR_BUSY`.
  Use `ControlEventRelay`, which holds a weak reference.
- **Never capture a raw `this` in a promise handler.** Rejections arrive on io_context
  threads long after the object is gone. Capture `weak_from_this()` and lock.
- **Anything that outlives a session must not hold a pointer back into it.** The
  connector is process lifetime, so `aa_session_destroy` calls `ClearHandlers()` first.
- **No channel class uses its channel member in place.** Every handler runs on an io
  thread, but `Stop()` is reached from Flutter's platform thread too, through
  `aa_session_stop`, so a frame can be halfway through being acknowledged while the
  channel is being dropped. `VideoChannel::Channel()`, `AudioChannels::Get()`,
  `MicrophoneChannel::Channel()`, `SensorChannel::Channel()` and
  `MetadataChannels::Get()` copy the pointer out
  under a mutex and the caller works from that copy, so a teardown mid handler drops the
  channel when the last reference
  goes rather than out from under whoever is using it. The flags beside them
  (`stopped_`, `streaming_`, `session_id_`) are `std::atomic` for the same reason.

When something crashes in a destructor or inside libusb's event thread, it is almost
always one of these rather than a new problem.

## Metadata, the five channels the phone talks on

The point of the project: a head unit that draws its own turn card and now playing bar
rather than only mirroring pixels. `MetadataState` is the seam, `metadata_channels.cc` is
the only file that turns wire messages into it, and `metadata/json.cc` is the only thing
that turns it into something the C ABI can carry.

- **aasdk names these channels but does not speak them.** Two of the five parse only the
  message ids openauto's phones sent, both deprecated in the current schema; three parse
  nothing past the channel open. So `MetadataChannel` here subclasses aasdk's public
  `Channel` base, answers the open and hands everything else to a decoder. Extending the
  submodule instead would have meant carrying five classes in the patch for messages
  nothing else consumes.
- **The one phone tested opened three of the five.** Navigation, playback and
  telephony, which are the three it pushes unprompted. That Pixel 8 Pro never opened the
  generic notification or the media browser, the two where the head unit has to speak
  first, so both of those are implemented and **unverified against any phone**.
  `AndroidAutoConfig.metadata` defaults to the three.
- **A channel that exists is not a channel that is open.** It exists from the moment it
  is advertised. Anything sending unprompted has to go through
  `MetadataChannels::GetOpen()`, or the request vanishes exactly as an early input report
  does. The same rule the input channel already had.
- **Updates are merged, not replaced.** The protocol splits one picture across several
  messages: the shape of a turn and the distance to it, a track's title and whether it is
  playing. Two deliberate exceptions, both in `metadata_channels.cc`: a `PhoneStatus`
  carries the whole call list every time so it replaces, and a navigation status that is
  not active or rerouting clears the turn fields, because an instruction left on screen
  after guidance ends misleads a driver.
- **Service discovery asks for the ENUM instrument cluster type, not IMAGE.** IMAGE makes
  the phone render each arrow and send a picture; ENUM makes it name the maneuver and
  leave the drawing to Flutter. It also decides which messages arrive: ENUM brings the
  navigation state and current position, and the deprecated turn and distance events
  carry neither lanes nor a destination. Both are decoded and
  `ManeuverFromTurnEvent` folds the older vocabulary into the newer one, so the API has
  one list of maneuver names rather than two.
- **One JSON object per update, pictures base64 inline.** Nothing else in the ABI has a
  shape like a lane diagram or a call list. Images are encoded once where they arrive,
  not on every update that carries them along, or a playback position ticking once a
  second would re-encode the cover art each time.
- **Unlike the sensors, none of this outlives the connection.** A parking brake does not
  come off because a cable was pulled out; the music does stop. `MetadataChannels::Stop`
  clears the state and `AndroidAutoLinux.stop` clears the Dart snapshots.

## Wireless, and the one thing it must not disturb

Android Auto without a cable is three stages, and only the middle one is new code: the
head unit publishes a Bluetooth RFCOMM service, tells the phone over it which Wi-Fi
network to be on and which address to dial, and the phone connects to port 5288. Above
that TCP connection everything is byte for byte the USB path, which is why the wireless
code ends at producing an `aasdk::transport::ITransport` and hands it to the same
`ProtocolSession`. Full write up in `docs/wireless.md`.

- **The head unit listens on both legs.** The phone advertises no Android Auto UUID of
  its own, so the head unit is the RFCOMM server; and it fills in the `ip_address` in
  `WifiStartRequest`, so it is the TCP server too.
- **Nothing here changes how the machine presents itself on Bluetooth**, and that is a
  rule rather than an accident. The machine this runs on has its own software pairing
  the phone for music and hands free calling, and that software is part of what makes a
  phone treat the machine as a car at all. One UUID is added to the adapter's service
  record; pairing, the agent, the adapter class, the alias and discoverability are
  untouched, and `AutoConnect` is deliberately off because it would make BlueZ reach
  out on a link other software owns.
- **Two things decide whether a phone will ever ask, and both fail silently.** It reads
  the service list at pairing time, so one paired before the service existed never
  asks; and BlueZ publishes a record with no RFCOMM entry at all unless
  `RegisterProfile` is given a `Channel`, which a phone reads and then disconnects
  from without a word at either end. That is why every working implementation
  hardcodes a channel number.
- **Only the passphrase has to be configured.** SSID, BSSID and address come off the
  machine. Bringing a network *up* is not this plugin's business, the same call
  `docs/echo-cancellation.md` makes about the echo canceller.
- **The wireless extensions cannot describe an access point.** `SIOCGIWESSID` and
  `SIOCGIWAP` answer `EINVAL` for an AP mode interface while `SIOCGIWMODE` works, so a
  hosting head unit knows it is hosting and reads an empty BSSID in the same breath.
  An empty BSSID is silently fatal, so when hosting it comes from `SIOCGIFHWADDR`.
- **`WifiStartRequest` is an instruction to connect, so send it once and only when the
  phone says it is on the network.** Sent early it is answered and forgotten and the
  phone never dials in; sent to a phone that is already projecting it tears the session
  down to obey, which looks like the splash screen appearing and vanishing in a loop.
- **A phone reporting `STATUS_WIFI_INCORRECT_CREDENTIALS` is rarely complaining about
  the passphrase.** It is its only word for an association that failed, and for an
  offer it rejected outright. The head unit logs the whole offer on every start for
  that reason; `journalctl -t wpa_supplicant` showing no association attempt at all
  means the offer was rejected rather than the join. The usual cause when it did try is
  an access point that is not WPA2 personal after all.
- **An access point needs a channel the kernel will beacon on**, a narrower set than
  the card supports: `no IR` channels cannot be used and asking NetworkManager for one
  blocks for ninety seconds in silence. Watch two traps in `iw phy`: `(disabled)`
  channels do not say `no IR`, and 6 GHz frequencies start at 5955 MHz.
- **Saying nothing is worse than saying no.** A phone that knows this machine asks for
  the service every five seconds for as long as Bluetooth is connected, forever, and
  shows the driver a "connecting" notice meanwhile. Withdrawing the service stops the
  answering, not the asking. So a head unit that is not projecting publishes the
  service and refuses, and the phone gives up: zero queries over a hundred seconds
  against one every 5.1 seconds. Never published at all when wireless is left out of
  `AaConfig::transports`, which is the setting for Bluetooth music and nothing else.
- **The service goes up when the controller is built, not when the session starts**, so
  an application that is open and not projecting answers phones instead of ignoring
  them. Starting stays an explicit act for wireless exactly as for the cable.
- **Refusing works well enough to need undoing.** A refused phone stops asking and does
  not notice when the answer changes, so `WirelessConnector` drops and remakes the
  Bluetooth link eight seconds after it starts offering, if nothing has asked by then.
  `ConnectProfile` on the Android Auto UUID is not an alternative: the phone asks for
  that service rather than offering it, so BlueZ answers "No more profiles to connect
  to".
- **A wireless connection never displaces a connected session, and a cable may.** A
  person plugging in a cable did something on purpose; a phone dialling in did not.
- **Wireless progress is news, not a lifecycle change**, the same rule the decoder
  follows. Reporting it as `searching` while a session was connected dragged every
  later report down with it, video statistics included.
- **A phone hosting a hotspot cannot join the head unit**, so wireless and a
  development machine whose only internet is that hotspot are mutually exclusive.
- **`dev/wireless-test.sh` drives a real attempt and names the stage it reached;
  `dev/wireless-capture.sh` records Bluetooth and Wi-Fi while it happens.** The
  second earns its place because nothing above the transport can tell a phone that
  never asked from one that asked and walked away.

## Native notes that keep coming back

- `android_auto_linux` must keep `pluginClass` in its pubspec even though it is mostly
  FFI. The GTK registration entry point is the only way to get the `FlTextureRegistrar`.
- The `io_context` thread pool must never run on Flutter's platform thread, and nothing
  outside the raster thread may call into Flutter's GL context without making the shared
  context current first.
- Texture *registration* must happen on the platform thread (so, from an FFI entry
  point). Only `mark_texture_frame_available` is safe from a producer thread.
- **The texture outlives every connection, so its id never means there is a picture.**
  `VideoDecoder::live()` does, and it is `hasVideo` in Dart and what `AndroidAutoView`
  shows its placeholder by. Anything new that ends a picture goes through the decoder's
  `Flush` or `Stop`, never straight to the ring, or the view goes on drawing a frozen
  frame of a phone that has gone. See "When there is a picture" in
  `docs/architecture.md`.
- Flutter's `apply_standard_settings` pins C++14, but aasdk headers need C++17, so the
  plugin target raises it afterwards. Do not remove that line.
- Clang on this machine targets the newest installed GCC. Without a matching
  `libstdc++-N-dev` every C++ compile dies with `'limits' file not found`, which looks
  like a project bug and is not. `tools/setup-dev-machine.sh --build-deps` installs it.
- The video path needs `libavcodec`, `libavutil` and `libswscale` (no libavformat, there
  is no container to demux). `drm_fourcc.h` comes from the kernel headers, not libdrm, so
  there is nothing extra for an end user to install.
- The audio path needs `libpulse` and `libpulse-simple`. On this machine that is PipeWire
  answering to PulseAudio's API, which is why it is the first backend: the same code runs
  on PipeWire, on PulseAudio, and on the compatibility layer infotainment images ship.
- Android Auto logs nothing to `logcat` on a production phone, so do not go looking.
  `adb` is still useful for telling a locked phone from an unresponsive one.
