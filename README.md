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
  dev-environment.md                  the dev machine and its quirks
tools/
  setup-dev-machine.sh                host provisioning
  run-example.sh                      build and launch the test bench
  ui.sh                               screenshot and synthetic input
PLAN.md                               milestones with checkboxes
```

## Getting started

```bash
tools/setup-dev-machine.sh --build-deps --udev
tools/run-example.sh
```

## Licence

**GPL-3.0-or-later**, see [`LICENSE`](LICENSE).

The Linux implementation links [`aasdk`](https://github.com/opencardev/aasdk), which is
GPL-3.0-or-later, so any application shipping this plugin is GPL-3.0-or-later too.

`android_auto` and `android_auto_platform_interface` contain no aasdk derived code. That
is deliberate: if a permissively licensed protocol implementation ever appears, those two
packages can be relicensed without untangling anything.

The bundled head unit certificate is the publicly known Google Automotive Link
certificate that every open source Android Auto implementation uses. It is loaded from a
configurable path so integrators can supply their own.

## Credits

Stands on the work of [f1xpl](https://github.com/f1xpl/aasdk) (original aasdk and
OpenAuto) and the [OpenCarDev](https://github.com/opencardev) community, who keep it
building.
