# Flutter Android Auto

A Flutter plugin that runs a real Android Auto head unit **inside** a Flutter app.

The projected phone screen is rendered into a Flutter `Texture`, so the host app can
composite its own widgets on top of it. No separate window, no Desktop Head Unit
executable, no window manager tricks.

Built for custom car infotainment systems written in Flutter. Linux first, built and
tested on x86_64 with ARM64 intended and not yet verified, and the package layout is
federated so an Android implementation can be added without touching the app facing API.

> **Status: working, not yet released.** A phone projects over USB and over Wi-Fi,
> with video, touch and key input, three audio streams, the microphone, car sensors,
> and the metadata channels that let the head unit draw its own turn card and now
> playing bar. Nothing is published to pub.dev yet, packaging and ARM64 are the
> remaining work, and only one phone model has been tested. See
> [What works](#what-works) for the detail.

## Usage

```dart
final controller = AndroidAutoController(
  config: const AndroidAutoConfig(width: 1280, height: 720, fps: 30),
);

Stack(
  children: [
    AndroidAutoView(controller: controller),  // the phone's screen
    MyStatusBar(),                            // your widgets, on top
    MyNowPlayingCard(),                       // fed by AA metadata, not pixels
  ],
)
```

`controller.start()` begins looking for a phone and `controller.events` reports what
happens; `AndroidAutoView` maps its own touches into the projected video, so input needs
no wiring. Everything else is a getter, a setter or a stream on the controller: the
sensors the car reports (`setNightMode`, `setLocation`, `setParkingBrake` and the rest),
the three audio streams' volume and mute, and `navigation`, `mediaPlayback`,
`phoneStatus` and `notifications` for drawing the car's own interface rather than
mirroring the phone's pixels.

The example app under `example/` exercises all of it and is the best documentation in
the repository.

## What works

| | |
|---|---|
| Video | H.264 to a Flutter texture, VA-API zero copy with a software fallback |
| Input | touch (multi-touch), keys and a rotary controller |
| Audio out | media, system and speech as three streams, mixed and ducked by the head unit |
| Audio in | the microphone, opened only when the phone asks for it |
| Sensors | night mode, driving status, location, speed, gear, parking brake and more |
| Metadata | navigation, media playback and telephony, plus notification and media browse |
| Transports | USB (AOAP), and Wi-Fi with Bluetooth for the handshake |
| Phone calls | over Bluetooth HFP, which is configuration rather than code, see [`docs/echo-cancellation.md`](docs/echo-cancellation.md) |

Not there yet: no published packages, no ARM64 build verified, no Android
implementation, and the notification and media browser channels are implemented against
the schema but unverified, because the one phone tested never opens them.

## Repository layout

```
packages/
  android_auto                        app facing API, pure Dart
  android_auto_platform_interface     the contract, pure Dart
  android_auto_linux                  Linux implementation, links aasdk
example/                              test bench app
docs/
  research.md                         protocol notes and library evaluation
  architecture.md                     how the pieces fit together
  aasdk-port-notes.md                 what the vendored aasdk needed and why
  echo-cancellation.md                calls over Bluetooth, and the config they need
  wireless.md                         Android Auto without a cable
tools/                                what anyone cloning this needs
  setup-dev-machine.sh                host provisioning
  build-aasdk.sh                      vendored aasdk: patch, build, smoke test
  port-aasdk.sh                       regenerate that patch
  install-echo-cancel.sh              the echo canceller a hands free call needs
  wireless-ap.sh                      bring this machine up as the access point
dev/                                  the author's own machine tooling, not shipped
PLAN.md                               how it was built, milestone by milestone
```

`dev/` is not part of the plugin: it drives one KDE-on-Wayland laptop with one paired
phone, and the project builds and runs without it. See [`dev/README.md`](dev/README.md).

## Getting started

Nothing is on pub.dev yet, so depend on it from a checkout:

```yaml
dependencies:
  android_auto:
    path: ../flutter_android_auto/packages/android_auto
```

**Doing so makes your application GPL-3.0-or-later**, because the Linux implementation
links aasdk. See [Licence](#licence) below before going further.

The minimum an app needs is a controller and a view. `AndroidAutoConfig` defaults to a
720p30 head unit that every phone accepts; `width` and `height` must be one of 800x480,
1280x720, 1920x1080, 2560x1440 or 3840x2160, since the protocol has names for no others.
Wireless is opt-in, with `transports: {AndroidAutoTransport.usb,
AndroidAutoTransport.wireless}` and the Wi-Fi passphrase in `AndroidAutoWirelessConfig`.

To build the repository itself, on Ubuntu or Debian:

```bash
git submodule update --init --recursive
tools/setup-dev-machine.sh --build-deps --udev
tools/build-aasdk.sh
cd example && flutter run -d linux
```

`build-aasdk.sh` is not optional: the vendored aasdk does not compile unpatched against
a current Boost, and the plugin's CMake refuses to build until it has been run. `--udev`
installs the rule that lets a normal user open a phone in accessory mode, without which
projection needs root.

## Licence

**GPL-3.0-or-later**, see [`LICENSE`](LICENSE).

The Linux implementation links [`aasdk`](https://github.com/opencardev/aasdk), which is
GPL-3.0-or-later, so any application shipping this plugin is GPL-3.0-or-later too.

`android_auto` and `android_auto_platform_interface` contain no aasdk derived code. That
is deliberate: if a permissively licensed protocol implementation ever appears, those two
packages can be relicensed without untangling anything.

The bundled head unit certificate is the publicly known Google Automotive Link
certificate that every open source Android Auto implementation uses. aasdk compiles it
into the library (`Messenger/Cryptor.cpp`), so there is no runtime override: an
integrator with their own certificate has to rebuild aasdk carrying it.

## Credits

Stands on the work of [f1xpl](https://github.com/f1xpl/aasdk) (original aasdk and
OpenAuto) and the [OpenCarDev](https://github.com/opencardev) community, who keep it
building.
