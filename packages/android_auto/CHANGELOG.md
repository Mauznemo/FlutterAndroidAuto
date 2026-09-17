## 0.0.1

Not released. What the app facing API offers today.

- `AndroidAutoController`: start and stop a session, over the cable or over Wi-Fi, and
  follow it through `events` and `state`.
- `AndroidAutoView`: the projected screen as an ordinary widget, with letterboxing and
  touch mapping handled, so widgets composite on top of it and taps land where they look.
- Sensors the host app reports to the phone, and audio volume, mute and device selection
  for the three streams.
- Streams of what the phone is doing, for drawing the car's own interface rather than
  mirroring pixels: `navigation`, `mediaPlayback`, `phoneStatus`, `notifications` and
  media browsing.
- `startTestPattern`, for laying an overlay out with no phone and no cable.
