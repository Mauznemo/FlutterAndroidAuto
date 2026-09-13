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
