/// Linux implementation of the `android_auto` plugin.
///
/// Links the native head unit core, which is built on aasdk and is therefore
/// GPL-3.0-or-later. See `docs/research.md` for what that means for host apps.
library;

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:ffi/ffi.dart';

import 'src/aa_library.dart';
import 'src/bindings/aa_core_bindings.dart';

/// Registers itself as the platform implementation on Linux.
///
/// Everything hot goes over FFI rather than a method channel. The plugin still has a
/// `pluginClass` registration on the native side, but only so it can capture Flutter's
/// texture registrar at startup.
class AndroidAutoLinux extends AndroidAutoPlatform {
  final _events = StreamController<AndroidAutoEvent>.broadcast();

  /// Audio settings live here rather than only in the core, so that a host app can set
  /// the volume or pick a microphone before it has ever started a session and have it
  /// apply when one exists. Everything in here is written through to the core whenever
  /// there is one, and re-applied to each new one.
  final Map<AndroidAutoAudioStream, double> _volumes = {
    for (final stream in AndroidAutoAudioStream.values) stream: 1.0,
  };
  final Map<AndroidAutoAudioStream, bool> _muted = {
    for (final stream in AndroidAutoAudioStream.values) stream: false,
  };
  String _audioDevice = '';
  bool _audioOutputEnabled = true;
  String _microphoneDevice = '';

  /// Which network to send a phone to, held here for the same reason the audio
  /// settings are: an app can set it before there is a session, and it has to be
  /// pushed into each new one.
  AndroidAutoWirelessConfig? _wireless;

  /// What the car is doing, held here for the same reason the audio settings are: a
  /// host app that sets the parking brake before it ever starts a session should not
  /// lose it, and the core only exists from [start] onwards. Replayed into each new
  /// session by [_applySensorSettings].
  ///
  /// Only the sensors the app has actually set are in here. A sensor with no entry is
  /// never pushed, so the core keeps its own distinction between a car that has no fix
  /// yet and a car at latitude zero.
  final Map<AndroidAutoSensor, void Function()> _sensorWrites = {};
  bool _nightMode = false;
  Set<AndroidAutoDrivingRestriction> _drivingRestrictions =
      const <AndroidAutoDrivingRestriction>{};
  AndroidAutoLocation? _location;

  AaCoreBindings get _bindings => AaLibrary.instance.bindings;

  /// What the phone has said about itself, one stream per kind plus the latest of the
  /// three that are states rather than events.
  ///
  /// Unlike the audio settings and the sensors these are not replayed into a new
  /// session: they came from a phone, and a phone that has been unplugged is not
  /// playing anything. [stop] clears them for that reason.
  final _navigation = StreamController<AndroidAutoNavigation>.broadcast();
  final _mediaPlayback = StreamController<AndroidAutoMediaInfo>.broadcast();
  final _phoneStatus = StreamController<AndroidAutoPhoneStatus>.broadcast();
  final _notifications = StreamController<AndroidAutoNotification>.broadcast();
  final _browseResults = StreamController<AndroidAutoBrowseNode>.broadcast();
  AndroidAutoNavigation? _lastNavigation;
  AndroidAutoMediaInfo? _lastMediaInfo;
  AndroidAutoPhoneStatus? _lastPhoneStatus;

  Pointer<AaSession> _session = nullptr;
  NativeCallable<Void Function(Int32, Pointer<Char>)>? _callback;
  StreamController<AndroidAutoAudioBuffer>? _audioController;
  NativeCallable<Void Function(Int32, Pointer<Uint8>, Int32, Int32, Int32)>?
  _audioCallback;
  NativeCallable<Void Function(Int32, Pointer<Char>)>? _metadataCallback;

  /// Called by the Flutter tooling through `dartPluginClass`.
  static void registerWith() {
    AndroidAutoPlatform.instance = AndroidAutoLinux();
  }

  @override
  Stream<AndroidAutoEvent> get events => _events.stream;

  @override
  Future<int?> get textureId async {
    if (_session == nullptr) {
      return null;
    }
    final id = _bindings.aa_session_texture_id(_session);
    return id < 0 ? null : id;
  }

  @override
  Future<AndroidAutoVideoInfo?> get videoInfo async {
    if (_session == nullptr) {
      return null;
    }
    final width = _bindings.aa_session_video_width(_session);
    final height = _bindings.aa_session_video_height(_session);
    if (width <= 0 || height <= 0) {
      return null;
    }
    final backend = _bindings.aa_session_video_backend(_session);
    var decoder = 'none';
    if (backend != nullptr) {
      decoder = backend.cast<Utf8>().toDartString();
      // Allocated on the core's heap, so it goes back the same way every other string
      // crossing this boundary does.
      _bindings.aa_string_free(backend);
    }
    return AndroidAutoVideoInfo(width: width, height: height, decoder: decoder);
  }

  @override
  Future<void> initialize(AndroidAutoConfig config) async {
    _createSession(config);
    if (_session == nullptr) {
      return;
    }
    // Publish the Bluetooth service and refuse on it, without offering anything.
    //
    // A head unit that is switched on and not projecting is the state a head unit is
    // in most of the time, and it is worth being in deliberately: a phone that knows
    // this machine as a wireless car asks for the service every five seconds and
    // shows its driver a notification saying it is connecting for as long as nothing
    // answers. A no-op unless the config asked for wireless.
    _bindings.aa_session_decline_wireless(_session);
  }

  @override
  Future<void> start(AndroidAutoConfig config) async {
    // A stopped session is restarted, not rebuilt. The native side keeps its USB
    // discovery alive across stop and start on purpose, so throwing the session away
    // here would defeat that. The same applies to a session that only exists because
    // initialize built it.
    if (_session != nullptr) {
      final result = _bindings.aa_session_start(_session);
      if (result != 0) {
        _emit(
          AndroidAutoConnectionState.error,
          'Could not restart the head unit session (code $result).',
        );
      }
      return;
    }
    _createSession(config);
    if (_session == nullptr) {
      return;
    }
    final result = _bindings.aa_session_start(_session);
    if (result != 0) {
      _emit(
        AndroidAutoConnectionState.error,
        'Could not start the head unit session (code $result).',
      );
    }
  }

  /// Builds the native session if there is not one already. Split out because a head
  /// unit application wants the session to exist from the moment it opens, so that it
  /// can refuse phones, while still only projecting once it is started.
  void _createSession(AndroidAutoConfig config) {
    if (_session != nullptr) {
      return;
    }

    // A listener callable, not an isolate local closure: the core raises events from
    // its own io_context threads, and only a listener is safe to invoke from those.
    _callback = NativeCallable<Void Function(Int32, Pointer<Char>)>.listener(
      _onNativeEvent,
    );

    final native = calloc<AaConfig>();
    final headUnitName = config.headUnitName.toNativeUtf8();
    final carModel = config.carModel.toNativeUtf8();
    final carYear = config.carYear.toNativeUtf8();
    final certificatePath = config.certificatePath?.toNativeUtf8();
    try {
      native.ref
        ..width = config.width
        ..height = config.height
        ..fps = config.fps
        ..dpi = config.dpi
        ..head_unit_name = headUnitName.cast()
        ..car_model = carModel.cast()
        ..car_year = carYear.cast()
        ..certificate_path = certificatePath?.cast() ?? nullptr
        ..sensors = config.sensors.fold(0, (mask, sensor) => mask | sensor.bit)
        ..metadata = config.metadata.fold(0, (mask, kind) => mask | kind.bit)
        ..transports = config.transports.fold(0, (mask, one) => mask | one.bit);

      _session = _bindings.aa_session_create(native, _callback!.nativeFunction);
      if (_session == nullptr) {
        _emit(AndroidAutoConnectionState.error, 'Could not create a head unit session.');
        return;
      }
      _wireless = config.wireless ?? _wireless;
      _applyAudioSettings();
      _applySensorSettings();
      _applyWirelessSettings();
      _installMetadataListener();
    } finally {
      // The core copies every string during aa_session_create, so they can go now.
      calloc
        ..free(native)
        ..free(headUnitName)
        ..free(carModel)
        ..free(carYear);
      if (certificatePath != null) {
        calloc.free(certificatePath);
      }
    }
  }

  @override
  Future<void> stop() async {
    // Stop, but keep the session. The native side reuses its USB discovery across
    // stop and start, because tearing that down and rebuilding it races with libusb's
    // hotplug callback. Destroying the session is dispose()'s job.
    if (_session != nullptr) {
      _bindings.aa_session_stop(_session);
    }
    // What the phone was playing is not true any more. The core empties its own copy
    // when the connection ends; this is the same statement on the Dart side, so a host
    // app reading lastMediaInfo after a stop gets nothing rather than a stale track.
    _lastNavigation = null;
    _lastMediaInfo = null;
    _lastPhoneStatus = null;
    _emit(AndroidAutoConnectionState.idle);
  }

  @override
  Future<void> dispose() async {
    if (_session != nullptr) {
      _bindings.aa_session_stop(_session);
      _bindings.aa_session_destroy(_session);
      _session = nullptr;
    }
    // Only after the session is gone: destroying it closes the event bus and clears the
    // audio tap, so no further events can arrive at the callables being torn down here.
    _callback?.close();
    _callback = null;
    _audioCallback?.close();
    _audioCallback = null;
    _metadataCallback?.close();
    _metadataCallback = null;
    await _audioController?.close();
    _audioController = null;
    await _navigation.close();
    await _mediaPlayback.close();
    await _phoneStatus.close();
    await _notifications.close();
    await _browseResults.close();
    await _events.close();
  }

  @override
  Future<void> startTestPattern() async {
    if (_session == nullptr) {
      return;
    }
    _bindings.aa_session_start_test_pattern(_session);
  }

  @override
  Future<void> stopTestPattern() async {
    if (_session == nullptr) {
      return;
    }
    _bindings.aa_session_stop_test_pattern(_session);
  }

  @override
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) {
    if (_session == nullptr || pointers.isEmpty) {
      return;
    }
    // Allocated and freed per report rather than kept in a reusable scratch buffer.
    // A drag produces one of these per pointer event, so at most a few hundred a
    // second, and calloc costs far less than the USB round trip that follows.
    final points = calloc<AaTouchPoint>(pointers.length);
    try {
      for (var i = 0; i < pointers.length; i++) {
        points[i]
          ..id = pointers[i].id
          ..x = pointers[i].x
          ..y = pointers[i].y;
      }
      _bindings.aa_session_send_touch(
        _session,
        _touchActions[action]!,
        actionIndex,
        points,
        pointers.length,
      );
    } finally {
      calloc.free(points);
    }
  }

  @override
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) {
    if (_session == nullptr) {
      return;
    }
    _bindings.aa_session_send_key(_session, key.code, down ? 1 : 0, longPress ? 1 : 0);
  }

  @override
  void sendRotary(int steps) {
    if (_session == nullptr) {
      return;
    }
    _bindings.aa_session_send_rotary(_session, steps);
  }

  @override
  double audioVolume(AndroidAutoAudioStream stream) => _volumes[stream] ?? 1.0;

  @override
  void setAudioVolume(AndroidAutoAudioStream stream, double volume) {
    final clamped = volume.clamp(0.0, 1.0);
    _volumes[stream] = clamped;
    if (_session != nullptr) {
      _bindings.aa_session_set_audio_volume(_session, stream.index, clamped);
    }
  }

  @override
  bool audioMuted(AndroidAutoAudioStream stream) => _muted[stream] ?? false;

  @override
  void setAudioMuted(AndroidAutoAudioStream stream, bool muted) {
    _muted[stream] = muted;
    if (_session != nullptr) {
      _bindings.aa_session_set_audio_muted(_session, stream.index, muted ? 1 : 0);
    }
  }

  @override
  Future<List<AndroidAutoAudioDevice>> audioDevices() async =>
      _parseDevices(_bindings.aa_audio_devices());

  @override
  Future<List<AndroidAutoAudioDevice>> microphoneDevices() async =>
      _parseDevices(_bindings.aa_microphone_devices());

  /// Turns one of the core's device listings into objects, and frees it.
  ///
  /// One device per line, three tab separated fields: the name to hand back, a
  /// description for a person, and "1" for the one the server would pick. Both
  /// directions use the format, so both use this.
  List<AndroidAutoAudioDevice> _parseDevices(Pointer<Char> listing) {
    if (listing == nullptr) {
      return const <AndroidAutoAudioDevice>[];
    }
    final text = listing.cast<Utf8>().toDartString();
    _bindings.aa_string_free(listing);
    final devices = <AndroidAutoAudioDevice>[];
    for (final line in text.split('\n')) {
      if (line.isEmpty) {
        continue;
      }
      final fields = line.split('\t');
      if (fields.length < 3) {
        continue;
      }
      devices.add(
        AndroidAutoAudioDevice(
          name: fields[0],
          description: fields[1],
          isDefault: fields[2] == '1',
        ),
      );
    }
    return devices;
  }

  @override
  Future<List<AndroidAutoBluetoothDevice>> pairedPhones() async {
    final listing = _bindings.aa_paired_phones();
    if (listing == nullptr) {
      return const <AndroidAutoBluetoothDevice>[];
    }
    final text = listing.cast<Utf8>().toDartString();
    _bindings.aa_string_free(listing);
    final devices = <AndroidAutoBluetoothDevice>[];
    for (final line in text.split('\n')) {
      if (line.isEmpty) {
        continue;
      }
      final fields = line.split('\t');
      if (fields.length < 4) {
        continue;
      }
      devices.add(
        AndroidAutoBluetoothDevice(
          address: fields[0],
          name: fields[1],
          connected: fields[2] == '1',
          isPhone: fields[3] == '1',
        ),
      );
    }
    return devices;
  }

  @override
  void setWirelessConfig(AndroidAutoWirelessConfig config) {
    _wireless = config;
    _applyWirelessSettings();
  }

  /// Pushes the held wireless configuration into the core. Called when it changes and
  /// again for every new session, the same shape as [_applyAudioSettings].
  void _applyWirelessSettings() {
    final config = _wireless;
    if (_session == nullptr || config == null) {
      return;
    }
    final native = calloc<AaWirelessConfig>();
    final ssid = config.ssid.toNativeUtf8();
    final passphrase = config.passphrase.toNativeUtf8();
    final bssid = config.bssid.toNativeUtf8();
    final interfaceName = config.interfaceName.toNativeUtf8();
    final ipAddress = config.ipAddress.toNativeUtf8();
    final phoneAddress = config.phoneAddress.toNativeUtf8();
    try {
      native.ref
        ..ssid = ssid.cast()
        ..passphrase = passphrase.cast()
        ..bssid = bssid.cast()
        ..interface_name = interfaceName.cast()
        ..ip_address = ipAddress.cast()
        ..phone_address = phoneAddress.cast()
        ..port = config.port
        ..security = config.security.value
        ..access_point = config.accessPoint.value;
      _bindings.aa_session_set_wireless_config(_session, native);
    } finally {
      // The core copies every string, so they can go as soon as the call returns.
      calloc
        ..free(native)
        ..free(ssid)
        ..free(passphrase)
        ..free(bssid)
        ..free(interfaceName)
        ..free(ipAddress)
        ..free(phoneAddress);
    }
  }

  @override
  Future<void> startWireless() async {
    if (_session == nullptr) {
      _emit(
        AndroidAutoConnectionState.error,
        'Start the head unit session before turning wireless on.',
      );
      return;
    }
    _applyWirelessSettings();
    // A failure has already gone out through the event callback with the reason in
    // it, which is more use than a code here would be.
    _bindings.aa_session_start_wireless(_session);
  }

  @override
  Future<void> stopWireless() async {
    if (_session == nullptr) {
      return;
    }
    _bindings.aa_session_stop_wireless(_session);
  }

  @override
  bool get wirelessActive => _session == nullptr
      ? false
      : _bindings.aa_session_wireless_active(_session) == 1;

  @override
  AndroidAutoWirelessStatus? get wirelessStatus {
    if (_session == nullptr) {
      return null;
    }
    final line = _bindings.aa_session_wireless_summary(_session);
    if (line == nullptr) {
      return null;
    }
    final text = line.cast<Utf8>().toDartString();
    _bindings.aa_string_free(line);
    if (text.isEmpty) {
      return null;
    }
    final fields = text.split('\t');
    if (fields.length < 8) {
      return null;
    }
    return AndroidAutoWirelessStatus(
      interfaceName: fields[0],
      ssid: fields[1],
      bssid: fields[2],
      ipAddress: fields[3],
      port: int.tryParse(fields[4]) ?? 0,
      hosting: fields[5] == '1',
      bluetoothReady: fields[6] == '1',
      phoneLinked: fields[7] == '1',
    );
  }

  @override
  String get audioDevice => _audioDevice;

  @override
  void setAudioDevice(String? name) {
    _audioDevice = name ?? '';
    if (_session == nullptr) {
      return;
    }
    final native = _audioDevice.isEmpty ? null : _audioDevice.toNativeUtf8();
    try {
      _bindings.aa_session_set_audio_device(_session, native?.cast() ?? nullptr);
    } finally {
      if (native != null) {
        calloc.free(native);
      }
    }
  }

  @override
  bool get audioOutputEnabled => _audioOutputEnabled;

  @override
  void setAudioOutputEnabled(bool enabled) {
    _audioOutputEnabled = enabled;
    if (_session != nullptr) {
      _bindings.aa_session_set_audio_output_enabled(_session, enabled ? 1 : 0);
    }
  }

  @override
  String get audioBackend {
    if (_session == nullptr) {
      return 'none';
    }
    final backend = _bindings.aa_session_audio_backend(_session);
    if (backend == nullptr) {
      return 'none';
    }
    final name = backend.cast<Utf8>().toDartString();
    _bindings.aa_string_free(backend);
    return name;
  }

  @override
  int audioUnderruns(AndroidAutoAudioStream stream) =>
      _session == nullptr
          ? 0
          : _bindings.aa_session_audio_underruns(_session, stream.index);

  @override
  int audioDropped(AndroidAutoAudioStream stream) =>
      _session == nullptr
          ? 0
          : _bindings.aa_session_audio_dropped(_session, stream.index);

  @override
  Duration audioLatency(AndroidAutoAudioStream stream) => Duration(
    microseconds:
        _session == nullptr
            ? 0
            : _bindings.aa_session_audio_latency(_session, stream.index),
  );

  @override
  bool get microphoneActive =>
      _session != nullptr && _bindings.aa_session_microphone_active(_session) != 0;

  @override
  double get microphoneLevel =>
      _session == nullptr ? 0.0 : _bindings.aa_session_microphone_level(_session);

  @override
  int get microphoneBytes =>
      _session == nullptr ? 0 : _bindings.aa_session_microphone_bytes(_session);

  @override
  String get microphoneDevice => _microphoneDevice;

  @override
  void setMicrophoneDevice(String? name) {
    _microphoneDevice = name ?? '';
    if (_session == nullptr) {
      return;
    }
    final native = _microphoneDevice.isEmpty ? null : _microphoneDevice.toNativeUtf8();
    try {
      _bindings.aa_session_set_microphone_device(_session, native?.cast() ?? nullptr);
    } finally {
      if (native != null) {
        calloc.free(native);
      }
    }
  }

  @override
  String get microphoneBackend {
    if (_session == nullptr) {
      return 'none';
    }
    final backend = _bindings.aa_session_microphone_backend(_session);
    if (backend == nullptr) {
      return 'none';
    }
    final name = backend.cast<Utf8>().toDartString();
    _bindings.aa_string_free(backend);
    return name;
  }

  @override
  Stream<AndroidAutoAudioBuffer> get audioBuffers {
    // Built on first use and never torn down while the plugin lives. Every buffer
    // delivered costs a copy in the core, so the tap is only installed once something
    // is actually listening.
    final controller =
        _audioController ??= StreamController<AndroidAutoAudioBuffer>.broadcast(
          onListen: _installAudioTap,
          onCancel: _removeAudioTap,
        );
    return controller.stream;
  }

  void _applyAudioSettings() {
    for (final stream in AndroidAutoAudioStream.values) {
      _bindings.aa_session_set_audio_volume(
        _session,
        stream.index,
        _volumes[stream] ?? 1.0,
      );
      _bindings.aa_session_set_audio_muted(
        _session,
        stream.index,
        (_muted[stream] ?? false) ? 1 : 0,
      );
    }
    setAudioDevice(_audioDevice);
    setMicrophoneDevice(_microphoneDevice);
    _bindings.aa_session_set_audio_output_enabled(
      _session,
      _audioOutputEnabled ? 1 : 0,
    );
    if (_audioCallback != null) {
      _bindings.aa_session_set_audio_callback(
        _session,
        _audioCallback!.nativeFunction,
      );
    }
  }

  @override
  bool get nightMode => _nightMode;

  @override
  void setNightMode(bool night) {
    _nightMode = night;
    _writeSensor(AndroidAutoSensor.nightMode, () {
      _bindings.aa_session_set_night_mode(_session, night ? 1 : 0);
    });
  }

  @override
  Set<AndroidAutoDrivingRestriction> get drivingRestrictions => _drivingRestrictions;

  @override
  void setDrivingRestrictions(Set<AndroidAutoDrivingRestriction> restrictions) {
    _drivingRestrictions = Set.unmodifiable(restrictions);
    final mask = restrictions.fold(0, (value, one) => value | one.code);
    _writeSensor(AndroidAutoSensor.drivingStatus, () {
      _bindings.aa_session_set_driving_status(_session, mask);
    });
  }

  @override
  AndroidAutoLocation? get location => _location;

  @override
  void setLocation(AndroidAutoLocation location) {
    _location = location;
    _writeSensor(AndroidAutoSensor.location, () {
      final native = calloc<AaLocation>();
      try {
        native.ref
          ..latitude = location.latitude
          ..longitude = location.longitude
          // The core skips a NaN rather than sending a zero, which is the whole point
          // of these four being nullable: a bearing of zero is due north.
          ..accuracy_metres = location.accuracyMetres ?? double.nan
          ..altitude_metres = location.altitudeMetres ?? double.nan
          ..speed_mps = location.speedMps ?? double.nan
          ..bearing_degrees = location.bearingDegrees ?? double.nan;
        _bindings.aa_session_set_location(_session, native);
      } finally {
        calloc.free(native);
      }
    });
  }

  @override
  void setSpeed(double metresPerSecond) {
    _writeSensor(AndroidAutoSensor.speed, () {
      _bindings.aa_session_set_speed(_session, metresPerSecond);
    });
  }

  @override
  void setRpm(double rpm) {
    _writeSensor(AndroidAutoSensor.rpm, () {
      _bindings.aa_session_set_rpm(_session, rpm);
    });
  }

  @override
  void setFuel({
    required double levelPercent,
    required double rangeMetres,
    bool low = false,
  }) {
    _writeSensor(AndroidAutoSensor.fuel, () {
      _bindings.aa_session_set_fuel(_session, levelPercent, rangeMetres, low ? 1 : 0);
    });
  }

  @override
  void setParkingBrake(bool engaged) {
    _writeSensor(AndroidAutoSensor.parkingBrake, () {
      _bindings.aa_session_set_parking_brake(_session, engaged ? 1 : 0);
    });
  }

  @override
  void setGear(int gear) {
    _writeSensor(AndroidAutoSensor.gear, () {
      _bindings.aa_session_set_gear(_session, gear);
    });
  }

  @override
  void setCompass(double bearingDegrees) {
    _writeSensor(AndroidAutoSensor.compass, () {
      _bindings.aa_session_set_compass(_session, bearingDegrees);
    });
  }

  @override
  void setEnvironment({double? temperatureCelsius, double? pressureKpa}) {
    _writeSensor(AndroidAutoSensor.environment, () {
      _bindings.aa_session_set_environment(
        _session,
        temperatureCelsius ?? double.nan,
        pressureKpa ?? double.nan,
      );
    });
  }

  @override
  void setOdometer(double kilometres) {
    _writeSensor(AndroidAutoSensor.odometer, () {
      _bindings.aa_session_set_odometer(_session, kilometres);
    });
  }

  @override
  void setTollCard(bool present) {
    _writeSensor(AndroidAutoSensor.tollCard, () {
      _bindings.aa_session_set_toll_card(_session, present ? 1 : 0);
    });
  }

  @override
  Set<AndroidAutoSensor> get sensorSubscriptions {
    if (_session == nullptr) {
      return const <AndroidAutoSensor>{};
    }
    final mask = _bindings.aa_session_sensor_subscriptions(_session);
    return {
      for (final sensor in AndroidAutoSensor.values)
        if (mask & sensor.bit != 0) sensor,
    };
  }

  @override
  int get sensorBatches =>
      _session == nullptr ? 0 : _bindings.aa_session_sensor_batches(_session);

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

  @override
  Stream<AndroidAutoNotification> get notifications => _notifications.stream;

  @override
  Stream<AndroidAutoBrowseNode> get browseResults => _browseResults.stream;

  @override
  bool browse({String path = '', int start = 0}) {
    if (_session == nullptr) {
      return false;
    }
    final native = path.isEmpty ? null : path.toNativeUtf8();
    try {
      return _bindings.aa_session_browse(
            _session,
            native?.cast() ?? nullptr,
            start,
          ) ==
          0;
    } finally {
      if (native != null) {
        calloc.free(native);
      }
    }
  }

  @override
  bool browseSelect(String path) {
    if (_session == nullptr || path.isEmpty) {
      return false;
    }
    final native = path.toNativeUtf8();
    try {
      return _bindings.aa_session_browse_select(_session, native.cast()) == 0;
    } finally {
      calloc.free(native);
    }
  }

  @override
  Set<AndroidAutoMetadata> get metadataChannels {
    if (_session == nullptr) {
      return const <AndroidAutoMetadata>{};
    }
    final mask = _bindings.aa_session_metadata_channels(_session);
    return {
      for (final kind in AndroidAutoMetadata.values)
        if (mask & kind.bit != 0) kind,
    };
  }

  @override
  int metadataUpdates(AndroidAutoMetadata kind) => _session == nullptr
      ? 0
      : _bindings.aa_session_metadata_updates(_session, kind.index);

  /// Installs the native metadata listener, once, and points it at this object.
  ///
  /// Unlike the raw PCM tap this is armed whether or not anything is listening. The
  /// updates are a few hundred bytes each and arrive when a track or a turn changes,
  /// which is orders of magnitude rarer than an audio buffer, and arming it lazily
  /// would mean a host app that subscribes after the phone has already said something
  /// sees nothing until it says it again.
  void _installMetadataListener() {
    // A listener callable, not an isolate local closure: the core delivers these from
    // its own io_context threads.
    _metadataCallback ??=
        NativeCallable<Void Function(Int32, Pointer<Char>)>.listener(_onNativeMetadata);
    if (_session != nullptr) {
      _bindings.aa_session_set_metadata_callback(
        _session,
        _metadataCallback!.nativeFunction,
      );
    }
  }

  void _onNativeMetadata(int kind, Pointer<Char> json) {
    if (json == nullptr) {
      return;
    }
    final text = json.cast<Utf8>().toDartString();
    // Back to the core's heap before anything else can throw, which has to happen
    // whatever the decode does with it.
    _bindings.aa_string_free(json);
    if (kind < 0 || kind >= AndroidAutoMetadata.values.length || text.isEmpty) {
      return;
    }
    Object? decoded;
    try {
      decoded = jsonDecode(text);
    } on FormatException {
      // The core built this string, so a failure here is a bug rather than a phone
      // saying something odd. Dropped rather than thrown: a malformed update must not
      // take down the isolate that is drawing the projection.
      return;
    }
    if (decoded is! Map<String, dynamic>) {
      return;
    }
    switch (AndroidAutoMetadata.values[kind]) {
      case AndroidAutoMetadata.navigation:
        _lastNavigation = AndroidAutoNavigation.fromJson(decoded);
        _add(_navigation, _lastNavigation!);
      case AndroidAutoMetadata.media:
        _lastMediaInfo = AndroidAutoMediaInfo.fromJson(decoded);
        _add(_mediaPlayback, _lastMediaInfo!);
      case AndroidAutoMetadata.phone:
        _lastPhoneStatus = AndroidAutoPhoneStatus.fromJson(decoded);
        _add(_phoneStatus, _lastPhoneStatus!);
      case AndroidAutoMetadata.notification:
        _add(_notifications, AndroidAutoNotification.fromJson(decoded));
      case AndroidAutoMetadata.browse:
        _add(_browseResults, AndroidAutoBrowseNode.fromJson(decoded));
    }
  }

  static void _add<T>(StreamController<T> controller, T value) {
    if (!controller.isClosed) {
      controller.add(value);
    }
  }

  /// Remembers how to push one sensor, and pushes it now if there is a session.
  ///
  /// The closure rather than the value, so replaying into a new session is the same
  /// code path as setting it in the first place and the two cannot drift.
  void _writeSensor(AndroidAutoSensor sensor, void Function() write) {
    _sensorWrites[sensor] = write;
    if (_session != nullptr) {
      write();
    }
  }

  void _applySensorSettings() {
    // In the order the app set them, which for the two that combine into one reading is
    // the order that leaves the newest value in place.
    for (final write in _sensorWrites.values) {
      write();
    }
  }

  void _installAudioTap() {
    // A listener callable, not an isolate local closure: the core delivers these from
    // its own audio writer threads.
    _audioCallback ??=
        NativeCallable<Void Function(Int32, Pointer<Uint8>, Int32, Int32, Int32)>
            .listener(_onNativeAudio);
    if (_session != nullptr) {
      _bindings.aa_session_set_audio_callback(
        _session,
        _audioCallback!.nativeFunction,
      );
    }
  }

  void _removeAudioTap() {
    if (_session != nullptr) {
      _bindings.aa_session_set_audio_callback(_session, nullptr);
    }
    // The callable itself is kept. A buffer can already be in flight towards it, and
    // closing it now would be closing it out from under that call.
  }

  void _onNativeAudio(
    int stream,
    Pointer<Uint8> data,
    int size,
    int sampleRate,
    int channels,
  ) {
    if (data == nullptr) {
      return;
    }
    // Copied out before the core's buffer goes back, which has to happen whatever the
    // controller does with it.
    final samples = Uint8List.fromList(data.asTypedList(size));
    _bindings.aa_audio_buffer_free(data);
    final controller = _audioController;
    if (controller == null || controller.isClosed || !controller.hasListener) {
      return;
    }
    if (stream < 0 || stream >= AndroidAutoAudioStream.values.length) {
      return;
    }
    controller.add(
      AndroidAutoAudioBuffer(
        stream: AndroidAutoAudioStream.values[stream],
        sampleRate: sampleRate,
        channels: channels,
        samples: samples,
      ),
    );
  }

  /// The AaTouchAction integers from `linux/src/aa_core.h`.
  ///
  /// Spelled out rather than taken from the enum's index, because the protocol reuses
  /// Android's MotionEvent constants and those skip 3 and 4.
  static const Map<AndroidAutoTouchAction, int> _touchActions = {
    AndroidAutoTouchAction.down: 0,
    AndroidAutoTouchAction.up: 1,
    AndroidAutoTouchAction.move: 2,
    AndroidAutoTouchAction.pointerDown: 5,
    AndroidAutoTouchAction.pointerUp: 6,
  };

  void _onNativeEvent(int state, Pointer<Char> message) {
    String? text;
    if (message != nullptr) {
      text = message.cast<Utf8>().toDartString();
      // The core allocated this on its own heap, so hand it back rather than calling
      // free from Dart.
      _bindings.aa_string_free(message);
    }
    _emit(_stateFromNative(state), text);
  }

  void _emit(AndroidAutoConnectionState state, [String? message]) {
    if (!_events.isClosed) {
      _events.add(AndroidAutoEvent(state, message));
    }
  }

  static AndroidAutoConnectionState _stateFromNative(int value) {
    // The integers are the AaState enum in linux/src/aa_core.h. Keep the two in step.
    if (value < 0 || value >= AndroidAutoConnectionState.values.length) {
      return AndroidAutoConnectionState.error;
    }
    return AndroidAutoConnectionState.values[value];
  }
}
