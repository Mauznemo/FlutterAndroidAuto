// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';
import 'dart:math' as math;

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/widgets.dart';

import 'simulated_phone.dart';
import 'simulated_screen.dart';

/// A head unit with a pretend phone plugged in, for developing a host app on a machine
/// that cannot run the real one.
///
/// The real head unit only runs on Linux. Most host apps are written on macOS or
/// Windows, so this is what the `android_auto` package registers there: pure Dart, no
/// native code, no USB, no audio. Everything else about the app, the controller, the
/// view, the overlays and every metadata stream, runs exactly as it will in the car.
///
/// What it does, so the app has something honest to react to:
///
/// * [start] walks through `searching` and `handshaking` to `connected` in about two
///   seconds, as plugging a phone in would, and [stop] takes it back to `idle`. The
///   picture follows a moment after `connected`, as a real phone's first frame does,
///   so an app that hides its "connecting" screen too early finds out here.
/// * The projection is a drawn stand in for Android Auto: a rail of apps, a map, a
///   player and a dialler, laid out for the view's size the way a phone would lay
///   out. It is labelled as simulated on screen.
/// * Touches reach it through `sendTouch` in projected video pixels, exactly as they
///   reach a phone, so the view's touch mapping is exercised rather than bypassed.
///   Fingers and taps are drawn, which makes a mis-mapped touch obvious.
/// * Hardware keys work: media keys drive the player, [AndroidAutoKey.microphone]
///   opens the Assistant for a few seconds, [AndroidAutoKey.home] opens the launcher.
/// * Starting the route on the map sends turn by turn guidance on [navigation], the
///   player sends [mediaPlayback] with generated cover art, and the dialler sends
///   [phoneStatus], including an incoming call on demand. Each only when its channel
///   is in [AndroidAutoConfig.metadata], as with a real phone.
/// * Night mode switches the map to its dark colours.
///
/// Nothing is ever played or recorded, the Bluetooth and Wi-Fi calls do nothing, and
/// no sensor value goes anywhere, though each is remembered and read back.
///
/// Registered for macOS and Windows automatically. On Linux the real head unit is
/// registered instead, and a host app that wants the simulator there too, to work
/// without a phone to hand, calls [registerWith] itself before creating its
/// `AndroidAutoController`.
class AndroidAutoSimulator extends AndroidAutoPlatform {
  /// How long [start] spends in `searching` before the pretend phone is found.
  final Duration searchTime;

  /// How long the pretend handshake takes.
  final Duration handshakeTime;

  /// How long after connecting the pretend phone's picture appears, see [hasVideo].
  final Duration firstFrameTime;

  final _events = StreamController<AndroidAutoEvent>.broadcast();
  final _navigation = StreamController<AndroidAutoNavigation>.broadcast();
  final _mediaPlayback = StreamController<AndroidAutoMediaInfo>.broadcast();
  final _phoneStatus = StreamController<AndroidAutoPhoneStatus>.broadcast();
  late final SimulatedPhone _phone = SimulatedPhone(
    onNavigation: _onNavigation,
    onMedia: _onMedia,
    onPhoneStatus: _onPhoneStatus,
  );

  AndroidAutoConfig _config = const AndroidAutoConfig();
  AndroidAutoConnectionState _state = AndroidAutoConnectionState.idle;
  final List<Timer> _pending = [];
  Timer? _relayout;
  bool _testPattern = false;
  bool _streaming = false;

  Size _view = Size.zero;
  Size? _frame;
  Size? _picture;

  AndroidAutoNavigation? _lastNavigation;
  AndroidAutoMediaInfo? _lastMediaInfo;
  AndroidAutoPhoneStatus? _lastPhoneStatus;
  final Map<AndroidAutoMetadata, int> _metadataUpdates = {};

  final Map<AndroidAutoAudioStream, double> _volumes = {};
  final Map<AndroidAutoAudioStream, bool> _muted = {};
  String _audioDevice = '';
  bool _audioOutputEnabled = true;
  String _microphoneDevice = '';

  bool _wirelessActive = false;
  Set<AndroidAutoDrivingRestriction> _restrictions = const {};
  AndroidAutoLocation? _location;
  int _sensorBatches = 0;

  /// Creates a simulator. The delays are there to make the connection sequence
  /// visible; a test can set them to zero.
  AndroidAutoSimulator({
    this.searchTime = const Duration(milliseconds: 1200),
    this.handshakeTime = const Duration(milliseconds: 800),
    this.firstFrameTime = const Duration(milliseconds: 600),
  });

  /// Called by the Flutter tooling through `dartPluginClass` on macOS and Windows.
  static void registerWith() {
    AndroidAutoPlatform.instance = AndroidAutoSimulator();
  }

  /// The simulator in use, or null when a real head unit is.
  ///
  /// For a host app that wants to drive the pretend phone from its own debug UI, or
  /// show that it is running on one.
  static AndroidAutoSimulator? get current {
    try {
      final instance = AndroidAutoPlatform.instance;
      return instance is AndroidAutoSimulator ? instance : null;
    } on UnsupportedError {
      return null;
    }
  }

  /// Starts guidance on the pretend phone, as tapping Start on its map does.
  void startRoute() => _phone.startRoute();

  /// Ends guidance.
  void stopRoute() => _phone.stopRoute();

  /// Makes the pretend phone ring.
  void simulateIncomingCall() => _phone.simulateIncomingCall();

  /// Ends or declines the pretend call.
  void hangUp() => _phone.hangUp();

  bool get _connected => _state == AndroidAutoConnectionState.connected;

  // === lifecycle ===

  @override
  Future<void> initialize(AndroidAutoConfig config) async {
    _config = config;
  }

  @override
  Future<void> start(AndroidAutoConfig config) async {
    _config = config;
    if (_state != AndroidAutoConnectionState.idle &&
        _state != AndroidAutoConnectionState.error) {
      return;
    }
    _emit(AndroidAutoConnectionState.searching, 'Simulated: waiting for a phone');
    _after(searchTime, () {
      _emit(AndroidAutoConnectionState.handshaking, 'Simulated: phone found');
      _after(handshakeTime, _connect);
    });
  }

  void _connect() {
    final width = _config.width;
    final height = _config.height;
    _frame = width != null && height != null
        ? Size(width.toDouble(), height.toDouble())
        : frameForView(_view, _config.matchViewAspectRatio);
    _picture = _pictureFor(_frame!);
    _phone.night = nightMode;
    _state = AndroidAutoConnectionState.connected;
    _phone.connect();
    _emit(AndroidAutoConnectionState.connected, 'Simulated phone projecting at $_size');
    _after(firstFrameTime, () {
      _streaming = true;
      _emit(AndroidAutoConnectionState.connected, 'Simulated: first frame');
    });
  }

  @override
  Future<void> stop() async {
    for (final timer in _pending) {
      timer.cancel();
    }
    _pending.clear();
    _relayout?.cancel();
    _testPattern = false;
    _streaming = false;
    _phone.disconnect();
    _lastNavigation = null;
    _lastMediaInfo = null;
    _lastPhoneStatus = null;
    _frame = null;
    _picture = null;
    _emit(AndroidAutoConnectionState.idle);
  }

  @override
  Future<void> dispose() async {
    await stop();
    _phone.dispose();
    await _events.close();
    await _navigation.close();
    await _mediaPlayback.close();
    await _phoneStatus.close();
  }

  @override
  Stream<AndroidAutoEvent> get events => _events.stream;

  void _emit(AndroidAutoConnectionState state, [String? message]) {
    _state = state;
    if (!_events.isClosed) {
      _events.add(AndroidAutoEvent(state, message));
    }
  }

  void _after(Duration delay, VoidCallback action) {
    late final Timer timer;
    timer = Timer(delay, () {
      _pending.remove(timer);
      action();
    });
    _pending.add(timer);
  }

  // === video ===

  /// Any id will do: nothing looks it up, [buildProjection] draws the screen instead.
  static const int _textureId = 0;

  @override
  Future<int?> get textureId async =>
      _connected || _testPattern ? _textureId : null;

  @override
  Future<bool> get hasVideo async => _connected ? _streaming : _testPattern;

  @override
  Future<AndroidAutoVideoInfo?> get videoInfo async {
    final picture = _connected
        ? (_streaming ? _picture : null)
        : (_testPattern ? _patternSize : null);
    if (picture == null) {
      return null;
    }
    return AndroidAutoVideoInfo(
      width: picture.width.round(),
      height: picture.height.round(),
      decoder: 'simulated',
    );
  }

  String get _size => '${_picture!.width.round()}x${_picture!.height.round()}';

  Size get _patternSize =>
      _pictureFor(frameForView(_view, _config.matchViewAspectRatio));

  @override
  Widget buildProjection(BuildContext context, int textureId) {
    final picture = _picture;
    if (!_connected || picture == null) {
      return const SimulatedTestPattern();
    }
    return SimulatedScreen(phone: _phone, picture: picture, dpi: _config.dpi);
  }

  @override
  void setViewSize(double width, double height) {
    _view = Size(width, height);
    final frame = _frame;
    if (!_connected || frame == null || !_config.matchViewAspectRatio) {
      return;
    }
    final next = _pictureFor(frame);
    if (next == _picture) {
      _relayout?.cancel();
      return;
    }
    // A real phone is asked to lay out again once the view has held still, and its
    // picture changes size when it has. The frame itself is fixed for the connection.
    _relayout?.cancel();
    _relayout = Timer(const Duration(milliseconds: 400), () {
      if (!_connected) {
        return;
      }
      _picture = next;
      _emit(AndroidAutoConnectionState.connected, 'Simulated phone laid out at $_size');
    });
  }

  @override
  Future<void> startTestPattern() async {
    if (_connected) {
      return;
    }
    _testPattern = true;
  }

  @override
  Future<void> stopTestPattern() async {
    if (!_testPattern) {
      return;
    }
    _testPattern = false;
    // Said so the controller looks at the texture again and finds it gone.
    _emit(_state);
  }

  /// The part of [frame] the phone draws in, for the view as it is now.
  Size _pictureFor(Size frame) {
    if (!_config.matchViewAspectRatio || _view.isEmpty) {
      return frame;
    }
    return visibleForView(frame, _view);
  }

  /// The smallest frame whose picture covers [view]. A copy of `FrameSizeForView` in
  /// the Linux implementation's `video_margins.cc`, so the simulated picture is the
  /// size the real one would be.
  @visibleForTesting
  static Size frameForView(Size view, bool matchView) {
    const candidates = [Size(800, 480), Size(1280, 720), Size(1920, 1080)];
    if (view.isEmpty || !view.isFinite) {
      return const Size(1280, 720);
    }
    for (final frame in candidates) {
      final visible = matchView ? visibleForView(frame, view) : frame;
      final scale = math.min(
        view.width / visible.width,
        view.height / visible.height,
      );
      if (scale <= 1.0 + 1.5 / math.min(visible.width, visible.height)) {
        return frame;
      }
    }
    return candidates.last;
  }

  /// The part of [frame] left once the margins for [view] are taken off. A copy of
  /// `MarginsForView` in the same file.
  @visibleForTesting
  static Size visibleForView(Size frame, Size view) {
    if (view.isEmpty || !view.isFinite) {
      return frame;
    }
    var scale = math.min(
      1.0,
      math.min(frame.width / view.width, frame.height / view.height),
    );
    scale = math.max(
      scale,
      math.max(frame.width / 3 / view.width, frame.height / 3 / view.height),
    );
    double even(double value) => ((value / 2).floor() * 2).toDouble();
    return Size(
      even(view.width * scale).clamp(2, frame.width),
      even(view.height * scale).clamp(2, frame.height),
    );
  }

  // === input ===

  @override
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) {
    if (!_connected || pointers.isEmpty) {
      return;
    }
    final scale = SimulatedScreen.layoutScale(_config.dpi);
    final fingers = {
      for (final point in pointers)
        point.id: Offset(point.x * scale, point.y * scale),
    };
    final subject = pointers[actionIndex.clamp(0, pointers.length - 1)].id;
    _phone.touch(action, fingers, subject);
  }

  @override
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) {
    if (down) {
      _phone.key(key);
    }
  }

  // === audio, which is silent ===

  @override
  double audioVolume(AndroidAutoAudioStream stream) => _volumes[stream] ?? 1.0;

  @override
  void setAudioVolume(AndroidAutoAudioStream stream, double volume) =>
      _volumes[stream] = volume.clamp(0.0, 1.0);

  @override
  bool audioMuted(AndroidAutoAudioStream stream) => _muted[stream] ?? false;

  @override
  void setAudioMuted(AndroidAutoAudioStream stream, bool muted) =>
      _muted[stream] = muted;

  @override
  String get audioDevice => _audioDevice;

  @override
  void setAudioDevice(String? name) => _audioDevice = name ?? '';

  @override
  bool get audioOutputEnabled => _audioOutputEnabled;

  @override
  void setAudioOutputEnabled(bool enabled) => _audioOutputEnabled = enabled;

  @override
  String get audioBackend => 'simulated, silent';

  @override
  bool get microphoneActive => _phone.listening;

  @override
  double get microphoneLevel => _phone.microphoneLevel;

  @override
  String get microphoneDevice => _microphoneDevice;

  @override
  void setMicrophoneDevice(String? name) => _microphoneDevice = name ?? '';

  @override
  String get microphoneBackend => 'simulated, records nothing';

  // === wireless, which does nothing ===

  @override
  Future<void> startWireless() async => _wirelessActive = true;

  @override
  Future<void> stopWireless() async => _wirelessActive = false;

  @override
  bool get wirelessActive => _wirelessActive;

  // === sensors, remembered and otherwise ignored ===

  @override
  bool get nightMode => _phone.night;

  @override
  void setNightMode(bool night) {
    _phone.night = night;
    _countSensor();
  }

  @override
  Set<AndroidAutoDrivingRestriction> get drivingRestrictions => _restrictions;

  @override
  void setDrivingRestrictions(Set<AndroidAutoDrivingRestriction> restrictions) {
    _restrictions = Set.unmodifiable(restrictions);
    _countSensor();
  }

  @override
  AndroidAutoLocation? get location => _location;

  @override
  void setLocation(AndroidAutoLocation location) {
    _location = location;
    _countSensor();
  }

  @override
  void setSpeed(double metresPerSecond) => _countSensor();

  @override
  void setRpm(double rpm) => _countSensor();

  @override
  void setFuel({
    required double levelPercent,
    required double rangeMetres,
    bool low = false,
  }) => _countSensor();

  @override
  void setParkingBrake(bool engaged) => _countSensor();

  @override
  void setGear(int gear) => _countSensor();

  @override
  void setCompass(double bearingDegrees) => _countSensor();

  @override
  void setEnvironment({double? temperatureCelsius, double? pressureKpa}) =>
      _countSensor();

  @override
  void setOdometer(double kilometres) => _countSensor();

  @override
  void setTollCard(bool present) => _countSensor();

  void _countSensor() {
    if (_connected) {
      _sensorBatches++;
    }
  }

  /// The pretend phone takes everything it is offered.
  @override
  Set<AndroidAutoSensor> get sensorSubscriptions =>
      _connected ? _config.sensors : const <AndroidAutoSensor>{};

  @override
  int get sensorBatches => _sensorBatches;

  // === metadata ===

  /// The three the phone pushes unprompted, which are also the three a real phone
  /// has been seen to open.
  static const _pushed = {
    AndroidAutoMetadata.navigation,
    AndroidAutoMetadata.media,
    AndroidAutoMetadata.phone,
  };

  @override
  Set<AndroidAutoMetadata> get metadataChannels => _connected
      ? _config.metadata.intersection(_pushed)
      : const <AndroidAutoMetadata>{};

  @override
  int metadataUpdates(AndroidAutoMetadata kind) => _metadataUpdates[kind] ?? 0;

  bool _offered(AndroidAutoMetadata kind) {
    if (!_config.metadata.contains(kind)) {
      return false;
    }
    _metadataUpdates[kind] = metadataUpdates(kind) + 1;
    return true;
  }

  void _onNavigation(AndroidAutoNavigation navigation) {
    if (_offered(AndroidAutoMetadata.navigation) && !_navigation.isClosed) {
      _lastNavigation = navigation;
      _navigation.add(navigation);
    }
  }

  void _onMedia(AndroidAutoMediaInfo media) {
    if (_offered(AndroidAutoMetadata.media) && !_mediaPlayback.isClosed) {
      _lastMediaInfo = media;
      _mediaPlayback.add(media);
    }
  }

  void _onPhoneStatus(AndroidAutoPhoneStatus status) {
    if (_offered(AndroidAutoMetadata.phone) && !_phoneStatus.isClosed) {
      _lastPhoneStatus = status;
      _phoneStatus.add(status);
    }
  }

  @override
  Stream<AndroidAutoNavigation> get navigation => _navigation.stream;

  @override
  AndroidAutoNavigation? get lastNavigation => _lastNavigation;

  @override
  Stream<AndroidAutoMediaInfo> get mediaPlayback => _mediaPlayback.stream;

  @override
  AndroidAutoMediaInfo? get lastMediaInfo => _lastMediaInfo;

  @override
  Stream<AndroidAutoPhoneStatus> get phoneStatus => _phoneStatus.stream;

  @override
  AndroidAutoPhoneStatus? get lastPhoneStatus => _lastPhoneStatus;
}
