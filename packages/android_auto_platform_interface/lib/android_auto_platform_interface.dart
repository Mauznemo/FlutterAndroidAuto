/// Platform interface for the `android_auto` plugin.
///
/// Contains no native code and no GPL code, so a future permissive implementation
/// can replace `android_auto_linux` without any host app change. See
/// `docs/architecture.md`.
library;

import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

/// Where a head unit session is in its lifecycle.
enum AndroidAutoConnectionState {
  /// Nothing is running.
  idle,

  /// Looking for a phone on USB, or waiting for one to connect over Wi-Fi.
  searching,

  /// A phone was found, the transport and SSL handshake are in progress.
  handshaking,

  /// Channels are open and the phone is projecting.
  connected,

  /// The session ended because of an error. See [AndroidAutoEvent.message].
  error,
}

/// How the head unit describes itself to the phone during service discovery.
///
/// The values matter: the phone picks its layout and video encoding from them, and
/// some of them show up in the phone's own UI.
class AndroidAutoConfig {
  /// Projected surface width in pixels. One of 800, 1280 or 1920 in practice.
  final int width;

  /// Projected surface height in pixels. One of 480, 720 or 1080 in practice.
  final int height;

  /// Target frame rate the head unit advertises. 30 or 60.
  final int fps;

  /// Screen density the phone should lay out for.
  final int dpi;

  /// Shown on the phone while Android Auto is active.
  final String headUnitName;

  /// Vehicle make, model and year, reported during service discovery.
  final String carModel;

  /// Vehicle model year, reported during service discovery.
  final String carYear;

  /// Overrides the bundled head unit certificate and key. Point this at a directory
  /// holding `headunit.crt` and `headunit.key`.
  final String? certificatePath;

  /// Creates a head unit description. The defaults are a safe 720p30 head unit
  /// that every phone accepts.
  const AndroidAutoConfig({
    this.width = 1280,
    this.height = 720,
    this.fps = 30,
    this.dpi = 140,
    this.headUnitName = 'Flutter Head Unit',
    this.carModel = 'Universal',
    this.carYear = '2026',
    this.certificatePath,
  });
}

/// What the phone is actually sending, once video is flowing.
///
/// Distinct from [AndroidAutoConfig] on purpose. The config is what the head unit asked
/// for; this is what turned up. The phone chooses from the video configurations the
/// head unit advertised, and it is allowed to change its mind mid session, so a host
/// app that lays out from the config alone will letterbox the projection wrongly the
/// first time a phone does something unexpected.
class AndroidAutoVideoInfo {
  /// Width of the decoded video in pixels.
  final int width;

  /// Height of the decoded video in pixels.
  final int height;

  /// Which decoder is doing the work: `VA-API`, `software`, or `none` before the first
  /// frame has been decoded.
  final String decoder;

  /// Creates a description of the incoming video stream.
  const AndroidAutoVideoInfo({
    required this.width,
    required this.height,
    required this.decoder,
  });

  /// Width divided by height, for laying the projection out.
  double get aspectRatio => height == 0 ? 0 : width / height;

  @override
  bool operator ==(Object other) =>
      other is AndroidAutoVideoInfo &&
      other.width == width &&
      other.height == height &&
      other.decoder == decoder;

  @override
  int get hashCode => Object.hash(width, height, decoder);

  @override
  String toString() => 'AndroidAutoVideoInfo($width x $height, $decoder)';
}

/// What a touch report describes.
///
/// The names and the order come from Android's own `MotionEvent`, which is what the
/// protocol carries, so the head unit has to follow the same rules Android does: the
/// first finger down is [down] and the last one up is [up], while the ones in between
/// are [pointerDown] and [pointerUp].
enum AndroidAutoTouchAction {
  /// The first finger touched the screen.
  down,

  /// The last finger left the screen.
  up,

  /// At least one finger moved.
  move,

  /// Another finger touched a screen that was already being touched.
  pointerDown,

  /// One of several fingers left the screen, and others are still down.
  pointerUp,
}

/// One finger in a touch report.
///
/// [x] and [y] are in **projected video pixels**, not logical pixels and not
/// normalised. The head unit tells the phone it has a touchscreen exactly the size of
/// the video it asked for, and the phone reads these against that, so a widget local
/// position has to be mapped through however the projection is fitted on screen first.
/// [AndroidAutoView] does that; an app sending its own touches has to do it itself.
class AndroidAutoTouchPoint {
  /// Identifies this finger for as long as it stays down. Small indices, the way
  /// Android numbers pointers, rather than an ever growing counter.
  final int id;

  /// Horizontal position in projected video pixels.
  final int x;

  /// Vertical position in projected video pixels.
  final int y;

  /// Creates one finger of a touch report.
  const AndroidAutoTouchPoint({required this.id, required this.x, required this.y});

  @override
  String toString() => 'AndroidAutoTouchPoint(#$id at $x,$y)';
}

/// A hardware key a head unit can report.
///
/// These are the keys advertised during service discovery, which is a promise that the
/// head unit can produce every one of them rather than a request to receive them. A
/// phone may bind any of them and route it itself. Adding to this list means adding to
/// `SupportedKeycodes()` in the Linux implementation as well, otherwise the phone is
/// sent a key it was never told about.
enum AndroidAutoKey {
  /// Go back one screen.
  back(4),

  /// Return to the Android Auto home screen.
  home(3),

  /// Answer a call, or open the dialler.
  call(5),

  /// Hang up.
  endCall(6),

  /// Toggle playback.
  playPause(85),

  /// Start playback.
  play(126),

  /// Pause playback.
  pause(127),

  /// Skip to the next track.
  next(87),

  /// Skip to the previous track.
  previous(88),

  /// Start the Assistant. The protocol calls this SEARCH; on a head unit it is the
  /// microphone button, not a text search.
  microphone(84),

  /// Move the selection up, on a head unit with a D-pad.
  up(19),

  /// Move the selection down.
  down(20),

  /// Move the selection left.
  left(21),

  /// Move the selection right.
  right(22),

  /// Activate the selection. This is also the rotary encoder's push.
  enter(23);

  /// The protocol's keycode, which is Android's `KeyEvent` code.
  final int code;

  const AndroidAutoKey(this.code);
}

/// One of the three audio streams a head unit plays.
///
/// Android Auto sends these separately and leaves the mixing to the head unit, which is
/// why they have separate volumes rather than one. It is also why the head unit is the
/// one that has to duck: by the time the music and the navigation prompt leave the
/// phone they are already on different channels, so nothing on the phone can turn one
/// down under the other.
enum AndroidAutoAudioStream {
  /// Music, podcasts, anything the phone calls media. 48 kHz stereo.
  ///
  /// Turned down automatically while [speech] is playing, and back up afterwards.
  media,

  /// Interface and notification sounds. 16 kHz mono.
  system,

  /// Navigation prompts and the Assistant. 16 kHz mono.
  speech,
}

/// An audio device the machine can play through or listen with.
///
/// An infotainment system often routes audio itself, through an amplifier the head unit
/// does not control, and its microphone is rarely the one the operating system would
/// pick by default, so choosing both is the host app's business rather than something
/// this plugin should decide. The same class describes an output and an input; which one
/// it is depends on whether it came from [AndroidAutoPlatform.audioDevices] or
/// [AndroidAutoPlatform.microphoneDevices].
class AndroidAutoAudioDevice {
  /// What to hand to [AndroidAutoPlatform.setAudioDevice] or
  /// [AndroidAutoPlatform.setMicrophoneDevice]. Stable, not human friendly.
  final String name;

  /// What to show a person, such as `Built-in Audio Analogue Stereo`.
  final String description;

  /// Whether the audio server would have picked this one anyway.
  final bool isDefault;

  /// Creates a description of one device.
  const AndroidAutoAudioDevice({
    required this.name,
    required this.description,
    this.isDefault = false,
  });

  @override
  bool operator ==(Object other) =>
      other is AndroidAutoAudioDevice &&
      other.name == name &&
      other.description == description &&
      other.isDefault == isDefault;

  @override
  int get hashCode => Object.hash(name, description, isDefault);

  @override
  String toString() => 'AndroidAutoAudioDevice($description)';
}

/// One buffer of PCM, exactly as the phone sent it.
///
/// Delivered to apps that want to mix the audio themselves. It is the raw stream,
/// before this head unit's volume or ducking, so an app reading these should also turn
/// the built in output off with
/// [AndroidAutoPlatform.setAudioOutputEnabled] or it will hear both.
class AndroidAutoAudioBuffer {
  /// Which of the three streams this belongs to.
  final AndroidAutoAudioStream stream;

  /// Samples per second, 48000 for media and 16000 for the other two.
  final int sampleRate;

  /// 2 for media, 1 for the other two.
  final int channels;

  /// Signed 16 bit little endian samples, interleaved when there is more than one
  /// channel. Owned by the caller, and a fresh copy per buffer.
  final Uint8List samples;

  /// Creates one buffer of raw PCM.
  const AndroidAutoAudioBuffer({
    required this.stream,
    required this.sampleRate,
    required this.channels,
    required this.samples,
  });

  /// How long this buffer takes to play.
  Duration get duration => Duration(
    microseconds:
        sampleRate == 0 || channels == 0
            ? 0
            : samples.lengthInBytes * 1000000 ~/ (sampleRate * channels * 2),
  );

  @override
  String toString() =>
      'AndroidAutoAudioBuffer(${stream.name}, ${samples.lengthInBytes} bytes)';
}

/// Something the native session wants the Dart side to know about.
class AndroidAutoEvent {
  /// The lifecycle state the session moved into.
  final AndroidAutoConnectionState state;

  /// Human readable detail, set when [state] is
  /// [AndroidAutoConnectionState.error].
  final String? message;

  /// Creates an event describing a session state change.
  const AndroidAutoEvent(this.state, [this.message]);
}

/// The contract every platform implementation fulfils.
///
/// Implementations register themselves by assigning to [instance] from their
/// `registerWith` entry point.
abstract class AndroidAutoPlatform extends PlatformInterface {
  /// Subclasses must call this so [PlatformInterface] can verify them.
  AndroidAutoPlatform() : super(token: _token);

  static final Object _token = Object();

  static AndroidAutoPlatform? _instance;

  /// The implementation registered for the current platform.
  ///
  /// Throws if the platform has no implementation, rather than silently no-opping,
  /// because a head unit that quietly does nothing is worse than one that fails loudly.
  static AndroidAutoPlatform get instance {
    final instance = _instance;
    if (instance == null) {
      throw UnsupportedError(
        'android_auto has no implementation for this platform yet. '
        'Only Linux is supported today.',
      );
    }
    return instance;
  }

  /// Registers [instance] as the implementation for the current platform.
  static set instance(AndroidAutoPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// Starts looking for a phone. Completes once the session is running, not once a
  /// phone has actually connected. Watch [events] for that.
  Future<void> start(AndroidAutoConfig config);

  /// Tears the session down and releases the transport.
  Future<void> stop();

  /// Lifecycle and error events from the native session.
  Stream<AndroidAutoEvent> get events;

  /// Id of the texture the projected video is rendered into, or null while there is
  /// no video stream.
  Future<int?> get textureId;

  /// The size and decoder of the incoming video, or null before the first frame.
  Future<AndroidAutoVideoInfo?> get videoInfo async => null;

  /// Feeds the video path from a generated pattern instead of a phone.
  ///
  /// Exists so the texture pipeline can be exercised without hardware, and so a host
  /// app can check its own overlay layout before a phone is ever plugged in. Does
  /// nothing once real video is running.
  Future<void> startTestPattern() async {}

  /// Stops the pattern started by [startTestPattern].
  Future<void> stopTestPattern() async {}

  /// Reports a touch to the phone.
  ///
  /// [pointers] carries every finger currently down, and [actionIndex] is the index
  /// within it of the finger this report is about, which only means anything for
  /// [AndroidAutoTouchAction.pointerDown] and [AndroidAutoTouchAction.pointerUp].
  ///
  /// Fire and forget, and synchronous on purpose: this is called straight out of a
  /// pointer callback, the protocol never acknowledges a report, and awaiting one
  /// would only add latency to a path that is measured in milliseconds. Does nothing
  /// while no phone is connected.
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) {}

  /// Reports one hardware key transition.
  ///
  /// Down and up are separate calls, as they are on Android: a phone that gets a down
  /// and no up believes the button is still held. Use [pressKey] for the common case.
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) {}

  /// Presses and releases [key].
  void pressKey(AndroidAutoKey key) {
    sendKey(key, down: true);
    sendKey(key, down: false);
  }

  /// Reports rotary encoder movement, in detents, positive clockwise.
  ///
  /// Sent as a relative axis rather than a key, which is what makes the phone scroll a
  /// list by steps instead of treating every detent as a button press.
  void sendRotary(int steps) {}

  /// Playback volume of one stream, 0.0 to 1.0.
  double audioVolume(AndroidAutoAudioStream stream) => 1.0;

  /// Sets the playback volume of one stream. Values outside 0.0 to 1.0 are clamped.
  ///
  /// Applied in software with a short ramp, so a change while music is playing is a
  /// fade rather than a click. Survives a phone disconnecting and reconnecting, because
  /// it describes the head unit rather than the phone.
  void setAudioVolume(AndroidAutoAudioStream stream, double volume) {}

  /// Whether one stream is muted.
  bool audioMuted(AndroidAutoAudioStream stream) => false;

  /// Mutes or unmutes one stream. A muted stream is played as silence rather than not
  /// played at all, so unmuting takes effect at once.
  void setAudioMuted(AndroidAutoAudioStream stream, bool muted) {}

  /// The outputs this machine offers, for a host app that wants to present a picker.
  ///
  /// Blocks briefly in the native layer, so it is a future. Empty when no audio server
  /// can be reached.
  Future<List<AndroidAutoAudioDevice>> audioDevices() async =>
      const <AndroidAutoAudioDevice>[];

  /// The selected output's [AndroidAutoAudioDevice.name], or empty for the default.
  String get audioDevice => '';

  /// Chooses the output to play through. Null or empty means the system default.
  void setAudioDevice(String? name) {}

  /// Whether the plugin is playing the phone's audio itself.
  bool get audioOutputEnabled => true;

  /// Turns the plugin's own playback off without touching the protocol.
  ///
  /// The phone keeps sending and [audioBuffers] keeps delivering, so this is what an
  /// app doing its own mixing, or an infotainment system routing audio through its own
  /// amplifier, turns off first.
  void setAudioOutputEnabled(bool enabled) {}

  /// Which audio backend is playing: `PulseAudio`, or `none` before the first buffer.
  String get audioBackend => 'none';

  /// Times this stream came close to running the speakers dry since the session
  /// started. Should stay at zero; anything else is an audible glitch.
  int audioUnderruns(AndroidAutoAudioStream stream) => 0;

  /// Buffers thrown away because the phone sent faster than they could be played.
  int audioDropped(AndroidAutoAudioStream stream) => 0;

  /// How far behind the head unit the speakers are.
  Duration audioLatency(AndroidAutoAudioStream stream) => Duration.zero;

  /// The phone's PCM, before this head unit touches it.
  ///
  /// Only worth listening to for an app that mixes the audio itself; the plugin plays
  /// these already. Listening costs a copy per buffer, so nothing is delivered until
  /// something is listening.
  Stream<AndroidAutoAudioBuffer> get audioBuffers =>
      const Stream<AndroidAutoAudioBuffer>.empty();

  /// Whether the phone has the head unit's microphone open right now.
  ///
  /// This is what a "listening" indicator shows, and it is the state of the device
  /// rather than of the channel: a phone that has opened the microphone channel but has
  /// not asked to record reads false. It goes true when the Assistant is invoked, by
  /// "Hey Google" or by [AndroidAutoKey.microphone], and false when it is finished.
  ///
  /// There is no call to turn it on. The phone asks, the head unit captures, and
  /// nothing else opens the device: a head unit whose host app could start recording
  /// would be a different and much worse thing than one that listens when asked.
  bool get microphoneActive => false;

  /// Peak level of the most recently captured buffer, 0.0 to 1.0, for a level meter.
  /// Zero while [microphoneActive] is false.
  double get microphoneLevel => 0.0;

  /// Bytes captured since the session was created.
  ///
  /// Worth showing next to [microphoneActive] while a head unit is being brought up: an
  /// Assistant that hears nothing looks the same whether the microphone is missing or
  /// the audio is not arriving, and this tells the two apart.
  int get microphoneBytes => 0;

  /// The inputs this machine offers, for a host app that wants to present a picker.
  ///
  /// Blocks briefly in the native layer, so it is a future. Empty when no audio server
  /// can be reached. Monitors of outputs are left out: they would let the head unit send
  /// the phone its own audio back.
  Future<List<AndroidAutoAudioDevice>> microphoneDevices() async =>
      const <AndroidAutoAudioDevice>[];

  /// The selected input's [AndroidAutoAudioDevice.name], or empty for the default.
  String get microphoneDevice => '';

  /// Chooses the input to capture from. Null or empty means the system default.
  ///
  /// Takes effect the next time the phone asks for the microphone, because that is the
  /// only moment the plugin is allowed to open one.
  void setMicrophoneDevice(String? name) {}

  /// Which capture backend is running: `PulseAudio`, or `none` before the first capture
  /// or on a machine with no microphone.
  String get microphoneBackend => 'none';

  /// Releases everything the implementation holds.
  ///
  /// Distinct from [stop]: a stopped session can be started again, a disposed one
  /// cannot. Host apps normally call this only when shutting down.
  Future<void> dispose() async {}
}
