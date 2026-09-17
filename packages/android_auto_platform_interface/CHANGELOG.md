## 0.0.1

Not released. The interface as it stands after the Linux implementation was built
against it.

- `AndroidAutoPlatform`, the contract every implementation answers.
- `AndroidAutoConfig`: projected size, frame rate, density, how the head unit describes
  itself, and which sensors, metadata channels and transports it offers.
- Connection state and events, video information, and touch, key and rotary input.
- Audio: three streams with volume, mute, device selection and statistics, plus the
  microphone and its own device selection.
- Sensors: night mode, driving status, location, speed, gear, parking brake, compass,
  fuel, rpm, environment, odometer and toll card.
- Metadata: navigation, media playback, telephony, notifications and media browsing.
- Wireless: configuration, status and the paired device list.
