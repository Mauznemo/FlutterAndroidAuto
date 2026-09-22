// SPDX-License-Identifier: GPL-3.0-or-later
//
// The controller is the whole app facing surface: a ChangeNotifier a host app hands to
// a view and listens to. What is worth pinning down is when it notifies, because a
// widget that does not repaint on a state change is the failure a head unit shows as a
// screen that never updates.

import 'package:android_auto/android_auto.dart';
import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter_test/flutter_test.dart';

import 'fake_platform.dart';

void main() {
  late FakeAndroidAutoPlatform platform;

  setUp(() {
    platform = FakeAndroidAutoPlatform();
    AndroidAutoPlatform.instance = platform;
  });

  tearDown(() => platform.close());

  test('starts idle and projects nothing', () {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    expect(controller.state, AndroidAutoConnectionState.idle);
    expect(controller.textureId, isNull);
    expect(controller.videoInfo, isNull);
    expect(controller.message, isNull);
  });

  test('publishes the Bluetooth service on construction, before any start', () async {
    // Not a detail: a phone that knows this machine as a wireless car asks every five
    // seconds for as long as Bluetooth is connected, and shows a "connecting" notice
    // until something answers. Refusing is what makes it stop.
    const config = AndroidAutoConfig(
      transports: {AndroidAutoTransport.usb, AndroidAutoTransport.wireless},
    );
    final controller = AndroidAutoController(config: config);
    addTearDown(controller.dispose);

    // initialize() is not awaited by the constructor, so let the microtask run.
    await Future<void>.delayed(Duration.zero);

    expect(platform.initializedWith, same(config));
    expect(platform.starts, 0);
  });

  test('start and stop reach the platform once each', () async {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    await controller.start();
    expect(platform.starts, 1);

    await controller.stop();
    expect(platform.stops, 1);
  });

  test('a lifecycle event updates the state and notifies', () async {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);
    var notifications = 0;
    controller.addListener(() => notifications++);

    platform.emit(AndroidAutoConnectionState.searching);
    await Future<void>.delayed(Duration.zero);

    expect(controller.state, AndroidAutoConnectionState.searching);
    expect(notifications, greaterThan(0));
  });

  test('an error event keeps its message', () async {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    platform.emit(AndroidAutoConnectionState.error, 'no phone in accessory mode');
    await Future<void>.delayed(Duration.zero);

    expect(controller.state, AndroidAutoConnectionState.error);
    expect(controller.message, 'no phone in accessory mode');
  });

  test('picks up the texture and the video size when a phone connects', () async {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    platform.texture = 42;
    platform.video = const AndroidAutoVideoInfo(
      width: 1920,
      height: 1080,
      decoder: 'vaapi',
    );
    platform.emit(AndroidAutoConnectionState.connected);
    await Future<void>.delayed(Duration.zero);

    expect(controller.textureId, 42);
    expect(controller.videoInfo?.width, 1920);
    expect(controller.videoInfo?.decoder, 'vaapi');
    expect(controller.videoInfo?.aspectRatio, closeTo(16 / 9, 0.001));
  });

  test('stop clears the texture, so the view falls back to its placeholder', () async {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    platform.texture = 42;
    await controller.start();
    expect(controller.textureId, 42);

    await controller.stop();

    expect(controller.textureId, isNull);
    expect(controller.videoInfo, isNull);
  });

  test('a key press is a down and an up, in that order', () {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    controller.pressKey(AndroidAutoKey.home);

    expect(platform.keys, ['home:down', 'home:up']);
  });

  test('a held key is only what the caller asked for', () {
    // A phone given a down and no up believes the button is still held, so the
    // controller must not helpfully add one.
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    controller.sendKey(AndroidAutoKey.home, down: true);

    expect(platform.keys, ['home:down']);
  });

  test('rotary steps pass straight through, sign and all', () {
    final controller = AndroidAutoController();
    addTearDown(controller.dispose);

    controller.sendRotary(3);
    controller.sendRotary(-1);

    expect(platform.rotarySteps, [3, -1]);
  });

  test('the default config is a 30 fps head unit with the two required sensors', () {
    const config = AndroidAutoConfig();

    // Left to the head unit, which picks a frame that covers the view.
    expect(config.width, isNull);
    expect(config.height, isNull);
    expect(config.matchViewAspectRatio, isTrue);
    expect(config.fps, 30);
    // Night mode and driving status are the two the phone will not finish opening its
    // interface without.
    expect(config.sensors, contains(AndroidAutoSensor.nightMode));
    expect(config.sensors, contains(AndroidAutoSensor.drivingStatus));
    // The cable only. Wireless costs a Bluetooth service and an open port, and no head
    // unit should acquire either by accident.
    expect(config.transports, {AndroidAutoTransport.usb});
    // The three the phone pushes unprompted, not the two the head unit has to drive.
    expect(config.metadata, {
      AndroidAutoMetadata.navigation,
      AndroidAutoMetadata.media,
      AndroidAutoMetadata.phone,
    });
  });

  test('disposing the controller tears the platform down too', () {
    // A stopped session can be started again, a disposed one cannot, and the host app
    // only ever disposes the controller. If this stopped happening the native session
    // would outlive the widget tree that owned it.
    final controller = AndroidAutoController();

    controller.dispose();

    expect(platform.disposals, 1);
  });

  test('a disposed controller stops listening', () async {
    final controller = AndroidAutoController();
    var notifications = 0;
    controller.addListener(() => notifications++);

    // Queued before the dispose and delivered after it. A ChangeNotifier that notified
    // here would throw, because notifying a disposed one is an error.
    platform.emit(AndroidAutoConnectionState.connected);
    controller.dispose();
    await Future<void>.delayed(Duration.zero);

    expect(notifications, 0);
  });
}
