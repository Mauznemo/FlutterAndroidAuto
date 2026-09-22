# android_auto_example

The reference integration, and the test bench. It shows how a head unit is put together
with the plugin, and it exercises every channel the plugin implements: if something is
unclear in a doc comment, the call is in here with a button attached to it.

Run it from the repository root:

```bash
git submodule update --init --recursive
cd example && flutter run -d linux
```

Then plug a phone in and press **Start**.

## Where to start reading

| File | What |
|---|---|
| `lib/config.dart` | what the head unit tells the phone it is: sensors, metadata channels, transports, the Wi-Fi passphrase. Read once, when a phone connects |
| `lib/head_unit_page.dart` | one controller, one `AndroidAutoView`, a status bar above it and everything else stacked on top of it |
| `lib/head_unit/` | the pieces a real head unit would ship: the status bar, the hardware keys, and the turn card, call banner and now playing bar drawn from metadata |
| `lib/bench/` | the test bench: the dock along the bottom and the four panels. A product would not show these to a driver, but every call in them is the one a product would make |

## What is on screen

**The status bar along the top** is the head unit's own view of itself: connection
state, the video size and which decoder is running, the plugin's latest message, the
last key sent, the audio backend and its underrun count, night mode, whether the car is
parked, and whether the phone currently has the microphone open. It sits above the
projection rather than over it, so the phone lays itself out for the space below it.

**The overlay at the lower left** is the turn card, the call banner and the now playing
bar, all drawn from the metadata channels rather than from the video. They appear only
when the phone has something to say, so an idle head unit shows an empty corner.

**The dock along the bottom** has the hardware keys a car has on its dashboard or
steering wheel, starting and stopping the head unit, the test pattern and the panels. It
is drawn over the projection and hides behind the chevron, because whatever covers the
projection is part of the phone's screen the driver cannot touch. Hiding it does not
resize the view, which matters: every change of view size makes the phone lay itself
out again, and that freezes the video for about a second.

**Hit test** is a button over the projection that counts its own taps, which is how to
tell that Flutter's hit testing and the projected touch mapping are not fighting each
other.

## The panels

One at a time, top right, over the projection.

| Panel | What it exercises |
|---|---|
| **Audio** | Per stream volume and mute, output and microphone selection, "Play here" to hand the PCM to the app instead of playing it, and a tap that copies the raw PCM out for an app that wants to mix it itself |
| **Sensors** | Night mode, driving status, and a GPS fix fed from two text fields. A fixed point rather than a simulated drive: feeding a phone a route it is not on makes Maps recalculate all the way through a test |
| **Metadata** | Which of the five channels the phone opened and how many updates each has sent, the last notification received, and a media library browser. Browsing is one of the two channels the head unit has to speak first on, so it is the one place the API is driven rather than listened to |
| **Wireless** | Paired phones, the network the head unit would offer, and starting or stopping the wireless service independently of a session |

**Test pattern** drives the video path from a generated pattern with no phone attached,
which is how to lay out an overlay without hardware, and how to tell a video problem
from a presentation one.

## Things worth knowing

- The GPS feed runs from the moment the app starts, whether or not the Sensors panel is
  open. Location is advertised in `config.dart`, and a phone that is told the car has a
  receiver stops using its own. A head unit without one should take location out of
  the advertised set rather than copy the feed.
- `AA_AUTOSTART=1` makes the app press its own Start button, for tests driven from a
  script. That is an example app knob, not a plugin one.
- `AA_WIRELESS_PASSPHRASE` is where the example reads the Wi-Fi passphrase from. The
  Wireless panel shows it hidden and can change it for the next time wireless starts.
- The plugin reads a few environment knobs, all off unless set, and they work here too.
  `AA_LOG_LEVEL=DEBUG` turns on aasdk's own protocol log and the service discovery
  exchange in full, `AA_SERVICES` narrows or widens the advertised channel set without a
  rebuild, `AA_TRANSPORTS` picks `usb`, `wireless` or both, and
  `AA_VIDEO_DECODER=software` forces the software decoder to tell a driver problem from
  a decoder problem.
- Sensors the app sets survive a stop and start, because a parking brake does not come
  off when a cable is pulled out. Metadata does not, because the music does stop.
