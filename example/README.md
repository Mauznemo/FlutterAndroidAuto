# android_auto_example

The test bench. It exercises every channel the plugin implements, and it is the most
complete documentation of the API in the repository: if something is unclear in a doc
comment, the call is in here with a button attached to it.

Run it from the repository root, after `tools/build-aasdk.sh`:

```bash
cd example && flutter run -d linux
```

Then plug a phone in and press **Start head unit**.

## What is on screen

The projected video fills the window, with everything else composited on top of it as
ordinary Flutter widgets. That is the point being demonstrated: none of the overlay is
read off the phone's pixels.

**The status bar along the top** is the head unit's own view of itself: connection
state, the Flutter texture id, the video size and which decoder is running, the last
input event sent, the audio backend and its underrun count, night mode, whether the car
is parked, and whether the phone currently has the microphone open.

**The overlay at the lower left** is the turn card, the call banner and the now playing
bar, all drawn from the metadata channels rather than from the video. They appear only
when the phone has something to say, so an idle head unit shows an empty corner.

**Overlay hit test** is a button that sits over the projection and counts its own taps,
which is how to tell that Flutter's hit testing and the projected touch mapping are not
fighting each other.

## The panels

Each button along the bottom opens a panel over the projection.

| Panel | What it exercises |
|---|---|
| **Audio** | Output and input device selection, per-stream volume and mute, "Play here" to hand the PCM to the app instead of playing it, and a tap that copies the raw PCM out for an app that wants to mix it itself |
| **Sensors** | Night mode, driving status, and a GPS fix fed from two text fields. A fixed point rather than a simulated drive: feeding a phone a route it is not on makes Maps recalculate all the way through a test |
| **Metadata** | Which of the five channels the phone opened and how many updates each has sent, the last notification received, and a media library browser. Browsing is one of the two channels the head unit has to speak first on, so it is the one place the API is driven rather than listened to |
| **Wireless** | Paired phones, the network the head unit would offer, and starting or stopping the wireless service independently of a session |

**Test pattern** drives the video path from a generated pattern with no phone attached,
which is how to lay out an overlay without hardware, and how to tell a video problem
from a presentation one.

## Things worth knowing

- `AA_AUTOSTART=1` makes the app press its own Start button, for tests driven from a
  script. That is an example app knob, not a plugin one.
- The plugin reads a few environment knobs, all off unless set, and they work here too.
  `AA_LOG_LEVEL=DEBUG` turns on aasdk's own protocol log and the service discovery
  exchange in full, `AA_SERVICES` narrows or widens the advertised channel set without a
  rebuild, `AA_TRANSPORTS` picks `usb`, `wireless` or both, and
  `AA_VIDEO_DECODER=software` forces the software decoder to tell a driver problem from
  a decoder problem.
- Sensors the app sets survive a stop and start, because a parking brake does not come
  off when a cable is pulled out. Metadata does not, because the music does stop.
