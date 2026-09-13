import 'package:flutter/widgets.dart';

import 'android_auto_controller.dart';

/// Renders the projected phone screen.
///
/// This is the whole point of the package: the projection is a [Texture] in the widget
/// tree, not a separate window, so a host app can put anything it likes on top of it
/// with ordinary Flutter widgets.
///
/// ```dart
/// Stack(
///   children: [
///     AndroidAutoView(controller: controller),
///     MyStatusBar(),
///   ],
/// )
/// ```
class AndroidAutoView extends StatelessWidget {
  /// The session to render.
  final AndroidAutoController controller;

  /// Shown while there is no video stream yet.
  final Widget? placeholder;

  /// How the projected surface is fitted into the available space.
  ///
  /// From milestone M5 touch coordinates are mapped through the same fit, so changing
  /// this stays consistent between what is drawn and where taps land.
  final BoxFit fit;

  /// Creates a view for [controller].
  const AndroidAutoView({
    super.key,
    required this.controller,
    this.placeholder,
    this.fit = BoxFit.contain,
  });

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: controller,
      builder: (context, _) {
        final textureId = controller.textureId;
        if (textureId == null) {
          return placeholder ?? const SizedBox.expand();
        }
        return FittedBox(
          fit: fit,
          child: SizedBox(
            width: controller.config.width.toDouble(),
            height: controller.config.height.toDouble(),
            child: Texture(textureId: textureId),
          ),
        );
      },
    );
  }
}
