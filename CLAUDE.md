## Style

- Never use em dashes (—) or en dashes (–), anywhere: not in UI strings, dart docs, normal comments, or markdown. Use a comma, parentheses, or a separate sentence instead.
- When defining widgets or classes put all the vars on top of the constructor and not the other way around.
- Update this or other CLAUDE.mds if the info in here changes or something new is worth adding if it will be needed for every later session. Do not clutter it with one of info or things that are just common sense or easy to figure out without having it here. If you think something one off needs explaining the dart docs with /// is the right place, not this file.

## What this project is

A Flutter plugin that runs an Android Auto head unit inside a Flutter app, rendering the
projected phone screen into a Flutter `Texture` so the host app can draw widgets on top.
Linux first, federated package layout so Android can be added later.

**Read `PLAN.md` first in every session.** It holds the milestones with checkboxes and is
the source of truth for what is done and what is next. Tick a box only when the thing is
verified on this machine, not when it merely compiles. Also update the status table at
the top of `PLAN.md` when a milestone changes state.

Background reading, only when relevant: `docs/research.md` (protocol and library
evaluation), `docs/architecture.md` (how the pieces fit), `docs/dev-environment.md`
(machine specifics).

## Layout

| Path | What |
|---|---|
| `packages/android_auto` | app facing API, pure Dart, no GPL code |
| `packages/android_auto_platform_interface` | the contract, pure Dart, no GPL code |
| `packages/android_auto_linux` | Linux implementation, links aasdk, **GPL-3.0** |
| `example/` | test bench app, run this to verify anything visually |
| `tools/` | `ui.sh`, `run-example.sh`, `setup-dev-machine.sh`, `build-aasdk.sh`, `port-aasdk.sh` |

Keep aasdk code out of the two pure Dart packages. That split is what keeps a future
permissive implementation possible.

## You can drive this machine yourself

KDE Plasma on **Wayland**, 1920x1080 at scale 1, so screenshot pixels equal screen
coordinates. X11 tools (xdotool, scrot, wmctrl) do not work. sudo is passwordless.

```bash
tools/ui.sh setup              # once per boot: starts ydotoold, flattens pointer accel
tools/ui.sh shot               # full screen png, prints the path, then Read it
tools/ui.sh crop <f> <x> <y> <w> <h>
tools/ui.sh click <x> <y>      # also: move, rclick, drag, scroll
tools/ui.sh key ctrl+s         # also: esc, enter, tab, f1..f12
tools/ui.sh paste "text"       # use this, not `type`, see below
tools/run-example.sh --bg      # build and launch the test bench detached
tools/run-example.sh --bundle  # run the built binary directly, line buffered logs
```

Three things that will bite otherwise:

- **`tools/ui.sh setup` is required after every reboot.** Without the flat pointer
  acceleration it sets on the ydotool virtual device, clicks land in the wrong place.
- **The keyboard layout is German (QWERTZ)** and ydotool sends raw keycodes, so
  `ui.sh type` mangles y/z and symbols. Use `ui.sh paste` for exact text.
- **Never run `pkill -f <pattern>`**, it matches the agent's own shell command line and
  kills the session. Resolve the pid first with `pgrep` into a variable built by
  concatenation, then `kill` it. `pgrep -x android_auto_example` never matches either:
  the name is over 15 characters. Match on `bundle/android_auto_ex""ample` instead.
- **Never leave two copies of the example app running.** They fight over the phone and
  the result looks exactly like a protocol bug: handshakes that half complete, reads that
  time out, a phone that goes silent. `tools/run-example.sh` kills the old one first, so
  use it rather than launching the bundle by hand.

## Build and run

```bash
cd example && flutter pub get && flutter build linux --debug
tools/run-example.sh --bg
```

`--bg` runs through `flutter run`, which block buffers the app's stdout: native logging
arrives in 8 KB lumps, minutes late, which is useless for watching a protocol exchange.
Use `--bundle` for that, which runs the built binary directly and line buffers.

Three environment knobs, all off unless set:

| Knob | What it does |
|---|---|
| `AA_LOG_LEVEL=DEBUG` | aasdk's own protocol log, the service discovery exchange in full, and libavcodec's diagnostics |
| `AA_SERVICES=video,input,sensor` | narrows or widens the advertised channel set without a rebuild. `all` for everything |
| `AA_VIDEO_DECODER=software` | forces the software decoder, to tell a driver problem from a decoder problem |
| `AA_FAULT_TRANSPORT_AFTER=20` | kills the transport after N seconds without touching USB, to exercise the reconnect path on demand |

Flutter 3.47.4 stable via snap at `~/snap/flutter/common/flutter`.

**Impeller is the only renderer on Linux now** and it runs its **OpenGLES** backend
(`Using the Impeller rendering backend (OpenGLESSDF)`). `--no-enable-impeller` is a
verified no-op, there is no Skia fallback to retreat to. Impeller's Vulkan backend for
Linux desktop is in progress upstream, and the project has to be ready for it, so the
video pipeline hands Flutter a **dmabuf**, never a raw GL texture. See
`docs/architecture.md`.

## Native build

```bash
tools/build-aasdk.sh           # submodule, patch, build, smoke test. Must print OK.
tools/port-aasdk.sh reset      # throw away local aasdk edits
tools/port-aasdk.sh apply      # redo the port in the working tree
tools/port-aasdk.sh patch      # regenerate linux/patches/ from the working tree
```

aasdk is vendored as a submodule at
`packages/android_auto_linux/linux/third_party/aasdk`, pinned to `9bf6adf`. **It does not
build unpatched**: Boost 1.90 removed `io_service`, and there are four other breakages.
The port is a single patch in `linux/patches/`, generated by `tools/port-aasdk.sh`, and
the submodule is never committed dirty. If you change the port, regenerate the patch and
re-run `tools/build-aasdk.sh`. Full write up in `docs/aasdk-port-notes.md`.

The key piece: `aasdk::Strand` subclasses `boost::asio::strand<io_context::executor_type>`
to restore the old one argument `dispatch`/`post`, `get_io_service()` and an
`io_context&` flavoured `context()`. That is what keeps the port a type substitution
instead of a rewrite, so prefer extending it over touching call sites.

## Committing
Do NOT git commit unless you are toled to do so!

## Native layout (M2 onward)

| File | What |
|---|---|
| `linux/src/aa_core.h` | the flat C ABI, the only thing Dart binds to |
| `linux/src/aa_core.cc` | session lifecycle, owns the `io_context` thread pool |
| `linux/src/frame_ring.*` | the API agnostic seam, three slots, producer to raster thread |
| `linux/src/present/gl_adapter.*` | **the only file allowed to name a GL type** |
| `linux/src/present/texture_registry.*` | the `FlTextureRegistrar` the GTK entry point captured |
| `linux/src/event_bus.*` | native to Dart events |
| `linux/src/video/video_decoder.*` | H.264 to frames on its own thread, VA-API or software |
| `linux/src/session/video_channel.*` | the MEDIA_SINK_VIDEO channel |
| `linux/src/session/support_channels.*` | the channels that must answer for video to flow at all |
| `linux/src/test_pattern.*` | drives the texture without a phone, for overlay layout |

After changing `aa_core.h`, regenerate the Dart bindings:

```bash
cd packages/android_auto_linux && dart run ffigen --config ffigen.yaml
```

`AaState` in `aa_core.h` and `AndroidAutoConnectionState` in the platform interface cross
FFI as plain integers, so their orders must stay in step.

Only the raster thread touches GL, inside `populate()`, where Flutter's context is
already current. That stayed true through M4: the dmabuf is imported as an `EGLImage` and
converted to RGBA by a shader inside `populate()`, so there is still no second GL context
and no cross-context fence to get right.

Two rules that cost real time in M4:

- **Every `glBindTexture` lands on whichever texture unit is active.** The NV12 converter
  binds luma on unit 0, chroma on unit 1 and the output texture on unit 2, in that order.
  Binding the output while unit 1 was current replaced the chroma plane with the previous
  frame: a perfect picture in entirely the wrong colours.
- `populate()` runs inside Flutter's own rasterisation, so `GlStateGuard` saves and
  restores everything the converter touches. Impeller re-binds most of what it uses, but
  "most" is not a contract.

## What Android Auto demands before it will project

Learned the hard way in M4, and none of it reports an error. A phone that dislikes the
service discovery response just stops talking and drops out of accessory mode.

- **Advertise every channel, not only the implemented ones.** A head unit offering video,
  input and sensors is one Android Auto refuses to project to. The three audio sinks and
  the microphone have to be there too. `src/session/support_channels.cc` answers them and
  discards what they carry until M6 and M7 make them real. The older rule still holds on
  top of this: an advertised channel that is never serviced gets the connection dropped.
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

- **Coordinates are projected video pixels.** Not logical pixels, not normalised. The
  head unit tells the phone it has a touchscreen exactly the size of the video it asked
  for. `AndroidAutoView` maps widget-local positions through the same `applyBoxFit`
  arithmetic `FittedBox` paints with, so letterboxing stays consistent between what is
  drawn and where taps land.
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
`SupportChannels` have the same shape of race and have not been given the same treatment.

**Movement is rate limited to one report per 16 ms.** Without it the head unit emits one
USB bulk write per Flutter pointer event, measured at 382 a second on a desktop mouse.
What comes back is a video stream of at most 60 fps, so extra samples cannot produce a
distinguishable picture. Only movement is limited: a finger landing or lifting is an edge,
not a sample, and has to go at once.

## Stopping and resuming a session

The order matters and it is not obvious. `PLAN.md` under M3 has the full reasoning.

1. `ByeByeRequest`, then **wait for the acknowledgement**. The phone keeps Android Auto
   running, and its claim on the USB interface, until it answers.
2. Then `libusb_reset_device`. After a ByeBye the phone stays in accessory mode and will
   not answer a fresh version request; only redoing the AOAP handshake re-arms it, and
   that needs it out of accessory mode first.

Skip step 1 and the phone is wedged. Skip step 2 and every other reconnect times out.

A run that dies before step 1 leaves the phone wedged for the next launch, so a session
that errors before ever reaching connected bounces the phone and retries, up to three
times.

Losing the cable mid session is a separate path: it reports `searching`, not `error`, and
resumes on its own. That needs discovery to be re-armed after **every** handover, because
`USBHub::handleDevice` ignores arrivals while its promise is null.

**Reading aasdk USB errors:** `USB_TRANSFER`'s "Native Code" is a
`libusb_transfer_status`, not a `libusb_error`. 2 is TIMED_OUT, 4 is STALL. The patched
`USBEndpoint` now prints the name, the endpoint and the byte counts, so this no longer
has to be decoded by hand.

## A dead transport does not mean the phone went away

The case that cost an evening in M5, and the reason `searching` could hang forever.

A failed bulk transfer (`LIBUSB_TRANSFER_ERROR` on a marginal link) kills the transport
**while leaving the phone enumerated and still in accessory mode**. M3's recovery waits
for `USBHub` to hand it a device, and the hub only fires on arrival, so nothing ever
came: the device had never left. Stop then Start was the only way out, because stopping
calls `ResetDevice()` and that is what makes the phone re-enumerate.

So a connected session that loses its transport **bounces the phone itself** rather than
waiting to be told about it. Right for both cases: if the cable really is out, the reset
fails harmlessly on a device that has already gone, and re-arming discovery is what the
replug needed anyway. Measured at 4.6 seconds from dead transport back to projecting.

One dead transport is one event however many channels notice it. All seven do, within a
millisecond, and they report their failures as messages on the **connected** state, which
puts `reached_connected` back up in between. The recovery is debounced with a `recovering`
flag; do not remove it or one failure bounces the phone once per channel.

## aasdk object lifetimes, the thing that keeps biting

aasdk was written for openauto, which builds everything once and exits the process when
the phone disconnects. Nothing in it survives being torn down and rebuilt in place: its
objects hold raw pointers and references to things the caller owns, and they outlive
them in ways no ordering fixes. Seven crashes came out of this, listed in `PLAN.md`
under M3.

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

When something crashes in a destructor or inside libusb's event thread, it is almost
always one of these rather than a new problem.

## Native notes that keep coming back

- `android_auto_linux` must keep `pluginClass` in its pubspec even though it is mostly
  FFI. The GTK registration entry point is the only way to get the `FlTextureRegistrar`.
- The `io_context` thread pool must never run on Flutter's platform thread, and nothing
  outside the raster thread may call into Flutter's GL context without making the shared
  context current first.
- Texture *registration* must happen on the platform thread (so, from an FFI entry
  point). Only `mark_texture_frame_available` is safe from a producer thread.
- Flutter's `apply_standard_settings` pins C++14, but aasdk headers need C++17, so the
  plugin target raises it afterwards. Do not remove that line.
- Clang on this machine targets the newest installed GCC. Without a matching
  `libstdc++-N-dev` every C++ compile dies with `'limits' file not found`, which looks
  like a project bug and is not. `tools/setup-dev-machine.sh --build-deps` installs it.
- The video path needs `libavcodec`, `libavutil` and `libswscale` (no libavformat, there
  is no container to demux). `drm_fourcc.h` comes from the kernel headers, not libdrm, so
  there is nothing extra for an end user to install.
- Android Auto logs nothing to `logcat` on a production phone, so do not go looking.
  `adb` is still useful for telling a locked phone from an unresponsive one.
