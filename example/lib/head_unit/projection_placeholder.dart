// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

/// Stands in for the projected video whenever there is no live picture.
///
/// Handed to `AndroidAutoView.placeholder`, which shows it until the phone's first
/// frame, not merely until it connects: a phone reports connected a few seconds before
/// it has drawn anything over a cable, and about twenty over Wi-Fi. So it says which of
/// those it is waiting for, rather than telling a driver whose phone is on its way to
/// plug one in.
class ProjectionPlaceholder extends StatelessWidget {
  /// Where the session is, which decides what to say.
  final AndroidAutoConnectionState state;

  const ProjectionPlaceholder({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    final (title, detail) = switch (state) {
      AndroidAutoConnectionState.handshaking ||
      AndroidAutoConnectionState.connected => (
        'Phone connecting',
        'Waiting for the first picture from the phone',
      ),
      AndroidAutoConnectionState.searching => (
        'Looking for a phone',
        'Waiting for a phone to connect',
      ),
      _ => (
        'No phone projecting',
        'Plug a phone in and press Start, or run the test pattern to drive the '
            'texture without one',
      ),
    };
    return Container(
      decoration: const BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF15202B), Color(0xFF0B1016)],
        ),
      ),
      alignment: Alignment.center,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.phone_android, size: 64, color: Colors.white24),
          const SizedBox(height: 16),
          Text(title, style: const TextStyle(fontSize: 22, color: Colors.white54)),
          const SizedBox(height: 8),
          Text(detail, style: const TextStyle(color: Colors.white38)),
        ],
      ),
    );
  }
}
