import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';

/// Starts, stops and observes a head unit session.
///
/// A host app normally creates one of these for the lifetime of the app and hands it
/// to an `AndroidAutoView`.
class AndroidAutoController {
  /// How the head unit describes itself to the phone.
  final AndroidAutoConfig config;

  /// Creates a controller. Nothing happens until [start] is called.
  AndroidAutoController({this.config = const AndroidAutoConfig()});

  AndroidAutoPlatform get _platform => AndroidAutoPlatform.instance;

  /// Lifecycle and error events from the native session.
  Stream<AndroidAutoEvent> get events => _platform.events;

  /// Id of the texture carrying the projected video, or null while there is none.
  Future<int?> get textureId => _platform.textureId;

  /// Begins looking for a phone.
  Future<void> start() => _platform.start(config);

  /// Ends the session.
  Future<void> stop() => _platform.stop();
}
