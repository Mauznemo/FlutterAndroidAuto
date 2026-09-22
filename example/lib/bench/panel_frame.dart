// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:flutter/material.dart';

/// The test bench panels, one open at a time.
enum BenchPanel { audio, sensors, metadata, wireless }

/// What every bench panel is drawn in: a title, a close button, and a body that
/// scrolls when the window is too short to show it whole.
class PanelFrame extends StatelessWidget {
  final String title;
  final IconData icon;
  final VoidCallback onClose;
  final List<Widget> children;

  const PanelFrame({
    super.key,
    required this.title,
    required this.icon,
    required this.onClose,
    required this.children,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF12161C).withValues(alpha: 0.92),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: Colors.white10),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 6, 6, 0),
            child: Row(
              spacing: 10,
              children: [
                Icon(icon, size: 18, color: Colors.white70),
                Expanded(
                  child: Text(
                    title,
                    style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 15),
                  ),
                ),
                IconButton(
                  tooltip: 'Close',
                  onPressed: onClose,
                  icon: const Icon(Icons.close, size: 18),
                ),
              ],
            ),
          ),
          Flexible(
            child: SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 14),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: children,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// A small heading inside a panel.
class PanelSection extends StatelessWidget {
  final String label;

  const PanelSection(this.label, {super.key});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 12, bottom: 4),
      child: Text(
        label.toUpperCase(),
        style: const TextStyle(
          fontSize: 11,
          letterSpacing: 0.8,
          color: Colors.white54,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}

/// A line of diagnostic text, the numbers worth reading before concluding anything.
class PanelNote extends StatelessWidget {
  final String text;
  final Color color;

  const PanelNote(this.text, {super.key, this.color = Colors.white54});

  @override
  Widget build(BuildContext context) {
    return Text(text, style: TextStyle(fontSize: 11, color: color));
  }
}

/// A dense switch with a title and an explanation, the shape most bench toggles take.
class PanelSwitch extends StatelessWidget {
  final String title;
  final String subtitle;
  final bool value;
  final ValueChanged<bool> onChanged;

  const PanelSwitch({
    super.key,
    required this.title,
    required this.subtitle,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return SwitchListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      title: Text(title, style: const TextStyle(fontSize: 13)),
      subtitle: Text(subtitle, style: const TextStyle(fontSize: 11)),
      value: value,
      onChanged: onChanged,
    );
  }
}
