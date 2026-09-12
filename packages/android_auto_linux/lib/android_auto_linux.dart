/// Linux implementation of the `android_auto` plugin.
///
/// Links the native head unit core, which is built on aasdk and is therefore
/// GPL-3.0-or-later. See `docs/research.md` for what that means for host apps.
library;

import 'dart:async';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';

/// Registers itself as the platform implementation on Linux.
class AndroidAutoLinux extends AndroidAutoPlatform {
  final _events = StreamController<AndroidAutoEvent>.broadcast();

  /// Called by the Flutter tooling through `dartPluginClass`.
  static void registerWith() {
    AndroidAutoPlatform.instance = AndroidAutoLinux();
  }

  @override
  Stream<AndroidAutoEvent> get events => _events.stream;

  @override
  Future<int?> get textureId async => null;

  @override
  Future<void> start(AndroidAutoConfig config) async {
    // M2 replaces this with the FFI call into aa_session_create/aa_session_start.
    _events.add(
      const AndroidAutoEvent(
        AndroidAutoConnectionState.error,
        'The native head unit core is not implemented yet (milestone M2).',
      ),
    );
  }

  @override
  Future<void> stop() async {
    _events.add(const AndroidAutoEvent(AndroidAutoConnectionState.idle));
  }
}
