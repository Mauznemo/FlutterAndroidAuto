import 'package:flutter/widgets.dart';

import 'android_auto_controller.dart';

/// Renders the projected phone screen, and forwards pointer events back to the phone.
///
/// Put it in a [Stack] and draw whatever the host app wants on top of it.
class AndroidAutoView extends StatefulWidget {
  /// The session to render. Must already have been started, or be started later.
  final AndroidAutoController controller;

  /// Shown while there is no video stream yet.
  final Widget? placeholder;

  /// How the projected surface is fitted into the available space. Touch coordinates
  /// are mapped through the same fit, so changing this stays consistent.
  final BoxFit fit;

  /// Creates a view for [controller].
  const AndroidAutoView({
    super.key,
    required this.controller,
    this.placeholder,
    this.fit = BoxFit.contain,
  });

  @override
  State<AndroidAutoView> createState() => _AndroidAutoViewState();
}

class _AndroidAutoViewState extends State<AndroidAutoView> {
  @override
  Widget build(BuildContext context) {
    // M4 replaces this with a Texture fed by the native video pipeline, and M5 adds
    // the Listener that maps pointer events into projected coordinates.
    return widget.placeholder ?? const SizedBox.expand();
  }
}
