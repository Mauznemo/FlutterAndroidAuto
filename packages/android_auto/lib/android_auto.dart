/// Embed an Android Auto head unit inside a Flutter app.
///
/// The projected phone screen is rendered into a Flutter [Texture], so ordinary
/// widgets can be composited on top of it. See `docs/architecture.md`.
library;

export 'package:android_auto_platform_interface/android_auto_platform_interface.dart'
    show AndroidAutoConfig, AndroidAutoConnectionState, AndroidAutoEvent;

export 'src/android_auto_controller.dart';
export 'src/android_auto_view.dart';
