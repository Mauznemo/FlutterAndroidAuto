# Research notes

Everything here was verified on 2026-09-12 on the dev machine described in
`dev-environment.md`. Anything not verified is marked as such.

## How Android Auto projection actually works

The phone is the computer. The head unit is a dumb-ish terminal that receives an H.264
video stream and sends input back. The head unit also plays the phone's audio and
captures the microphone for it.

### Transport

Two ways in:

**USB (AOAP).** The head unit is the USB host. It finds an Android phone, then uses
Android Open Accessory Protocol vendor control requests to ask the phone to switch into
accessory mode:

| Request | Meaning |
|---|---|
| `51` | Get AOAP protocol version |
| `52` | Send identifying strings (manufacturer, model, description, version, uri, serial) |
| `53` | Start accessory mode |

After request 53 the phone re-enumerates with vendor `0x18d1` and one of the product
ids `0x2d00`, `0x2d01`, `0x2d04`, `0x2d05`. The head unit then opens the bulk in/out
endpoints and everything after that is the same for both transports.

The magic string that makes the phone offer Android Auto rather than a generic accessory
is manufacturer `Android`, model `Android Auto`.

**Wi-Fi (Android Auto Wireless).** Bluetooth RFCOMM is used only to bootstrap: the head
unit advertises the AAW UUID, the phone connects, and they exchange
`WifiInfoRequest`/`WifiInfoResponse` and `WifiSecurityRequest`/`WifiSecurityResponse`
carrying the SSID, passphrase, head unit IP and port. The phone then opens a TCP socket
to the head unit on port **5288** and the rest is identical.

### Framing and security

On top of the transport sits a small frame header (channel id, flags, payload length),
then a TLS 1.2 session. The head unit authenticates with an X.509 certificate issued by
Google Automotive Link. Every open source implementation ships the same publicly known
certificate and key, which originally came from the Desktop Head Unit. The TLS records
are tunnelled inside the AA framing rather than sitting under it, so the handshake is
driven manually with an OpenSSL memory BIO.

### Channels

After the handshake: `VersionRequest`/`VersionResponse`, then
`ServiceDiscoveryRequest` from the phone and `ServiceDiscoveryResponse` from us. That
response is where the head unit declares what it is: screen resolution, DPI, supported
frame rates, audio stream formats, input capabilities, which sensors exist.

Channels the phone then opens, roughly in importance order:

| Channel | Direction | Payload |
|---|---|---|
| Control | both | version, ping, audio focus, video focus, shutdown, navigation focus |
| Video (MediaSink) | phone to HU | H.264 Annex-B, 480p/720p/1080p, 30 or 60 fps |
| Media audio (MediaSink) | phone to HU | PCM 48 kHz stereo 16 bit |
| System audio (MediaSink) | phone to HU | PCM 16 kHz mono 16 bit, notification sounds |
| Speech audio (MediaSink) | phone to HU | PCM 16 kHz mono 16 bit, Assistant voice |
| Microphone (MediaSource) | HU to phone | PCM 16 kHz mono 16 bit |
| Input | HU to phone | touch events, key events, absolute/relative |
| Sensor | HU to phone | night mode, driving status, location, speed, ... |
| Navigation status | phone to HU | turn by turn text and maneuver data |
| Media playback status | phone to HU | track metadata and transport state |
| Phone status | phone to HU | call state |
| Media browser | both | browse the phone's media library |
| Bluetooth | both | pairing handoff |
| Vendor extension | both | OEM specific |

The last five are the interesting ones for this project: they let the Flutter app render
**native** widgets from Android Auto state instead of only mirroring pixels.

A channel that is advertised in service discovery but then never serviced will get the
connection dropped by the phone.

## Library choice

| Project | Language | Licence | State | Notes |
|---|---|---|---|---|
| [`f1xpl/aasdk`](https://github.com/f1xpl/aasdk) | C++ | GPL-3.0-or-later | archived, last real work 2018 | the original, everything else descends from it |
| [`opencardev/aasdk`](https://github.com/opencardev/aasdk) | C++ | GPL-3.0-or-later | **active, last commit 2026-06-11** | modernised CMake, better logging, packaging, multi-arch |
| [`uglyoldbob/android-auto`](https://github.com/uglyoldbob/android-auto) | Rust | LGPL-3.0-or-later | young | LGPL would be friendlier, but far less proven |
| [`aa-proxy/aa-proxy-rs`](https://github.com/aa-proxy/aa-proxy-rs) | Rust | check repo | active | a wireless **proxy**, not a head unit, wrong shape for us |
| OpenAuto / OpenDsh / crankshaft | C++/Qt | GPL-3.0 | varies | full head unit apps built on aasdk, useful as reference |

**Chosen: `opencardev/aasdk`.** It is the only actively maintained C++ implementation of
the full protocol, it already has the protobuf definitions and the certificate, and it
covers USB, TCP and SSL. We vendor it as a submodule and keep our changes as a patch
series so upstream can still be tracked.

### What is inside opencardev/aasdk

```
include/aasdk/{USB,TCP,Transport,Messenger,IO,Channel,Error,Common}
src/...
protobuf/aap_protobuf/{service,channel,shared,aaw}
cert/{headunit.crt,headunit.key}
```

Channel implementations present: Control, Bluetooth, GenericNotification, InputSource,
MediaBrowser, MediaPlaybackStatus, MediaSink, MediaSource, NavigationStatus,
PhoneStatus, Radio, SensorSource, VendorExtension, WifiProjection. That is everything
M3 to M10 need.

### The one real build problem

`aasdk` uses `boost::asio::io_service` in **95 source files**, plus
`boost::asio::io_service::strand` 67 times and `ip::address::from_string`.

- `io_service` was deprecated in Boost 1.66 and **removed in Boost 1.87**.
- Ubuntu 26.04 ships **Boost 1.90**.

So a port is mandatory, not optional. It is mechanical:

| Old | New |
|---|---|
| `boost::asio::io_service` | `boost::asio::io_context` |
| `boost::asio::io_service::strand` | `boost::asio::strand<boost::asio::io_context::executor_type>` |
| `strand_.wrap(handler)` | `boost::asio::bind_executor(strand_, handler)` |
| `boost::asio::ip::address::from_string` | `boost::asio::ip::make_address` |
| `io_service.post(h)` | `boost::asio::post(io_context, h)` |

The strand construction also changes shape: the old `strand(io_service&)` constructor
becomes `strand<...>(io_context.get_executor())`.

Escape hatches if this goes badly: pin and build Boost 1.86 into the repo, or replace
Asio entirely with a small epoll reactor behind aasdk's existing IO interfaces.

### Other dependency notes

| Dependency | Ubuntu 26.04 version | Comment |
|---|---|---|
| Boost | 1.90 | see above |
| protobuf | 3.21.12 | aasdk's CMake defaults to building protobuf 30.0 itself; try `-DSKIP_BUILD_PROTOBUF=ON` first, the bundled `.proto` files are plain proto2/proto3 and should not need a newer protoc |
| OpenSSL | 3.5.5 | fine |
| libusb | 1.0.29 | fine |
| Abseil | system | only needed if we build protobuf ourselves |

## Licence situation

**Settled: the repository is GPL-3.0-or-later.**

`aasdk` is GPL-3.0-or-later. Linking it into the plugin makes the plugin, and any
application shipping the plugin, GPL-3.0. There is no way around that while using aasdk,
and the infotainment app this is built for is GPL-3.0 anyway.

The federated package split still matters, for future optionality rather than for
licensing today:

- `android_auto_platform_interface` - pure Dart, no aasdk, can be permissive
- `android_auto` - pure Dart, no aasdk, can be permissive
- `android_auto_linux` - links aasdk, **must be GPL-3.0-or-later**

An app that depends on `android_auto` and therefore pulls in `android_auto_linux` is
subject to GPL-3.0. The README says so plainly.

The head unit certificate is publicly known and shipped by every implementation, but it
is issued to a third party. aasdk compiles it in as a pair of string literals in
`Messenger/Cryptor.cpp`, so substituting one means rebuilding aasdk rather than
configuring the plugin.

## Flutter side

### Renderer situation on Linux, and why it constrains the design

Verified on this machine, Flutter 3.47.4, by reading the app's own startup log:

```
[IMPORTANT:...embedder_surface_gl_impeller.cc(126)]
    Using the Impeller rendering backend (OpenGLESSDF).
```

So, as of Flutter 3.47:

- **Impeller is the only renderer on Linux.** Skia is gone.
- Impeller on Linux currently runs its **OpenGLES** backend.
- `--no-enable-impeller` is a **verified no-op**. Passing it changes nothing, the log
  still says Impeller. There is no fallback renderer to retreat to, so any plan that
  treats "turn Impeller off" as an escape hatch is already wrong.
- An **Impeller Vulkan backend for Linux and Windows desktop** is in progress upstream
  (design doc [flutter/flutter#183495](https://github.com/flutter/flutter/issues/183495),
  tracking #181711, draft PR #183382). Assume Linux moves to Vulkan.

This matters beyond renderer trivia, because **Flutter GPU and `flutter_scene` want
Vulkan**. Flutter GPU runs on GLES but with known rough edges, and `flutter_scene`'s
retained backend activates only on Metal and Vulkan contexts. A host app that wants 3D
in its infotainment UI is therefore pulling in the same direction Flutter is already
heading.

### External textures, and the Vulkan problem

The Flutter Linux embedder exposes `FlTextureRegistrar` with two texture types:

- `FlTextureGL` - we hand over a GL texture name. Flutter only accepts `GL_RGBA8`.
- `FlPixelBufferTexture` - we hand over CPU pixels, the engine uploads them.

Verified against the headers Flutter 3.47.4 ships in
`example/linux/flutter/ephemeral/flutter_linux/`:

```c
struct _FlTextureGLClass {
  GObjectClass parent_class;
  gboolean (*populate)(FlTextureGL* texture, uint32_t* target, uint32_t* name,
                       uint32_t* width, uint32_t* height, GError** error);
};

gboolean fl_texture_registrar_register_texture(FlTextureRegistrar*, FlTexture*);
gboolean fl_texture_registrar_mark_texture_frame_available(FlTextureRegistrar*, FlTexture*);
gboolean fl_texture_registrar_unregister_texture(FlTextureRegistrar*, FlTexture*);
```

The header states that Flutter's GL context is already current when `populate` is
called, so that callback must not make another context current.

`FlTextureGL` works today because Linux is on Impeller-GLES. It is, by name and by
signature, an **OpenGL** interface. Impeller's Vulkan external texture work so far
([flutter/flutter#137639](https://github.com/flutter/flutter/issues/137639), closed) was
about Android `SurfaceTexture`, not a Linux embedder API. When Linux moves to Vulkan,
the Linux embedder needs a new external texture entry point, and `FlTextureGL` either
gains a GL interop shim or is superseded.

**The design response, and the reason this is not a trap:** do not treat "a GL texture"
as the thing the video pipeline produces. Produce a **dmabuf**. VA-API already decodes
into dmabuf-backed surfaces, and a dmabuf imports into either API:

| Target | Import path |
|---|---|
| OpenGL / EGL | `EGL_EXT_image_dma_buf_import` to `EGLImage` to GL texture |
| Vulkan | `VK_EXT_external_memory_dma_buf` plus `VK_EXT_image_drm_format_modifier` to `VkImage` |

Everything upstream of the import is shared. Only a thin present adapter is API
specific, so following Flutter to Vulkan is a contained change rather than a rewrite.
See `docs/architecture.md` for where that seam sits.

### Why FFI and not method channels

Touch events at 60 Hz and per frame signalling through a method channel would add
latency and garbage. The design is:

- `pluginClass` entry point: runs once, stashes the `FlTextureRegistrar`.
- FFI: everything hot (start/stop, config, touch, sensor pushes, frame callbacks).
- `NativeCallable.listener` + a Dart `SendPort`: native to Dart events without blocking
  a native thread on the Dart isolate.

### Video decode path

Preferred: `libavcodec` with `AV_HWDEVICE_TYPE_VAAPI`, frames exported as dmabuf,
imported into GL with `EGL_EXT_image_dma_buf_import` as an `EGLImage`, bound to a
texture. Zero copy, which matters on an ARM mini PC.

Fallback: software `libavcodec` to `YUV420P`, converted to RGBA either by a shader or on
the CPU into an `FlPixelBufferTexture`.

The dev machine has Intel UHD (Comet Lake) so VA-API is available, though `vainfo` is not
installed yet.

### Audio

PipeWire is running with the PulseAudio compatibility layer, so `libpulse-simple` is the
pragmatic first backend: it works on PipeWire, PulseAudio and on the target mini PC.
Revisit with native libpipewire if latency is not good enough.

## Sources

- [opencardev/aasdk](https://github.com/opencardev/aasdk)
- [f1xpl/aasdk](https://github.com/f1xpl/aasdk)
- [uglyoldbob/android-auto (Rust, LGPL)](https://github.com/uglyoldbob/android-auto)
- [aa-proxy/aa-proxy-rs](https://github.com/aa-proxy/aa-proxy-rs)
- [Flutter Linux embedder: FlTextureGL](https://api.flutter.dev/linux-embedder/struct___fl_texture_g_l_class.html)
- [Flutter Linux embedder: fl_texture_registrar.h](https://api.flutter.dev/linux-embedder/fl__texture__registrar_8h.html)
- [flutter/flutter#137639 Impeller Vulkan external textures](https://github.com/flutter/flutter/issues/137639)
- [flutter/flutter#181656 Impeller Linux pixel buffer texture fix](https://github.com/flutter/flutter/pull/181656)
