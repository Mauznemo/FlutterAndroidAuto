// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'panel_frame.dart';

/// The density the phone lays out at, and what the video actually is.
///
/// The density is what a "size of text and buttons" setting in a real head unit would
/// change. It applies from the next connection, so the way to see it is to move the
/// slider, stop, and start again: the phone's interface comes back bigger or smaller.
class DisplayPanel extends StatelessWidget {
  final AndroidAutoController controller;
  final VoidCallback onClose;

  const DisplayPanel({super.key, required this.controller, required this.onClose});

  static const int _minDpi = 80;
  static const int _maxDpi = 320;
  static const int _dpiStep = 20;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: controller,
      builder: (context, _) {
        final video = controller.videoInfo;
        final dpi = controller.dpi.clamp(_minDpi, _maxDpi);
        return PanelFrame(
          title: 'Display',
          icon: Icons.aspect_ratio,
          onClose: onClose,
          children: [
            const PanelSection('Density'),
            Row(
              children: [
                Expanded(
                  child: Slider(
                    value: dpi.toDouble(),
                    min: _minDpi.toDouble(),
                    max: _maxDpi.toDouble(),
                    divisions: (_maxDpi - _minDpi) ~/ _dpiStep,
                    label: '$dpi dpi',
                    onChanged: (value) => controller.setDpi(value.round()),
                  ),
                ),
                SizedBox(
                  width: 58,
                  child: Text(
                    '${controller.dpi} dpi',
                    textAlign: TextAlign.right,
                    style: const TextStyle(fontSize: 12),
                  ),
                ),
              ],
            ),
            PanelNote(
              'Higher is bigger. Applies from the next connection; the config said '
              '${controller.config.dpi}.',
            ),
            const PanelSection('Video'),
            PanelNote(
              video == null
                  ? 'No picture'
                  : '${video.width}x${video.height}, ${video.decoder}',
            ),
          ],
        );
      },
    );
  }
}
