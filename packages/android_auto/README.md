# android_auto

Run a real Android Auto head unit **inside** a Flutter app.

The projected phone screen is rendered into a Flutter `Texture`, so ordinary widgets
composite on top of it. No separate window, no Desktop Head Unit executable, no window
manager tricks. Built for custom car infotainment systems written in Flutter.

> **GPL-3.0-or-later.** The Linux implementation links
> [aasdk](https://github.com/opencardev/aasdk), so any application shipping this plugin
> is GPL-3.0-or-later too. Read [Licence](#licence) before depending on it.

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
happens. `AndroidAutoView` maps its own touches into the projected video, letterboxing
included, so input needs no wiring.

The [example app](https://github.com/Mauznemo/FlutterAndroidAuto/tree/main/example)
is the reference integration: a status bar, the hardware keys, and a turn card and now
playing bar drawn from metadata, with test bench panels for the rest of the API.

Left out, `width` and `height` are picked per connection: the smallest of 800x480,
1280x720 and 1920x1080 whose picture covers the view in physical pixels, so it is never
stretched. Set both to pin a size instead. They must then be one of 800x480, 1280x720,
1920x1080, 2560x1440 or 3840x2160: the protocol has names for no others, and anything
else is advertised as 1280x720 with a warning.

## What the controller offers

| | |
|---|---|
| Session | `start`, `stop`, `state`, `events`, and `startWireless` / `stopWireless` |
| Input | `sendTouch`, `sendKey`, `sendRotary`, all in projected video pixels |
| Audio | per-stream `volume`, `setVolume`, `muted`, `setMuted`, device selection, statistics, and a raw PCM tap |
| Microphone | `microphoneActive`, `microphoneLevel`, device selection. Capture happens only while the phone asks for it |
| Sensors | what the car reports: `setNightMode`, `setLocation`, `setParkingBrake`, `setSpeed`, `setGear` and the rest |
| Metadata | `navigation`, `mediaPlayback`, `phoneStatus`, `notifications` and media browsing, for drawing the car's own interface |
| Test pattern | `startTestPattern`, to lay an overlay out with no phone and no cable |

Advertising a sensor is a promise, not a feature switch. The phone subscribes to what is
offered and then waits, and for location it stops using its own receiver the moment the
head unit claims a position. Offer only what the app will actually feed.

## Platforms

Linux today, through `android_auto_linux`, over USB or over Wi-Fi. Built and tested on
x86_64; ARM64 is intended and not yet verified. The package layout is federated, so an
Android implementation can be added without touching this API.

## Setting up a Linux machine

**To build** an app that uses this package, the machine needs the development packages,
because `android_auto_linux` compiles aasdk from source as part of the app's build. On
Ubuntu or Debian:

```bash
sudo apt-get install cmake ninja-build pkg-config \
  libboost-all-dev libusb-1.0-0-dev libssl-dev libprotobuf-dev protobuf-compiler \
  libavcodec-dev libavutil-dev libswscale-dev libva-dev libpulse-dev \
  libgtk-3-dev libegl1-mesa-dev libgles2-mesa-dev
```

**To run** it, a machine that did not build it needs the runtime libraries, listed in
[`docs/packaging.md`](https://github.com/Mauznemo/FlutterAndroidAuto/blob/main/docs/packaging.md#runtime-dependencies).
Either way, projecting over USB as a normal user needs a udev rule, or the app can only
open the phone as root. The build does not need it; the first connection does.
[`tools/setup-dev-machine.sh --udev`](https://github.com/Mauznemo/FlutterAndroidAuto/blob/main/tools/setup-dev-machine.sh)
writes one, and the rule is short enough to copy out of it.

## Licence

GPL-3.0-or-later, see `LICENSE`. This package contains no aasdk derived code, which
keeps the option of relicensing it open, but it is GPL-3.0-or-later as it stands.

Source, issues and the full documentation:
<https://github.com/Mauznemo/FlutterAndroidAuto>
