// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/foundation.dart';

/// Subscribes to the raw PCM the phone is sending, for apps that mix it themselves.
///
/// Nothing is copied out of the core while nobody is listening, so subscribing is what
/// turns the tap on. Pair it with "Play here" off to hear the difference between the
/// plugin playing the audio and the app being handed it.
///
/// Lives on the page rather than in the audio panel, so closing the panel does not
/// quietly turn the tap off.
class PcmTap extends ChangeNotifier {
  final AndroidAutoController _controller;
  StreamSubscription<AndroidAutoAudioBuffer>? _subscription;
  int _bytes = 0;
  double _peak = 0;

  PcmTap(this._controller);

  bool get active => _subscription != null;

  /// Bytes received since the tap was turned on.
  int get bytes => _bytes;

  /// Peak of the latest buffer, 0 to 1, so the meter says something arrived rather
  /// than only that bytes did.
  double get peak => _peak;

  set active(bool on) {
    if (on == active) {
      return;
    }
    if (on) {
      _subscription = _controller.audioBuffers.listen(_onBuffer);
    } else {
      _subscription?.cancel();
      _subscription = null;
      _bytes = 0;
      _peak = 0;
    }
    notifyListeners();
  }

  void _onBuffer(AndroidAutoAudioBuffer buffer) {
    // Signed 16 bit little endian, which is all the protocol sends.
    final samples = buffer.samples.buffer.asInt16List(
      buffer.samples.offsetInBytes,
      buffer.samples.lengthInBytes ~/ 2,
    );
    var peak = 0;
    for (final sample in samples) {
      final magnitude = sample.abs();
      if (magnitude > peak) {
        peak = magnitude;
      }
    }
    _bytes += buffer.samples.lengthInBytes;
    _peak = peak / 32768;
    notifyListeners();
  }

  @override
  void dispose() {
    _subscription?.cancel();
    super.dispose();
  }
}
