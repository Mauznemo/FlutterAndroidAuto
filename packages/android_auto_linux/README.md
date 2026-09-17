# android_auto_linux

The Linux implementation of [`android_auto`](https://pub.dev/packages/android_auto).

**You do not depend on this directly.** Depend on `android_auto`; this package is
endorsed by it and registers itself.

> **GPL-3.0-or-later.** This package links
> [aasdk](https://github.com/opencardev/aasdk), which is GPL-3.0-or-later, so any
> application that ships it is GPL-3.0-or-later too. That is not avoidable while aasdk
> is the protocol implementation.

## What it does

A native head unit core behind a flat C ABI, bound with `dart:ffi`:

- **USB transport**: AOAP accessory mode, SSL, service discovery, and recovery from a
  transport that dies while the phone stays enumerated.
- **Wireless transport**: a Bluetooth RFCOMM service, the handshake that tells the phone
  which Wi-Fi network to join and which address to dial, then a TCP acceptor on 5288.
  Everything above that connection is byte for byte the USB path.
- **Video**: H.264 decoded through VA-API into a dmabuf, imported as an `EGLImage` and
  converted to RGBA on Flutter's raster thread, with a libswscale fallback. The plugin
  hands Flutter a dmabuf rather than a GL texture, so an Impeller Vulkan backend needs no
  redesign.
- **Audio**: media, system and speech as three PulseAudio streams, mixed here, with
  media ducked under speech. The microphone in the other direction, opened only when the
  phone asks.
- **Sensors and metadata**: what the car reports to the phone, and the five channels the
  phone reports on.

## Building

The vendored aasdk is a git submodule and does not compile unpatched against Boost 1.87
or newer. A fresh clone therefore needs its submodules, and the plugin's CMake applies
the patch itself at configure time.

System packages needed: Boost, libusb, OpenSSL, protobuf, libavcodec, libavutil,
libswscale, libva, libpulse and the GTK/EGL development headers.
`tools/setup-dev-machine.sh --build-deps` installs them on Ubuntu and Debian.

Talking to a phone over USB as a normal user needs a udev rule, installed by
`tools/setup-dev-machine.sh --udev`. Without it, projection needs root.

## Licence

GPL-3.0-or-later, see `LICENSE`.

Source, issues and the full documentation:
<https://github.com/mauznemo/FlutterAndroidAuto>
