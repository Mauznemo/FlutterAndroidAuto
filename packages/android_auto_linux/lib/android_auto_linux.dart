/// Linux implementation of the `android_auto` plugin.
///
/// Links the native head unit core, which is built on aasdk and is therefore
/// GPL-3.0-or-later. See `docs/research.md` for what that means for host apps.
library;

import 'dart:async';
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

  AaCoreBindings get _bindings => AaLibrary.instance.bindings;

  Pointer<AaSession> _session = nullptr;
  NativeCallable<Void Function(Int32, Pointer<Char>)>? _callback;
  StreamController<AndroidAutoAudioBuffer>? _audioController;
  NativeCallable<Void Function(Int32, Pointer<Uint8>, Int32, Int32, Int32)>?
  _audioCallback;

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
  Future<void> start(AndroidAutoConfig config) async {
    // A stopped session is restarted, not rebuilt. The native side keeps its USB
    // discovery alive across stop and start on purpose, so throwing the session away
    // here would defeat that.
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
        ..certificate_path = certificatePath?.cast() ?? nullptr;

      _session = _bindings.aa_session_create(native, _callback!.nativeFunction);
      if (_session == nullptr) {
        _emit(AndroidAutoConnectionState.error, 'Could not create a head unit session.');
        return;
      }
      _applyAudioSettings();
      final result = _bindings.aa_session_start(_session);
      if (result != 0) {
        _emit(
          AndroidAutoConnectionState.error,
          'Could not start the head unit session (code $result).',
        );
      }
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
    await _audioController?.close();
    _audioController = null;
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
