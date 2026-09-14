import 'dart:async';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/foundation.dart';

/// Starts, stops and observes a head unit session.
///
/// A host app normally creates one of these for the lifetime of the app and hands it
/// to an [AndroidAutoView].
class AndroidAutoController extends ChangeNotifier {
  /// How the head unit describes itself to the phone.
  final AndroidAutoConfig config;

  late final StreamSubscription<AndroidAutoEvent> _subscription;
  AndroidAutoConnectionState _state = AndroidAutoConnectionState.idle;
  String? _message;
  int? _textureId;
  AndroidAutoVideoInfo? _videoInfo;

  /// Creates a controller. Nothing happens until [start] is called.
  AndroidAutoController({this.config = const AndroidAutoConfig()}) {
    _subscription = _platform.events.listen(_onEvent);
  }

  AndroidAutoPlatform get _platform => AndroidAutoPlatform.instance;

  /// Lifecycle and error events from the native session.
  Stream<AndroidAutoEvent> get events => _platform.events;

  /// Where the session is in its lifecycle.
  AndroidAutoConnectionState get state => _state;

  /// Detail for the current [state], typically only set when it is
  /// [AndroidAutoConnectionState.error].
  String? get message => _message;

  /// Id of the texture carrying the projected video, or null while there is none.
  int? get textureId => _textureId;

  /// The size and decoder of the incoming video, or null before the first frame.
  ///
  /// Prefer this over [config] when laying the projection out: the config is what was
  /// asked for, this is what arrived.
  AndroidAutoVideoInfo? get videoInfo => _videoInfo;

  /// Begins looking for a phone.
  Future<void> start() async {
    await _platform.start(config);
    await _refreshVideoState();
  }

  /// Ends the session.
  Future<void> stop() async {
    await _platform.stop();
    _textureId = null;
    _videoInfo = null;
    notifyListeners();
  }

  /// Drives the video path from a generated pattern instead of a phone.
  ///
  /// Useful for laying out an overlay before any hardware is involved.
  Future<void> startTestPattern() async {
    await _platform.startTestPattern();
    await _refreshVideoState();
  }

  /// Stops the pattern started by [startTestPattern].
  Future<void> stopTestPattern() => _platform.stopTestPattern();

  /// Reports a touch to the phone.
  ///
  /// [AndroidAutoView] calls this for every pointer event over the projection, so an
  /// app that uses the view has nothing to do here. Calling it directly is for head
  /// units with a real touchscreen wired up some other way, and it is then the
  /// caller's job to put the coordinates in projected video pixels: see
  /// [AndroidAutoTouchPoint].
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) => _platform.sendTouch(action, pointers, actionIndex: actionIndex);

  /// Presses and releases a hardware key.
  ///
  /// This is how a head unit's steering wheel and dashboard buttons reach the phone.
  void pressKey(AndroidAutoKey key) => _platform.pressKey(key);

  /// Reports one half of a key press, for a button that can be held.
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) =>
      _platform.sendKey(key, down: down, longPress: longPress);

  /// Reports rotary encoder movement, in detents, positive clockwise.
  ///
  /// Pair it with [pressKey] and [AndroidAutoKey.enter] for the encoder's push.
  void sendRotary(int steps) => _platform.sendRotary(steps);

  /// Playback volume of one audio stream, 0.0 to 1.0.
  double volume(AndroidAutoAudioStream stream) => _platform.audioVolume(stream);

  /// Sets the playback volume of one audio stream.
  ///
  /// The three streams are separate because the phone sends them separately. Media is
  /// ducked automatically while speech is playing, so an app does not have to do that
  /// itself.
  void setVolume(AndroidAutoAudioStream stream, double volume) {
    _platform.setAudioVolume(stream, volume);
    notifyListeners();
  }

  /// Whether one audio stream is muted.
  bool muted(AndroidAutoAudioStream stream) => _platform.audioMuted(stream);

  /// Mutes or unmutes one audio stream.
  void setMuted(AndroidAutoAudioStream stream, bool muted) {
    _platform.setAudioMuted(stream, muted);
    notifyListeners();
  }

  /// The audio outputs this machine offers, for presenting a picker.
  Future<List<AndroidAutoAudioDevice>> audioDevices() => _platform.audioDevices();

  /// The selected output's [AndroidAutoAudioDevice.name], or empty for the default.
  String get audioDevice => _platform.audioDevice;

  /// Chooses the audio output. Null or empty means the system default.
  void setAudioDevice(String? name) {
    _platform.setAudioDevice(name);
    notifyListeners();
  }

  /// Whether the plugin plays the phone's audio itself.
  bool get audioOutputEnabled => _platform.audioOutputEnabled;

  /// Turns the plugin's own playback off without touching the protocol.
  ///
  /// For an infotainment system that routes audio through its own amplifier, or an app
  /// that mixes [audioBuffers] itself.
  void setAudioOutputEnabled(bool enabled) {
    _platform.setAudioOutputEnabled(enabled);
    notifyListeners();
  }

  /// Which audio backend is playing: `PulseAudio`, or `none` before the first buffer.
  String get audioBackend => _platform.audioBackend;

  /// Times a stream came close to running the speakers dry since the session started.
  /// Should stay at zero.
  int audioUnderruns(AndroidAutoAudioStream stream) =>
      _platform.audioUnderruns(stream);

  /// Buffers thrown away because the phone sent faster than they could be played.
  int audioDropped(AndroidAutoAudioStream stream) => _platform.audioDropped(stream);

  /// How far behind the head unit the speakers are.
  Duration audioLatency(AndroidAutoAudioStream stream) =>
      _platform.audioLatency(stream);

  /// The phone's PCM, before this head unit touches it.
  ///
  /// Only worth listening to for an app that mixes the audio itself, which should pair
  /// it with `setAudioOutputEnabled(false)`. Nothing is delivered while nothing is
  /// listening.
  Stream<AndroidAutoAudioBuffer> get audioBuffers => _platform.audioBuffers;

  /// Re-reads the texture id and the incoming video description from the platform.
  Future<void> _refreshVideoState() async {
    final id = await _platform.textureId;
    final info = await _platform.videoInfo;
    if (id != _textureId || info != _videoInfo) {
      _textureId = id;
      _videoInfo = info;
      notifyListeners();
    }
  }

  void _onEvent(AndroidAutoEvent event) {
    _state = event.state;
    _message = event.message;
    notifyListeners();
    // The texture is registered natively the moment video starts, so an event is the
    // earliest point at which asking for the id is worthwhile. The video size arrives
    // later still, on the event the decoder raises once it has a frame.
    _refreshVideoState();
  }

  @override
  void dispose() {
    _subscription.cancel();
    _platform.dispose();
    super.dispose();
  }
}
