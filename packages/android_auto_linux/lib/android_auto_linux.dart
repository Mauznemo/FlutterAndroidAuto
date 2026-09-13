/// Linux implementation of the `android_auto` plugin.
///
/// Links the native head unit core, which is built on aasdk and is therefore
/// GPL-3.0-or-later. See `docs/research.md` for what that means for host apps.
library;

import 'dart:async';
import 'dart:ffi';

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

  AaCoreBindings get _bindings => AaLibrary.instance.bindings;

  Pointer<AaSession> _session = nullptr;
  NativeCallable<Void Function(Int32, Pointer<Char>)>? _callback;

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
    // Only after the session is gone: destroying it closes the event bus, so no further
    // events can arrive at the callable being torn down here.
    _callback?.close();
    _callback = null;
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
