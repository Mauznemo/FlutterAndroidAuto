## 0.0.1

Not released. The Linux implementation, built on a vendored and patched
[aasdk](https://github.com/opencardev/aasdk), verified on x86_64 against one phone.

- USB transport: AOAP accessory mode, SSL, service discovery, and recovery from a
  transport that dies while the phone stays enumerated.
- Wireless transport: a Bluetooth RFCOMM service, the handshake that tells the phone
  which network to join, and a TCP acceptor on port 5288.
- Video: H.264 decoded through VA-API into a dmabuf, imported as an `EGLImage` and
  converted on Flutter's raster thread, with a software fallback.
- Input: multi-touch following Android's `MotionEvent` rules, keys and a rotary
  controller, rate limited to one movement report per frame.
- Audio out: media, system and speech as three PulseAudio streams, mixed here, with
  media ducked under speech.
- Audio in: the microphone, opened only when the phone asks and closed when it lets go.
- Sensors and the five metadata channels, two of which aasdk names but does not speak.
- ARM64 is intended and not yet verified. Android is a separate package that does not
  exist yet.
