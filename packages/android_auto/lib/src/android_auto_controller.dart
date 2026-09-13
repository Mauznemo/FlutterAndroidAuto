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

  /// Begins looking for a phone.
  Future<void> start() async {
    await _platform.start(config);
    await _refreshTextureId();
  }

  /// Ends the session.
  Future<void> stop() async {
    await _platform.stop();
    _textureId = null;
    notifyListeners();
  }

  /// Drives the video path from a generated pattern instead of a phone.
  ///
  /// Useful for laying out an overlay before any hardware is involved.
  Future<void> startTestPattern() async {
    await _platform.startTestPattern();
    await _refreshTextureId();
  }

  /// Stops the pattern started by [startTestPattern].
  Future<void> stopTestPattern() => _platform.stopTestPattern();

  Future<void> _refreshTextureId() async {
    final id = await _platform.textureId;
    if (id != _textureId) {
      _textureId = id;
      notifyListeners();
    }
  }

  void _onEvent(AndroidAutoEvent event) {
    _state = event.state;
    _message = event.message;
    notifyListeners();
    // The texture is registered natively the moment video starts, so an event is the
    // earliest point at which asking for the id is worthwhile.
    _refreshTextureId();
  }

  @override
  void dispose() {
    _subscription.cancel();
    _platform.dispose();
    super.dispose();
  }
}
