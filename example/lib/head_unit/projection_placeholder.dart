// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:flutter/material.dart';

/// Stands in for the projected video until a session is producing frames.
///
/// Handed to `AndroidAutoView.placeholder`, which shows it for as long as there is no
/// texture to draw.
class ProjectionPlaceholder extends StatelessWidget {
  const ProjectionPlaceholder({super.key});

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: const BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF15202B), Color(0xFF0B1016)],
        ),
      ),
      alignment: Alignment.center,
      child: const Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.phone_android, size: 64, color: Colors.white24),
          SizedBox(height: 16),
          Text(
            'No phone projecting',
            style: TextStyle(fontSize: 22, color: Colors.white54),
          ),
          SizedBox(height: 8),
          Text(
            'Plug a phone in and press Start, or run the test pattern to drive the '
            'texture without one',
            style: TextStyle(color: Colors.white38),
          ),
        ],
      ),
    );
  }
}
