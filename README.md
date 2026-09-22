# Flutter Android Auto

[![pub package](https://img.shields.io/pub/v/android_auto.svg)](https://pub.dev/packages/android_auto)

A Flutter plugin that runs a real Android Auto head unit **inside** a Flutter app.

The projected phone screen is rendered into a Flutter `Texture`, so the host app can
composite its own widgets on top of it. No separate window, no Desktop Head Unit
executable, no window manager tricks.

Built for custom car infotainment systems written in Flutter. Linux first, built and
tested on x86_64 with ARM64 built in CI and not yet run on a device, and the package
layout is federated so an Android implementation can be added without touching the app
facing API.

![A Pixel projecting Google Maps into a Flutter texture, under the example app's own
status bar and with its controls and a panel drawn over it](docs/images/example-projecting.png)

*A phone projecting over USB, decoded on VA-API into a Flutter `Texture`. The status bar
at the top is the example app's own, and the phone has laid itself out for the space
left below it. The dock of keys and buttons along the bottom and the sensor panel are
ordinary Flutter drawn on top of the projection. The position is the example app's
fixed test fix, not a real one.*

> **Status: released, early.** Published on pub.dev as
> [`android_auto`](https://pub.dev/packages/android_auto). A phone projects over USB and
> over Wi-Fi, with video, touch and key input, three audio streams, the microphone, car
> sensors, and the metadata channels that let the head unit draw its own turn card and
> now playing bar. ARM64 has been built but never run on a device, and only one phone
> model has been tested. See [What works](#what-works) for the detail.

## Usage

```dart
final controller = AndroidAutoController(
  config: const AndroidAutoConfig(fps: 30),
);

Column(
  children: [
    MyStatusBar(),                                // your widgets, beside it
    Expanded(
      child: Stack(
        children: [
          AndroidAutoView(controller: controller),  // the phone's screen
          MyNowPlayingCard(),                       // on top, fed by AA metadata
        ],
      ),
    ),
  ],
)
```

The phone lays its interface out in whatever shape the view is, even though the protocol
only has 16:9 frame sizes: it is asked to leave margins inside the frame, and those are
cropped off before the texture reaches Flutter. So a status bar beside the view takes
space from the phone rather than covering part of it, with no black bars either way.
`AndroidAutoConfig.matchViewAspectRatio: false` turns that off.

`controller.start()` begins looking for a phone and `controller.events` reports what
happens; `AndroidAutoView` maps its own touches into the projected video, so input needs
no wiring. Everything else is a getter, a setter or a stream on the controller: the
sensors the car reports (`setNightMode`, `setLocation`, `setParkingBrake` and the rest),
the three audio streams' volume and mute, and `navigation`, `mediaPlayback`,
`phoneStatus` and `notifications` for drawing the car's own interface rather than
mirroring the phone's pixels.

The example app under [`example/`](example/) is the reference integration: one
controller, one view, a status bar, the hardware keys, and the turn card and now playing
bar drawn from metadata, with test bench panels for every other part of the API. It is
the best documentation in the repository, and [its README](example/README.md) says which
file to read first.

## What works

| | |
|---|---|
| Video | H.264 to a Flutter texture, VA-API zero copy with a software fallback, laid out by the phone for a view of any shape |
| Input | touch (multi-touch), keys and a rotary controller |
| Audio out | media, system and speech as three streams, mixed and ducked by the head unit |
| Audio in | the microphone, opened only when the phone asks for it |
| Sensors | night mode, driving status, location, speed, gear, parking brake and more |
| Metadata | navigation, media playback and telephony, plus notification and media browse |
| Transports | USB (AOAP), and Wi-Fi with Bluetooth for the handshake |
| Phone calls | over Bluetooth HFP, which is configuration rather than code, see [`docs/echo-cancellation.md`](docs/echo-cancellation.md) |

Not there yet: no ARM64 device tested (CI builds it, which is not the same thing), no
Android implementation, and the notification and media browser channels are
implemented against the schema but unverified, because the one phone tested never opens
them.

## Repository layout

```
packages/
  android_auto                        app facing API, pure Dart
  android_auto_platform_interface     the contract, pure Dart
  android_auto_linux                  Linux implementation, links aasdk
example/                              the reference integration and test bench
docs/
  research.md                         protocol notes and library evaluation
  architecture.md                     how the pieces fit together
  aasdk-port-notes.md                 what the vendored aasdk needed and why
  echo-cancellation.md                calls over Bluetooth, and the config they need
  wireless.md                         Android Auto without a cable
  packaging.md                        shipping a build, and what a machine needs to run it
  releasing.md                        cutting a release, for the maintainer
tools/                                what anyone cloning this needs
  setup-dev-machine.sh                host provisioning
  build-aasdk.sh                      vendored aasdk: patch, build, smoke test
  port-aasdk.sh                       regenerate that patch
  install-echo-cancel.sh              the echo canceller a hands free call needs
  wireless-ap.sh                      bring this machine up as the access point
dev/                                  the author's own machine tooling, not shipped
pubspec.yaml                          workspace root, not a package
PLAN.md                               how it was built, milestone by milestone
```

The three packages and the example are one Dart workspace, so `flutter pub get` and
`flutter analyze` run from the repository root and there is a single `pubspec.lock`.
Tests are named by package, because the root is not one:

```bash
flutter test packages/*/test
```

`dev/` is not part of the plugin: it drives one KDE-on-Wayland laptop with one paired
phone, and the project builds and runs without it. See [`dev/README.md`](dev/README.md).

## Getting started

```bash
flutter pub add android_auto
```

That is the only package to name.
[`android_auto_linux`](https://pub.dev/packages/android_auto_linux) is endorsed by it
and comes along on its own, and
[`android_auto_platform_interface`](https://pub.dev/packages/android_auto_platform_interface)
is only for writing another implementation.

**Doing so makes your application GPL-3.0-or-later**, because the Linux implementation
links aasdk. See [Licence](#licence) below before going further.

The Linux implementation compiles aasdk from source as part of your app's build, so the
machine building it needs the development packages. On Ubuntu or Debian:

```bash
sudo apt-get install cmake ninja-build pkg-config \
  libboost-all-dev libusb-1.0-0-dev libssl-dev libprotobuf-dev protobuf-compiler \
  libavcodec-dev libavutil-dev libswscale-dev libva-dev libpulse-dev \
  libgtk-3-dev libegl1-mesa-dev libgles2-mesa-dev
```

A machine that only runs the built app needs the runtime libraries instead, listed in
[`docs/packaging.md`](docs/packaging.md). Either way, a normal user can only open a phone
in accessory mode with a udev rule; `tools/setup-dev-machine.sh --udev` in this
repository writes one, and it is short enough to copy from there.

The minimum an app needs is a controller and a view. `AndroidAutoConfig` defaults to a
30 fps head unit whose frame size follows its view: the smallest of 800x480, 1280x720 and
1920x1080 that covers it. Set `width` and `height` to pin one instead; they must be one of
800x480, 1280x720, 1920x1080, 2560x1440 or 3840x2160, since the protocol has names for no
others.
Wireless is opt-in, with `transports: {AndroidAutoTransport.usb,
AndroidAutoTransport.wireless}` and the Wi-Fi passphrase in `AndroidAutoWirelessConfig`.

To build this repository and run the example, on Ubuntu or Debian:

```bash
git submodule update --init --recursive
tools/setup-dev-machine.sh --build-deps --udev
cd example && flutter run -d linux
```

The vendored aasdk does not compile unpatched against Boost 1.87 or newer, but that is
not a step to remember: the plugin's CMake applies the patch itself at configure time,
and refuses with a readable message if it cannot. `tools/build-aasdk.sh` builds aasdk on
its own and runs a smoke test against it, which is useful when the port is what is in
question and unnecessary otherwise.

`--udev` installs the rule that lets a normal user open a phone in accessory mode,
without which projection needs root.

`--build-deps` also installs ccache, which is not required but turns a clean rebuild
from three minutes into twenty seconds: `flutter clean` deletes the object tree and
aasdk is almost all of it. The plugin's CMake picks ccache up on its own when it is
there and says so at configure time when it is not.

## Licence

**GPL-3.0-or-later**, see [`LICENSE`](LICENSE).

The Linux implementation links [`aasdk`](https://github.com/opencardev/aasdk), which is
GPL-3.0-or-later, so any application shipping this plugin is GPL-3.0-or-later too.

The bundled head unit certificate is the publicly known Google Automotive Link
certificate that every open source Android Auto implementation uses. aasdk compiles it
into the library (`Messenger/Cryptor.cpp`), so there is no runtime override: an
integrator with their own certificate has to rebuild aasdk carrying it.

## Credits

Stands on the work of [f1xpl](https://github.com/f1xpl/aasdk) (original aasdk and
OpenAuto) and the [OpenCarDev](https://github.com/opencardev) community, who keep it
building.
