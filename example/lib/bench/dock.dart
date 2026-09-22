// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import '../head_unit/keypad.dart';
import 'panel_frame.dart';

export 'panel_frame.dart' show BenchPanel;

/// The controls along the bottom: the hardware keys, starting and stopping, and the
/// bench panels.
///
/// Collapses to a single handle, because whatever sits over the projection is a patch
/// of the phone's screen the driver cannot touch. Maps puts its zoom buttons and speed
/// limit down there.
class Dock extends StatelessWidget {
  final AndroidAutoController controller;
  final bool expanded;
  final BenchPanel? openPanel;
  final bool patternRunning;
  final int hitTestTaps;
  final ValueChanged<bool> onExpandedChanged;
  final VoidCallback onStart;
  final VoidCallback onStop;
  final VoidCallback onTogglePattern;
  final ValueChanged<BenchPanel> onTogglePanel;
  final ValueChanged<AndroidAutoKey> onKey;
  final ValueChanged<int> onRotate;
  final VoidCallback onHitTest;

  const Dock({
    super.key,
    required this.controller,
    required this.expanded,
    required this.openPanel,
    required this.patternRunning,
    required this.hitTestTaps,
    required this.onExpandedChanged,
    required this.onStart,
    required this.onStop,
    required this.onTogglePattern,
    required this.onTogglePanel,
    required this.onKey,
    required this.onRotate,
    required this.onHitTest,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: AnimatedSize(
        duration: const Duration(milliseconds: 180),
        alignment: Alignment.bottomCenter,
        child: expanded ? _expanded() : _collapsed(),
      ),
    );
  }

  Widget _collapsed() {
    return _surface(
      padding: const EdgeInsets.all(4),
      child: IconButton(
        tooltip: 'Show controls',
        onPressed: () => onExpandedChanged(true),
        icon: const Icon(Icons.expand_less),
      ),
    );
  }

  Widget _expanded() {
    final running = controller.state != AndroidAutoConnectionState.idle;
    return _surface(
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        spacing: 10,
        children: [
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Keypad(onKey: onKey, onRotate: onRotate),
              const SizedBox(width: 12),
              IconButton(
                tooltip: 'Hide controls',
                onPressed: () => onExpandedChanged(false),
                icon: const Icon(Icons.expand_more),
              ),
            ],
          ),
          Wrap(
            alignment: WrapAlignment.center,
            spacing: 8,
            runSpacing: 8,
            children: [
              running
                  ? FilledButton.icon(
                      onPressed: onStop,
                      style: FilledButton.styleFrom(
                        backgroundColor: Colors.redAccent.shade100,
                      ),
                      icon: const Icon(Icons.stop),
                      label: const Text('Stop'),
                    )
                  : FilledButton.icon(
                      onPressed: onStart,
                      icon: const Icon(Icons.play_arrow),
                      label: const Text('Start'),
                    ),
              _toggle(
                label: patternRunning ? 'Stop pattern' : 'Test pattern',
                icon: Icons.gradient,
                selected: patternRunning,
                onPressed: onTogglePattern,
              ),
              _panelButton(BenchPanel.audio, 'Audio', Icons.tune),
              _panelButton(BenchPanel.sensors, 'Sensors', Icons.sensors),
              _panelButton(BenchPanel.metadata, 'Metadata', Icons.info_outline),
              _panelButton(
                BenchPanel.wireless,
                'Wireless',
                controller.wirelessActive ? Icons.wifi : Icons.wifi_off,
              ),
              // Proves that widgets drawn over the projection still receive input,
              // and gives an automated test a click target.
              OutlinedButton(
                onPressed: onHitTest,
                child: Text('Hit test: $hitTestTaps'),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _panelButton(BenchPanel panel, String label, IconData icon) {
    return _toggle(
      label: label,
      icon: icon,
      selected: openPanel == panel,
      onPressed: () => onTogglePanel(panel),
    );
  }

  Widget _toggle({
    required String label,
    required IconData icon,
    required bool selected,
    required VoidCallback onPressed,
  }) {
    return selected
        ? FilledButton.icon(
            onPressed: onPressed,
            icon: Icon(icon),
            label: Text(label),
          )
        : FilledButton.tonalIcon(
            onPressed: onPressed,
            icon: Icon(icon),
            label: Text(label),
          );
  }

  Widget _surface({required EdgeInsets padding, required Widget child}) {
    return Container(
      padding: padding,
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.6),
        borderRadius: BorderRadius.circular(22),
      ),
      child: child,
    );
  }
}
