// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

/// The hardware buttons a real head unit has on its dashboard or steering wheel.
///
/// Exists to exercise the key half of the input channel, which touch alone never
/// reaches: a phone routes these itself rather than drawing them, so the only way to
/// tell they arrived is to watch what the projection does. A product would wire its
/// physical buttons to the same two calls, `pressKey` and `sendRotary`.
class Keypad extends StatelessWidget {
  final ValueChanged<AndroidAutoKey> onKey;

  /// Rotary detents, negative for anticlockwise.
  final ValueChanged<int> onRotate;

  const Keypad({super.key, required this.onKey, required this.onRotate});

  @override
  Widget build(BuildContext context) {
    Widget key(IconData icon, String tooltip, VoidCallback onPressed) {
      return IconButton.filledTonal(
        tooltip: tooltip,
        onPressed: onPressed,
        icon: Icon(icon, size: 20),
      );
    }

    return Row(
      mainAxisSize: MainAxisSize.min,
      spacing: 6,
      children: [
        key(Icons.arrow_back, 'Back', () => onKey(AndroidAutoKey.back)),
        key(Icons.home, 'Home', () => onKey(AndroidAutoKey.home)),
        key(Icons.skip_previous, 'Previous', () => onKey(AndroidAutoKey.previous)),
        key(Icons.play_arrow, 'Play or pause', () => onKey(AndroidAutoKey.playPause)),
        key(Icons.skip_next, 'Next', () => onKey(AndroidAutoKey.next)),
        key(Icons.mic, 'Assistant', () => onKey(AndroidAutoKey.microphone)),
        const SizedBox(width: 12),
        key(Icons.rotate_left, 'Rotary anticlockwise', () => onRotate(-1)),
        key(Icons.adjust, 'Rotary push', () => onKey(AndroidAutoKey.enter)),
        key(Icons.rotate_right, 'Rotary clockwise', () => onRotate(1)),
      ],
    );
  }
}
