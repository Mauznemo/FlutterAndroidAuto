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
  config: const AndroidAutoConfig(width: 1280, height: 720, fps: 30),
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

`width` and `height` must be one of 800x480, 1280x720, 1920x1080, 2560x1440 or
3840x2160: the protocol has names for no others, and anything else is advertised as
1280x720 with a warning.

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

## Licence

GPL-3.0-or-later, see `LICENSE`. This package contains no aasdk derived code, which
keeps the option of relicensing it open, but it is GPL-3.0-or-later as it stands.

Source, issues and the full documentation:
<https://github.com/Mauznemo/FlutterAndroidAuto>
