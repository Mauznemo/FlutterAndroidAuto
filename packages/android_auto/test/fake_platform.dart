// SPDX-License-Identifier: GPL-3.0-or-later
//
// A head unit with no phone, no native library and no texture, standing in for the
// Linux implementation so the widget and controller layers can be tested on their own.

import 'dart:async';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';

/// One call to [AndroidAutoPlatform.sendTouch], kept so a test can read it back.
class RecordedTouch {
  /// The action reported.
  final AndroidAutoTouchAction action;

  /// Every finger that was down, in the order the phone was given them.
  final List<AndroidAutoTouchPoint> pointers;

  /// Which of [pointers] the report was about.
  final int actionIndex;

  /// Records one report.
  const RecordedTouch(this.action, this.pointers, this.actionIndex);

  /// The finger this report is about.
  AndroidAutoTouchPoint get subject => pointers[actionIndex];

  @override
  String toString() =>
      '${action.name}(#$actionIndex of ${pointers.length}) at '
      '${subject.x},${subject.y}';
}

/// A platform implementation that records instead of projecting.
class FakeAndroidAutoPlatform extends AndroidAutoPlatform {
  /// Every touch report the view has sent, oldest first.
  final List<RecordedTouch> touches = [];

  /// Every key press, as `keyName:down` or `keyName:up`.
  final List<String> keys = [];

  /// Rotary steps reported.
  final List<int> rotarySteps = [];

  /// How many times [start] has been called.
  int starts = 0;

  /// How many times [stop] has been called.
  int stops = 0;

  /// The config the last [initialize] was given, if any.
  AndroidAutoConfig? initializedWith;

  /// What [textureId] answers. Set it to pretend video has arrived.
  int? texture;

  /// What [videoInfo] answers.
  AndroidAutoVideoInfo? video;

  final StreamController<AndroidAutoEvent> _events =
      StreamController<AndroidAutoEvent>.broadcast();

  @override
  Stream<AndroidAutoEvent> get events => _events.stream;

  /// Pushes a lifecycle event as the native layer would.
  void emit(AndroidAutoConnectionState state, [String? message]) =>
      _events.add(AndroidAutoEvent(state, message));

  @override
  Future<void> initialize(AndroidAutoConfig config) async {
    initializedWith = config;
  }

  @override
  Future<void> start(AndroidAutoConfig config) async {
    starts++;
  }

  @override
  Future<void> stop() async {
    stops++;
  }

  @override
  Future<int?> get textureId async => texture;

  @override
  Future<AndroidAutoVideoInfo?> get videoInfo async => video;

  @override
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) {
    // Copied, because the view reuses its own list between reports.
    touches.add(
      RecordedTouch(action, List<AndroidAutoTouchPoint>.of(pointers), actionIndex),
    );
  }

  @override
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) {
    keys.add('${key.name}:${down ? "down" : "up"}');
  }

  @override
  void sendRotary(int steps) => rotarySteps.add(steps);

  /// How many times [dispose] has been called.
  int disposals = 0;

  /// Tears the head unit down for good, as the real one does: the controller calls
  /// this from its own dispose.
  @override
  Future<void> dispose() async {
    disposals++;
    await close();
  }

  /// Closes the event stream. Safe to call twice, because a test that disposes its
  /// controller has already been here once.
  Future<void> close() async {
    if (!_events.isClosed) {
      await _events.close();
    }
  }
}
