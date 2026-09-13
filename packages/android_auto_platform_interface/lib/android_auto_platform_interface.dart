/// Platform interface for the `android_auto` plugin.
///
/// Contains no native code and no GPL code, so a future permissive implementation
/// can replace `android_auto_linux` without any host app change. See
/// `docs/architecture.md`.
library;

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

  /// Releases everything the implementation holds.
  ///
  /// Distinct from [stop]: a stopped session can be started again, a disposed one
  /// cannot. Host apps normally call this only when shutting down.
  Future<void> dispose() async {}
}
