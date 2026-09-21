# android_auto_platform_interface

The contract between [`android_auto`](https://pub.dev/packages/android_auto) and its
platform implementations. Pure Dart, no native code, no aasdk derived code.

**Most people want `android_auto` instead.** This package is for writing a new platform
implementation, or for a host app that needs a type the app facing package does not
re-export.

## What is in it

- `AndroidAutoPlatform`, the interface every implementation extends.
- `AndroidAutoConfig`: the projected size, frame rate and density, how the head unit
  describes itself to the phone, and which sensors, metadata channels and transports it
  offers.
- The models everything else is expressed in: connection state and events, video
  information, touch and key input, audio streams and devices, the twelve sensors, the
  five metadata kinds with their navigation, media, telephony, notification and browse
  payloads, and the wireless configuration and status.

## Writing an implementation

Extend `AndroidAutoPlatform` and set `AndroidAutoPlatform.instance`. The base class uses
`plugin_platform_interface`, so implementations must extend it rather than implement it,
which is what lets methods be added later without breaking every implementation at once.

Some enums cross a foreign function boundary positionally in the Linux implementation,
so their declared order is part of the platform boundary rather than an internal detail.
Those say so in their own doc comments.

## Licence

GPL-3.0-or-later, see `LICENSE`. This package contains no aasdk derived code, which
keeps the option of relicensing it open, but it is GPL-3.0-or-later as it stands.

Source, issues and the full documentation:
<https://github.com/Mauznemo/FlutterAndroidAuto>
