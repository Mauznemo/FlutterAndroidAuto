// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/foundation.dart';

/// Feeds the phone a fixed position once a second, standing in for a GPS receiver.
///
/// Once a second because that is what a receiver produces and what the phone expects.
/// A fixed point rather than a simulated drive: feeding a phone a route it is not on
/// makes Maps recalculate all the way through a test, which is noise rather than
/// evidence. What this has to show is that a fix reaches the phone at all.
///
/// A real head unit replaces this with its receiver calling
/// [AndroidAutoController.setLocation]. The obligation is the same either way: location
/// is advertised in `config.dart`, so the phone has stopped using its own receiver and
/// is relying on this.
class LocationFeed extends ChangeNotifier {
  final AndroidAutoController _controller;
  Timer? _timer;
  bool _enabled = true;

  /// A neutral test fix, a city centre with nothing personal about it.
  double latitude = 52.520008;
  double longitude = 13.404954;

  LocationFeed(this._controller);

  bool get enabled => _enabled;

  set enabled(bool value) {
    _enabled = value;
    notifyListeners();
    send();
  }

  void start() {
    _timer ??= Timer.periodic(const Duration(seconds: 1), (_) => send());
    send();
  }

  /// Pushes the current fix, if the feed is on.
  void send() {
    if (!_enabled) {
      return;
    }
    _controller.setLocation(
      AndroidAutoLocation(
        latitude: latitude,
        longitude: longitude,
        accuracyMetres: 5,
        speedMps: 0,
      ),
    );
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }
}
