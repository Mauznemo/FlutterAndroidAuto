// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'location_feed.dart';
import 'panel_frame.dart';

/// What the head unit is telling the phone about the car.
///
/// The first two switches are the ones with a visible effect: night mode flips the
/// phone's own theme within a second, and moving locks parts of its interface. Read
/// the subscription line at the bottom before concluding anything, because a value set
/// for a sensor the phone did not subscribe to goes nowhere by design.
class SensorPanel extends StatefulWidget {
  final AndroidAutoController controller;
  final LocationFeed location;
  final VoidCallback onClose;

  const SensorPanel({
    super.key,
    required this.controller,
    required this.location,
    required this.onClose,
  });

  @override
  State<SensorPanel> createState() => _SensorPanelState();
}

class _SensorPanelState extends State<SensorPanel> {
  late final TextEditingController _latitude = TextEditingController(
    text: widget.location.latitude.toString(),
  );
  late final TextEditingController _longitude = TextEditingController(
    text: widget.location.longitude.toString(),
  );

  AndroidAutoController get _controller => widget.controller;

  @override
  void dispose() {
    _latitude.dispose();
    _longitude.dispose();
    super.dispose();
  }

  /// Takes the two fields, if they parse, and sends the new fix straight away rather
  /// than at the next tick.
  void _applyFix() {
    final latitude = double.tryParse(_latitude.text);
    final longitude = double.tryParse(_longitude.text);
    if (latitude == null || longitude == null) {
      return;
    }
    widget.location
      ..latitude = latitude
      ..longitude = longitude
      ..send();
  }

  @override
  Widget build(BuildContext context) {
    final subscribed = _controller.sensorSubscriptions;
    return PanelFrame(
      title: 'Sensors',
      icon: Icons.sensors,
      onClose: widget.onClose,
      children: [
        PanelSwitch(
          title: 'Night mode',
          subtitle: "Flips the phone's own light and dark theme",
          value: _controller.nightMode,
          onChanged: _controller.setNightMode,
        ),
        PanelSwitch(
          title: 'Moving',
          subtitle: 'Locks the keyboard, settings and video on the phone',
          value: _controller.drivingRestrictions.isNotEmpty,
          onChanged: (moving) => _controller.setParked(!moving),
        ),
        ListenableBuilder(
          listenable: widget.location,
          builder: (context, _) => PanelSwitch(
            title: 'Feed GPS',
            subtitle: 'A fix a second. The phone stops using its own receiver',
            value: widget.location.enabled,
            onChanged: (value) => widget.location.enabled = value,
          ),
        ),
        Row(
          spacing: 10,
          children: [
            Expanded(child: _coordinate(_latitude, 'Latitude')),
            Expanded(child: _coordinate(_longitude, 'Longitude')),
          ],
        ),
        const PanelSection('What the phone sees'),
        PanelNote(
          'Advertised: ${_controller.config.sensors.map((s) => s.name).join(", ")}',
        ),
        PanelNote(
          'Subscribed: '
          '${subscribed.isEmpty ? "none" : subscribed.map((s) => s.name).join(", ")}',
        ),
        PanelNote('${_controller.sensorBatches} readings sent'),
      ],
    );
  }

  Widget _coordinate(TextEditingController controller, String label) {
    return TextField(
      controller: controller,
      style: const TextStyle(fontSize: 12),
      keyboardType: const TextInputType.numberWithOptions(
        decimal: true,
        signed: true,
      ),
      decoration: InputDecoration(labelText: label, isDense: true),
      onSubmitted: (_) => _applyFix(),
    );
  }
}
