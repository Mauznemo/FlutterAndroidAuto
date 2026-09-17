# Flutter Android Auto

A Flutter plugin that runs a real Android Auto head unit **inside** a Flutter app.

The projected phone screen is rendered into a Flutter `Texture`, so the host app can
composite its own widgets on top of it. No separate window, no Desktop Head Unit
executable, no window manager tricks.

Built for custom car infotainment systems written in Flutter. Linux first (x86_64 and
ARM64), with the package layout prepared for an Android implementation later.

> **Status: early. Nothing works yet.** The protocol research, architecture and
> milestone plan are done, the native head unit core has not been built.
> See [`PLAN.md`](PLAN.md) for exactly where things stand.

## What it will look like

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
PLAN.md                               milestones with checkboxes
```

`dev/` is not part of the plugin: it drives one KDE-on-Wayland laptop with one paired
phone, and the project builds and runs without it. See [`dev/README.md`](dev/README.md).

## Getting started

```bash
git submodule update --init --recursive
tools/setup-dev-machine.sh --build-deps --udev
tools/build-aasdk.sh
cd example && flutter run -d linux
```

`build-aasdk.sh` is not optional: the vendored aasdk does not compile unpatched against
a current Boost, and the plugin's CMake refuses to build until it has been run.

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
