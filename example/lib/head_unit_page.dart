// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';
import 'dart:io';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'bench/audio_panel.dart';
import 'bench/dock.dart';
import 'bench/location_feed.dart';
import 'bench/metadata_panel.dart';
import 'bench/pcm_tap.dart';
import 'bench/sensor_panel.dart';
import 'bench/wireless_panel.dart';
import 'config.dart';
import 'head_unit/metadata_overlay.dart';
import 'head_unit/projection_placeholder.dart';
import 'head_unit/status_bar.dart';

/// The whole head unit: the projection, and Flutter drawn over it.
///
/// Deliberately shaped like the real thing: a status bar of the head unit's own above
/// the projection, and every other piece of chrome an ordinary Flutter widget stacked
/// on top of it. If overlaying ever stops working, this app breaks immediately rather
/// than at the end of a project.
///
/// Touches that land on the chrome go to the chrome, and everywhere else they go
/// through to [AndroidAutoView], which forwards them to the phone. Nothing in here has
/// to arrange that: it is ordinary Flutter hit testing.
class HeadUnitPage extends StatefulWidget {
  const HeadUnitPage({super.key});

  @override
  State<HeadUnitPage> createState() => _HeadUnitPageState();
}

class _HeadUnitPageState extends State<HeadUnitPage> {
  final AndroidAutoController _controller = AndroidAutoController(
    config: headUnitConfig(),
  );
  late final LocationFeed _location = LocationFeed(_controller);
  late final PcmTap _pcmTap = PcmTap(_controller);

  /// Remembered here rather than in the panel, so that closing the panel does not
  /// forget what was typed into it.
  late AndroidAutoWirelessConfig _wirelessConfig =
      _controller.config.wireless ?? const AndroidAutoWirelessConfig();

  BenchPanel? _openPanel;
  bool _dockExpanded = true;
  bool _patternRunning = false;
  String _lastInput = 'none';
  int _hitTestTaps = 0;

  /// Notifications are events rather than state, so nothing keeps the last one unless
  /// the app does.
  AndroidAutoNotification? _lastNotification;
  StreamSubscription<AndroidAutoNotification>? _notifications;

  @override
  void initState() {
    super.initState();
    _controller.addListener(_onChanged);
    _notifications = _controller.notifications.listen(
      (notification) => setState(() => _lastNotification = notification),
    );
    // Before there is a phone on purpose: the value is remembered across sessions, and
    // setting it early is the case worth exercising.
    _location.start();
    // Pressing Start is one click too many when the machine running this has no
    // network for anything else and the test is being driven from a script. An example
    // app knob, not a plugin one.
    if (Platform.environment['AA_AUTOSTART'] == '1') {
      WidgetsBinding.instance.addPostFrameCallback((_) => _controller.start());
    }
  }

  void _onChanged() => setState(() {});

  @override
  void dispose() {
    _notifications?.cancel();
    _location.dispose();
    _pcmTap.dispose();
    _controller
      ..removeListener(_onChanged)
      ..dispose();
    super.dispose();
  }

  Future<void> _start() async {
    await _controller.start();
  }

  Future<void> _stop() async {
    await _controller.stop();
    setState(() => _patternRunning = false);
  }

  Future<void> _togglePattern() async {
    if (_patternRunning) {
      await _controller.stopTestPattern();
    } else {
      if (_controller.state == AndroidAutoConnectionState.idle) {
        await _controller.start();
      }
      await _controller.startTestPattern();
    }
    setState(() => _patternRunning = !_patternRunning);
  }

  void _togglePanel(BenchPanel panel) {
    setState(() => _openPanel = _openPanel == panel ? null : panel);
  }

  void _press(AndroidAutoKey key) {
    _controller.pressKey(key);
    setState(() => _lastInput = 'key ${key.name}');
  }

  void _rotate(int steps) {
    _controller.sendRotary(steps);
    setState(() => _lastInput = 'rotary $steps');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      // The status bar sits above the projection rather than over it, the way a real
      // head unit's own bar would, so the view below it is not 16:9 and the phone lays
      // itself out for what is left. Everything else is drawn over the projection, and
      // none of it changes the view's size: opening a panel or hiding the dock must not
      // make the phone lay out again, which freezes its video for a second.
      body: Column(
        children: [
          StatusBar(controller: _controller, lastInput: _lastInput),
          Expanded(
            child: Stack(
              fit: StackFit.expand,
              children: [
                AndroidAutoView(
                  controller: _controller,
                  placeholder: ProjectionPlaceholder(state: _controller.state),
                ),
                Column(
                  children: [
                    Expanded(
                      child: Padding(
                        padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
                        child: Stack(
                          children: [
                            // The point of the metadata channels, drawn as ordinary
                            // Flutter widgets over the projection rather than read off
                            // its pixels.
                            Align(
                              alignment: Alignment.bottomLeft,
                              child: SizedBox(
                                width: 420,
                                child: MetadataOverlay(controller: _controller),
                              ),
                            ),
                            if (_openPanel != null)
                              Align(
                                alignment: Alignment.topRight,
                                child: SizedBox(
                                  width: 400,
                                  child: _panel(_openPanel!),
                                ),
                              ),
                          ],
                        ),
                      ),
                    ),
                    Dock(
                      controller: _controller,
                      expanded: _dockExpanded,
                      openPanel: _openPanel,
                      patternRunning: _patternRunning,
                      hitTestTaps: _hitTestTaps,
                      onExpandedChanged: (value) =>
                          setState(() => _dockExpanded = value),
                      onStart: _start,
                      onStop: _stop,
                      onTogglePattern: _togglePattern,
                      onTogglePanel: _togglePanel,
                      onKey: _press,
                      onRotate: _rotate,
                      onHitTest: () => setState(() => _hitTestTaps++),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _panel(BenchPanel panel) {
    void onClose() => setState(() => _openPanel = null);
    switch (panel) {
      case BenchPanel.audio:
        return AudioPanel(
          controller: _controller,
          pcmTap: _pcmTap,
          onClose: onClose,
        );
      case BenchPanel.sensors:
        return SensorPanel(
          controller: _controller,
          location: _location,
          onClose: onClose,
        );
      case BenchPanel.metadata:
        return MetadataPanel(
          controller: _controller,
          lastNotification: _lastNotification,
          onClose: onClose,
        );
      case BenchPanel.wireless:
        return WirelessPanel(
          controller: _controller,
          config: _wirelessConfig,
          onConfigChanged: (config) => _wirelessConfig = config,
          onClose: onClose,
        );
    }
  }
}
